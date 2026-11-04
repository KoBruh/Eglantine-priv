// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <qdebug.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>
#include "core/core.h"
#include "core/cpu_manager.h"
#include "emu_thread.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Threads given to EmuThread::Abandon. Never deleted: they may still be running when the process
// exits. Only the UI thread touches this list.
std::vector<EmuThread*>& AbandonedThreads() {
    static std::vector<EmuThread*> threads;
    return threads;
}

} // Anonymous namespace

EmuThread::EmuThread() {}

EmuThread::~EmuThread() = default;

void EmuThread::Abandon(std::unique_ptr<EmuThread> thread) {
    if (!thread) {
        return;
    }
    thread->disconnect();
    AbandonedThreads().push_back(thread.release());
}

bool EmuThread::HasAbandonedThread() {
    return !AbandonedThreads().empty();
}

void EmuThread::ExitProcessNow(int exit_code) {
#ifdef _WIN32
    // ExitProcess, which _Exit ends in, still runs every DLL's detach code (the GPU driver's
    // among them) after killing the other threads, and that can wait forever on a lock one of
    // them held. TerminateProcess ends the process without running any of it.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
#endif
    std::_Exit(exit_code);
}

void EmuThread::run() {
    Common::SetCurrentThreadName("EmuControlThread");

    auto& gpu = QtCommon::system->GPU();
    auto stop_token = m_stop_source.get_token();

    QtCommon::system->RegisterHostThread();

    // Main process has been loaded. Make the context current to this thread and begin GPU and CPU
    // execution.
    gpu.ObtainContext();

    const auto cache_started = std::chrono::steady_clock::now();
    LOG_INFO(Frontend, "Disk shader resource loading started");
    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        QtCommon::system->Renderer().ReadRasterizer()->LoadDiskResources(
            QtCommon::system->GetApplicationProcessProgramID(), stop_token,
            [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
                emit LoadProgress(stage, value, total);
            });
    }
    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);
    LOG_INFO(Frontend, "Disk shader resource loading completed: elapsed_ms={} cancelled={}",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - cache_started).count(),
             stop_token.stop_requested());

    gpu.ReleaseContext();
    gpu.Start();

    QtCommon::system->GetCpuManager().OnGpuReady();

    if (QtCommon::system->DebuggerEnabled()) {
        QtCommon::system->InitializeDebugger();
    }

    while (!stop_token.stop_requested()) {
        std::unique_lock lk{m_should_run_mutex};
        if (m_should_run) {
            QtCommon::system->Run();
            m_stopped.Reset();

            m_should_run_cv.wait(lk, stop_token, [&] {
                return !m_should_run || m_pending_shader_cache_title.has_value();
            });

            if (m_should_run && m_pending_shader_cache_title.has_value()) {
                const u64 program_id = *m_pending_shader_cache_title;
                m_pending_shader_cache_title.reset();

                lk.unlock();
                this->ReloadDiskShaderCache(program_id);
                lk.lock();
            }
        } else {
            QtCommon::system->Pause();
            m_stopped.Set();

            EmulationPaused(lk);
            m_should_run_cv.wait(lk, stop_token, [&] { return m_should_run; });
            EmulationResumed(lk);
        }
    }

    // EDEN_NX2_HANG_ON_STOP=1 keeps this thread from ever stopping, to check how the frontend
    // copes with an emulation thread that does not stop.
    if (const char* const hang = std::getenv("EDEN_NX2_HANG_ON_STOP"); hang && *hang == '1') {
        LOG_WARNING(Frontend, "EDEN_NX2_HANG_ON_STOP: the emulation thread will not stop");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours{1});
        }
    }

    // Shutdown the main emulated process
    LOG_INFO(Frontend, "Emulation thread entering core shutdown");
    Common::Log::Flush();
    QtCommon::system->DetachDebugger();
    QtCommon::system->ShutdownMainProcess();
}

void EmuThread::ReloadDiskShaderCache(u64 program_id) {
    if (!Settings::values.use_disk_shader_cache.GetValue()) {
        return;
    }

    LOG_INFO(Frontend, "Reloading disk shader cache for {:016X}", program_id);

    auto& system = *QtCommon::system;
    auto& gpu = system.GPU();

    system.Pause();
    gpu.WaitForIdle();
    gpu.ObtainContext();

    emit ShaderCacheReloadStarted();
    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
    system.Renderer().ReadRasterizer()->LoadDiskResources(program_id, m_stop_source.get_token(),
        [this](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
            emit LoadProgress(stage, value, total);
        });
    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);
    emit ShaderCacheReloadFinished();

    gpu.ReleaseContext();
}

// Unlock while emitting signals so that the main thread can
// continue pumping events.

void EmuThread::EmulationPaused(std::unique_lock<std::mutex>& lk) {
    lk.unlock();
    emit DebugModeEntered();
    lk.lock();
}

void EmuThread::EmulationResumed(std::unique_lock<std::mutex>& lk) {
    lk.unlock();
    emit DebugModeLeft();
    lk.lock();
}
