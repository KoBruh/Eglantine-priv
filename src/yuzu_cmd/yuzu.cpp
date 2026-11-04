// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <thread>
#include <atomic>
#include <iterator>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include "common/settings_enums.h"
#include "core/launch_params.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include <fmt/ostream.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/main.h"
#include "network/network.h"
#include "sdl_config.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"
#ifdef HAS_OPENGL
#include "yuzu_cmd/emu_window/emu_window_sdl3_gl.h"
#endif
#include "yuzu_cmd/emu_window/emu_window_sdl3_null.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_vk.h"
#include "yuzu_cmd/nx2_sampler.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include "common/windows/timer_resolution.h"
#endif

#ifdef _WIN32
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// EDEN_NX2_CRASH_DUMP=<dir>: on an unhandled exception (a real crash; fastmem faults are handled
// before this), write <dir>/eden-crash-<pid>.dmp. A debugger changes the timing enough to hide
// some crashes, so this catches them at full speed. dbghelp.dll is loaded only then.
// Prints a code address as module+offset (resolve with llvm-symbolizer and the module's PDB).
static void PrintCodeAddress(const char* label, const void* address) {
    HMODULE module{};
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, name, sizeof(name));
    }
    const char* base_name = std::strrchr(name, '\\');
    std::fprintf(stderr, "  %s %p %s+%#llx\n", label, address, base_name ? base_name + 1 : name,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                 reinterpret_cast<uintptr_t>(module)));
}

static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* exception) {
    static std::atomic<bool> written{};
    const char* directory = std::getenv("EDEN_NX2_CRASH_DUMP");
    if (directory == nullptr || written.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The faulting address and stack first: they survive even if the dump fails.
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    std::fprintf(stderr, "Unhandled exception %#lx on thread %lu, info %#llx %#llx\n",
                 record.ExceptionCode, GetCurrentThreadId(),
                 static_cast<unsigned long long>(record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0),
                 static_cast<unsigned long long>(record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0));
    PrintCodeAddress("at", record.ExceptionAddress);
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        PrintCodeAddress("frame", frames[i]);
    }
    std::fflush(stderr);
    struct ExceptionInformation {
        DWORD thread_id;
        EXCEPTION_POINTERS* exception_pointers;
        BOOL client_pointers;
    };
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, const ExceptionInformation*,
                                              const void*, const void*);
    const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                                     GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                               : nullptr;
    if (write == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\eden-crash-%lu.dmp", directory, GetCurrentProcessId());
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const ExceptionInformation information{GetCurrentThreadId(), exception, FALSE};
        // MiniDumpWithThreadInfo; the larger MiniDumpWithIndirectlyReferencedMemory failed on
        // Eden's address space and left an empty file.
        const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1000,
                              &information, nullptr, nullptr);
        const DWORD error = ok ? 0 : GetLastError();
        CloseHandle(file);
        std::fprintf(stderr, ok ? "Crash dump written to %s\n" : "Crash dump to %s failed (%#lx)\n",
                     path, error);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// With EDEN_NX2_CRASH_DUMP set, also report each C++ exception when it is thrown: its type,
// what() for a std::exception, and the stack symbolized from the PDBs. An exception that escapes
// a thread ends the process through std::terminate without reaching WriteCrashDump.
static LONG WINAPI ReportCxxThrow(EXCEPTION_POINTERS* exception) {
    constexpr DWORD MsvcCxxException = 0xe06d7363;
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    if (record.ExceptionCode != MsvcCxxException || record.NumberParameters < 4) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<int> reported{};
    if (reported.fetch_add(1) >= 32) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    // x64 throw metadata is image-relative: ThrowInfo {attributes, unwind, forward_compat,
    // catchable_types}; CatchableType {properties, type, mdisp, pdisp, vdisp, size, copy};
    // TypeDescriptor {vftable, spare, name}.
    const auto base = static_cast<uintptr_t>(record.ExceptionInformation[3]);
    const auto* object = reinterpret_cast<const u8*>(record.ExceptionInformation[1]);
    const auto* throw_info = reinterpret_cast<const s32*>(record.ExceptionInformation[2]);
    std::fprintf(stderr, "C++ exception thrown on thread %lu:", GetCurrentThreadId());
    const char* message = nullptr;
    if (throw_info != nullptr && base != 0) {
        const auto* types = reinterpret_cast<const s32*>(base + throw_info[3]);
        for (s32 i = 0; i < types[0]; ++i) {
            const auto* type = reinterpret_cast<const s32*>(base + types[1 + i]);
            const char* name = reinterpret_cast<const char*>(base + type[1]) + 2 * sizeof(void*);
            std::fprintf(stderr, " %s", name);
            if (std::strcmp(name, ".?AVexception@std@@") == 0 && object != nullptr) {
                message = reinterpret_cast<const std::exception*>(object + type[2])->what();
            }
        }
    }
    std::fprintf(stderr, "\n  what(): %s\n", message ? message : "(not a std::exception)");
    using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using SymGetLineFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
    static const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    static const auto sym_from_addr =
        dbghelp ? reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr")) : nullptr;
    static const auto sym_get_line = dbghelp ? reinterpret_cast<SymGetLineFn>(
                                                   GetProcAddress(dbghelp, "SymGetLineFromAddr64"))
                                             : nullptr;
    static const bool symbols = [] {
        if (dbghelp == nullptr) return false;
        const auto set_options = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
        const auto initialize = reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
        if (set_options == nullptr || initialize == nullptr) return false;
        set_options(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        return initialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    void* frames[24];
    const USHORT count = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const auto address = reinterpret_cast<DWORD64>(frames[i]);
        if (symbols && sym_from_addr && sym_from_addr(GetCurrentProcess(), address, &displacement, symbol)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_displacement = 0;
            if (sym_get_line && sym_get_line(GetCurrentProcess(), address, &line_displacement, &line)) {
                std::fprintf(stderr, "  #%u %s (%s:%lu)\n", i, symbol->Name, line.FileName, line.LineNumber);
            } else {
                std::fprintf(stderr, "  #%u %s+%#llx\n", i, symbol->Name,
                             static_cast<unsigned long long>(displacement));
            }
        } else {
            PrintCodeAddress("frame", frames[i]);
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#ifdef _WIN32
// EDEN_NX2_HANG_DUMP=<seconds>: if no frame is presented for that long, write every thread's
// stack to stderr once (a hang's who-waits-on-whom). Each thread's chain is captured while it is
// suspended with RtlVirtualUnwind into preallocated arrays only (a suspended thread may hold the
// heap lock); symbols are looked up after all threads are running again.
static void DumpAllThreadStacks(u64 presents) {
    struct Captured {
        DWORD id;
        std::wstring name;
        std::array<DWORD64, 32> pcs{};
        size_t count{};
    };
    std::vector<Captured> threads;
    threads.reserve(512);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() &&
            entry.th32ThreadID != GetCurrentThreadId() && threads.size() < threads.capacity()) {
            threads.push_back(Captured{entry.th32ThreadID});
        }
    }
    CloseHandle(snapshot);
    for (auto& thread : threads) {
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, thread.id);
        if (handle == nullptr) {
            continue;
        }
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(handle, &description)) && description != nullptr) {
            thread.name = description;
            LocalFree(description);
        }
        if (SuspendThread(handle) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(handle, &context)) {
                for (; thread.count < thread.pcs.size() && context.Rip != 0; ++thread.count) {
                    thread.pcs[thread.count] = context.Rip;
                    DWORD64 image_base = 0;
                    auto* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
                    if (function == nullptr) {
                        // A leaf (or code without unwind data, such as JIT output): return address
                        // at the top of the stack.
                        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                        context.Rsp += 8;
                    } else {
                        PVOID handler_data = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                                         &handler_data, &establisher, nullptr);
                    }
                }
            }
            ResumeThread(handle);
        }
        CloseHandle(handle);
    }
    using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto sym_from_addr =
        dbghelp ? reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr")) : nullptr;
    if (dbghelp != nullptr) {
        const auto set_options = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
        const auto initialize = reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
        if (set_options && initialize) {
            set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            initialize(GetCurrentProcess(), nullptr, TRUE);
        }
    }
    std::fprintf(stderr, "Hang: no frame presented for %s s (presents so far %llu); thread stacks:\n",
                 std::getenv("EDEN_NX2_HANG_DUMP"), static_cast<unsigned long long>(presents));
    for (const auto& thread : threads) {
        std::fprintf(stderr, "thread %lu \"%ls\"\n", thread.id, thread.name.c_str());
        for (size_t i = 0; i < thread.count; ++i) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (sym_from_addr && sym_from_addr(GetCurrentProcess(), thread.pcs[i], &displacement, symbol)) {
                std::fprintf(stderr, "  #%zu %s+%#llx\n", i, symbol->Name,
                             static_cast<unsigned long long>(displacement));
            } else {
                std::fprintf(stderr, "  #%zu %#llx\n", i, static_cast<unsigned long long>(thread.pcs[i]));
            }
        }
    }
    std::fflush(stderr);
}

static void StartHangWatchdog() {
    const char* text = std::getenv("EDEN_NX2_HANG_DUMP");
    const long long seconds = text ? std::strtoll(text, nullptr, 10) : 0;
    if (seconds <= 0) {
        return;
    }
    std::thread([seconds] {
        u64 last = ~u64{};
        auto since = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
            const u64 now = Core::g_nx2_present_count.load(std::memory_order_relaxed);
            if (now != last) {
                last = now;
                since = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - since >= std::chrono::seconds{seconds}) {
                DumpAllThreadStacks(now);
                return;
            }
        }
    }).detach();
}

// abort() and std::terminate end the process with a fail-fast that skips the exception filter:
// with EDEN_NX2_CRASH_DUMP set, print the uncaught exception (if any) and the stack first.
static void PrintAbortStack(const char* reason) {
    std::fprintf(stderr, "%s on thread %lu\n", reason, GetCurrentThreadId());
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        PrintCodeAddress("frame", frames[i]);
    }
    std::fflush(stderr);
}

static void InstallAbortReporting() {
    if (std::getenv("EDEN_NX2_CRASH_DUMP") == nullptr) {
        return;
    }
    std::set_terminate([] {
        if (const std::exception_ptr current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "Uncaught exception: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "Uncaught non-standard exception\n");
            }
        }
        PrintAbortStack("std::terminate");
        std::_Exit(3);
    });
    std::signal(SIGABRT, [](int) {
        PrintAbortStack("abort()");
        std::_Exit(3);
    });
}
#endif

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::IpCollision:
        LOG_ERROR(Network, "You tried to use the same fake IP-Address as another user that is "
                           "connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    case Network::RoomMember::Error::UnknownError:
        LOG_ERROR(Network, "UnknownError");
        break;
    case Network::RoomMember::Error::PermissionDenied:
        LOG_ERROR(Network, "PermissionDenied");
        break;
    case Network::RoomMember::Error::NoSuchUser:
        LOG_ERROR(Network, "NoSuchUser");
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message = [&]() {
        switch (msg.type) {
        case Network::IdMemberJoin:
            return fmt::format("{} has joined", msg.nickname);
        case Network::IdMemberLeave:
            return fmt::format("{} has left", msg.nickname);
        case Network::IdMemberKicked:
            return fmt::format("{} has been kicked", msg.nickname);
        case Network::IdMemberBanned:
            return fmt::format("{} has been banned", msg.nickname);
        case Network::IdAddressUnbanned:
            return fmt::format("{} has been unbanned", msg.nickname);
        default:
            return std::string{};
        }
    }();
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

struct SdlState {
    Core::System system{};
    // Must outlive emu_window, which keeps a pointer to it.
    InputCommon::InputSubsystem input_subsystem{};
    std::unique_ptr<EmuWindow_SDL3> emu_window;
    bool network_initialized{};
};

static std::atomic_bool pack_nup_cancel{false};
static std::atomic_bool unpack_nup_cancel{false};
static std::atomic_bool edit_nup_cancel{false};

// eden-cli --pack-nup <dump> [<out.nup>]; the output defaults to <dump>.nup next to the dump.
static bool PackNup(const std::string& source, const std::string& output) {
    const auto to_path = [](const std::string& utf8) {
        return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
    };
    const auto dump_root = to_path(source);
    const auto archive =
        output.empty() ? FileSys::Nup::DefaultArchivePath(dump_root) : to_path(output);
    std::signal(SIGINT, [](int) { pack_nup_cancel = true; });

    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    std::string error;
    const auto result = FileSys::Nup::PackDump(
        dump_root, archive, {},
        [&](const FileSys::Nup::PackProgress& p) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report < std::chrono::seconds(1) && p.files_done != p.files_total) {
                return;
            }
            last_report = now;
            fmt::print(stderr, "\r{} / {} MiB, {} / {} files", p.bytes_done >> 20,
                       p.bytes_total >> 20, p.files_done, p.files_total);
        },
        &pack_nup_cancel, &error);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fmt::print(stderr, "\n");
    switch (result) {
    case FileSys::Nup::PackResult::Success:
        fmt::print(stderr, "Wrote {} in {:.1f} s\n", archive.string(), seconds);
        return true;
    case FileSys::Nup::PackResult::Cancelled:
        fmt::print(stderr, "Cancelled\n");
        return false;
    default:
        fmt::print(stderr, "Packing failed: {}\n", error);
        return false;
    }
}

// eden-cli --unpack-nup <file.nup> [<out_dir>]; the output defaults to <name>/ next to the NUP.
static bool UnpackNup(const std::string& source, const std::string& output) {
    const auto to_path = [](const std::string& utf8) {
        return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
    };
    const auto archive = to_path(source);
    const auto out_dir =
        output.empty() ? FileSys::Nup::DefaultExtractPath(archive) : to_path(output);
    std::signal(SIGINT, [](int) { unpack_nup_cancel = true; });

    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    std::string error;
    const auto result = FileSys::Nup::ExtractNup(
        archive, out_dir,
        [&](const FileSys::Nup::PackProgress& p) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report < std::chrono::seconds(1) && p.files_done != p.files_total) {
                return;
            }
            last_report = now;
            fmt::print(stderr, "\r{} / {} MiB, {} / {} files", p.bytes_done >> 20,
                       p.bytes_total >> 20, p.files_done, p.files_total);
        },
        &unpack_nup_cancel, &error);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fmt::print(stderr, "\n");
    switch (result) {
    case FileSys::Nup::ExtractResult::Success:
        // ExtractNup checks every file against its stored SHA-256 as it writes it.
        fmt::print(stderr, "Extracted to {} in {:.1f} s, every file verified\n", out_dir.string(),
                   seconds);
        return true;
    case FileSys::Nup::ExtractResult::Cancelled:
        fmt::print(stderr, "Cancelled\n");
        return false;
    default:
        fmt::print(stderr, "Extraction failed: {}\n", error);
        return false;
    }
}

static bool EditNup(const Core::LaunchParams& lp) {
    const auto to_path = [](const std::string& utf8) {
        return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
    };
    const auto archive = to_path(*lp.edit_nup_source);
    FileSys::Nup::MetadataEdit edit;
    if (lp.edit_nup_name) {
        edit.title_name = *lp.edit_nup_name;
    }
    if (lp.edit_nup_publisher) {
        edit.publisher = *lp.edit_nup_publisher;
    }
    if (lp.edit_nup_version) {
        edit.display_version = *lp.edit_nup_version;
    }
    if (lp.edit_nup_icon) {
        if (lp.edit_nup_icon->empty()) {
            edit.icon = std::vector<u8>{};
        } else {
            const auto icon_path = to_path(*lp.edit_nup_icon);
            std::ifstream in(icon_path, std::ios::binary);
            if (!in) {
                fmt::print(stderr, "Cannot read icon {}\n", icon_path.string());
                return false;
            }
            edit.icon = std::vector<u8>((std::istreambuf_iterator<char>(in)), {});
            if (edit.icon->empty()) {
                fmt::print(stderr, "Icon file is empty\n");
                return false;
            }
        }
    }
    if (!edit.title_name && !edit.icon && !edit.display_version && !edit.publisher) {
        fmt::print(stderr, "Nothing to edit: pass --nup-name, --nup-icon, --nup-version or "
                           "--nup-publisher (or a --clear-* flag)\n");
        return false;
    }
    std::signal(SIGINT, [](int) { edit_nup_cancel = true; });
    std::string error;
    switch (FileSys::Nup::EditNupMetadata(archive, edit, &error, &edit_nup_cancel)) {
    case FileSys::Nup::EditResult::Success:
        fmt::print(stderr, "Updated {}\n", archive.string());
        return true;
    case FileSys::Nup::EditResult::NoChange:
        fmt::print(stderr, "No changes to apply\n");
        return true;
    case FileSys::Nup::EditResult::Cancelled:
        fmt::print(stderr, "Cancelled\n");
        return false;
    default:
        fmt::print(stderr, "Edit failed: {}\n", error);
        return false;
    }
}

extern "C" SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(WriteCrashDump);
    if (std::getenv("EDEN_NX2_CRASH_DUMP") != nullptr) {
        AddVectoredExceptionHandler(1, ReportCxxThrow);
    }
    StartHangWatchdog();
    InstallAbortReporting();
#endif
    Nx2Sampler::StartFromEnvironment();
    // SDL calls SDL_AppIterate back to back by default, which kept a host core at 100% polling
    // input; 240 Hz is plenty for input and the key script.
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "240");
    SdlState* state = new SdlState();
    *appstate = state;

#ifdef _WIN32
    // A GUI-subsystem program has no console of its own, so borrow the terminal's for the log. A
    // stream the parent redirected (eden-cli > log.txt, tools/nx2/run_scenario.py's log files) keeps
    // its redirection: reopening it on CONOUT$ would send the log to the parent's console instead.
    const auto redirected = [](DWORD which) {
        const HANDLE handle = GetStdHandle(which);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        const DWORD type = GetFileType(handle);
        return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
    };
    const bool out_redirected = redirected(STD_OUTPUT_HANDLE);
    const bool err_redirected = redirected(STD_ERROR_HANDLE);
    if (!(out_redirected && err_redirected) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!out_redirected) {
            freopen("CONOUT$", "wb", stdout);
        }
        if (!err_redirected) {
            freopen("CONOUT$", "wb", stderr);
        }
    }
    // Raise the CRT's open-file limit like the Qt frontend: loose RomFS folders keep thousands of
    // files open through the VFS cache.
    _setmaxstdio(8192);
#elif defined(__unix__) || defined(__APPLE__)
    // Same for the open file limit (macOS shells default to 256).
    if (struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = std::min<rlim_t>(8192, rl.rlim_max);
        setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();

    int option_index = 0;
#ifdef _WIN32
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        return SDL_APP_FAILURE;
    }
#else
    wchar_t **argv_w = nullptr;
#endif
    Core::LaunchParams lp = Core::ParseLaunchParams(state->system, argc, argv, argv_w);
#ifdef _WIN32
    LocalFree(argv_w);
#endif
    SdlConfig config{lp.config_path};

    Core::ApplyLaunchParams(lp);

    // Informational options already printed by ApplyLaunchParams; handle them before any
    // archive operation so --help/--version never packs, unpacks or edits.
    if (lp.print_help || lp.print_version) {
        return SDL_APP_SUCCESS;
    }

    if (lp.pack_nup_source) {
        return PackNup(*lp.pack_nup_source, lp.filepath) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    if (lp.unpack_nup_source) {
        return UnpackNup(*lp.unpack_nup_source, lp.filepath) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    if (lp.edit_nup_source) {
        return EditNup(lp) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    if (lp.filepath.empty()) {
        LOG_CRITICAL(Frontend, "Failed to load ROM: No ROM specified");
        return SDL_APP_FAILURE;
    }

    state->network_initialized = Network::Init();
    if (!state->network_initialized) {
        LOG_CRITICAL(Network, "Failed to initialize room networking");
        return SDL_APP_FAILURE;
    }

    state->system.Initialize();

    // Apply the command line arguments
    state->system.ApplySettings();

    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        state->emu_window = std::make_unique<EmuWindow_SDL3_GL>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
#endif
    case Settings::RendererBackend::Vulkan:
        state->emu_window = std::make_unique<EmuWindow_SDL3_VK>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
    case Settings::RendererBackend::Null:
        state->emu_window = std::make_unique<EmuWindow_SDL3_Null>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
    default:
        LOG_CRITICAL(Frontend, "Invalid renderer backend");
        return SDL_APP_FAILURE;
    }

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    state->system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    state->system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    state->system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    state->system.GetFileSystemController().CreateFactories(*state->system.GetFilesystem());
    state->system.GetUserChannel().clear();

    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
    };
    const Core::SystemResultStatus load_result = state->system.Load(*state->emu_window, lp.filepath, load_parameters);
    switch (load_result) {
    case Core::SystemResultStatus::Success:
        break; // Expected case
    case Core::SystemResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", lp.filepath);
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorVideoCore:
        LOG_CRITICAL(Frontend, "Failed to initialize VideoCore!");
        return SDL_APP_FAILURE;
    default:
        const u16 loader_id = u16(Core::SystemResultStatus::ErrorLoader);
        const u16 error_id = u16(load_result) - loader_id;
        LOG_CRITICAL(Frontend,
            "While attempting to load the ROM requested, an error occurred. Please "
            "refer to the Eden wiki for more information or the Eden discord for "
            "additional help.\n\nError Code: {:04X}-{:04X}\nError Description: {}",
            loader_id, error_id, Loader::ResultStatus(error_id));
        return SDL_APP_FAILURE;
    }

    if (lp.use_multiplayer) {
        if (auto member = Network::GetRoomMember().lock()) {
            member->BindOnChatMessageReceived(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", lp.address, lp.port, lp.nickname);
            member->Join(lp.nickname, lp.address.c_str(), lp.port, 0, Network::NoPreferredIP, lp.password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            return SDL_APP_FAILURE;
        }
    }

    // Core is loaded, start the GPU (makes the GPU contexts current to this thread)
    state->system.GPU().Start();
    state->system.GetCpuManager().OnGpuReady();

    if (Settings::values.use_disk_shader_cache.GetValue()) {
        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            state->system.GetApplicationProcessProgramID(), std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t value, size_t total) {});
    }

    // don't do anything, SDL3 already exists for us :D
    state->system.RegisterExitCallback([] {});

    // QLaunch launched applications (should) now reload shader cache.
    static std::mutex shader_cache_reload_mutex;
    // By value: state is a local of SDL_AppInit, the callback runs long after it returns.
    state->system.RegisterApplicationChangedCallback([state](u64 changed_program_id) {
        if (!Settings::values.use_disk_shader_cache.GetValue()) {
            return;
        }

        std::scoped_lock lk{shader_cache_reload_mutex};

        LOG_INFO(Frontend, "Reloading disk shader cache for {:016X}", changed_program_id);

        state->system.Pause();
        state->system.GPU().WaitForIdle();
        state->system.GPU().ObtainContext();

        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            changed_program_id, std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t, size_t) {});

        state->system.GPU().ReleaseContext();
        state->system.Run();
    });

    void(state->system.Run());
    if (state->system.DebuggerEnabled())
        state->system.InitializeDebugger();
    return SDL_APP_CONTINUE;
}
// EDEN_NX2_KEY_SCRIPT="seconds:code,seconds:-code,..." (or "@<file>"): press (code) or release (-code) keyboard
// keys, by SDL scancode, that many seconds after start. Lets hidden test runs drive menus.
double Nx2KeyClockSeconds(); // emu_window_sdl3.cpp

static void RunKeyScript(InputCommon::InputSubsystem& input_subsystem) {
    struct KeyEvent {
        double seconds; // when frame is 0
        u64 frame;      // "f<frame>:code": after that many presents
        int code;
    };
    static const std::vector<KeyEvent> script = [] {
        std::vector<KeyEvent> events;
        if (const char* text = std::getenv("EDEN_NX2_KEY_SCRIPT")) {
            // "@<file>" reads the list from a file: long scripts in the environment slow down
            // every getenv in the process.
            std::string list{text};
            if (!list.empty() && list[0] == '@') {
                std::ifstream file{list.substr(1)};
                list.assign(std::istreambuf_iterator<char>{file}, {});
            }
            size_t start = 0;
            while (start < list.size()) {
                const size_t end = (std::min)(list.find(',', start), list.size());
                const std::string item = list.substr(start, end - start);
                if (const size_t colon = item.find(':'); colon != std::string::npos) {
                    const std::string when = item.substr(0, colon);
                    const int code = std::stoi(item.substr(colon + 1));
                    if (!when.empty() && when[0] == 'f') {
                        events.push_back({0.0, std::stoull(when.substr(1)), code});
                    } else {
                        events.push_back({std::stod(when), 0, code});
                    }
                }
                start = end + 1;
            }
        }
        return events;
    }();
    static size_t next = 0;
    const double elapsed = Nx2KeyClockSeconds();
    const u64 frame = Core::g_nx2_present_count.load();
    while (next < script.size() && (script[next].frame != 0 ? script[next].frame <= frame
                                                            : script[next].seconds <= elapsed)) {
        const int code = script[next].code;
        LOG_INFO(Frontend, "Key script: {} key {} at {:.1f} s (frame {})",
                 code < 0 ? "release" : "press", code < 0 ? -code : code, elapsed, frame);
        if (code < 0) {
            input_subsystem.GetKeyboard()->ReleaseKey(static_cast<std::size_t>(-code));
        } else {
            input_subsystem.GetKeyboard()->PressKey(static_cast<std::size_t>(code));
        }
        ++next;
    }
}

extern "C" SDL_AppResult SDL_AppIterate(void *appstate) {
    SdlState *state = (SdlState *)appstate;
    RunKeyScript(state->input_subsystem);
    return state->emu_window && state->emu_window->IsOpen() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}
extern "C" SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    SdlState *state = (SdlState *)appstate;
    if (state->emu_window)
        state->emu_window->OnEvent(*event);
    return SDL_APP_CONTINUE;
}
extern "C" void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    SdlState *state = (SdlState *)appstate;
    if (!state) {
        return;
    }
    state->system.DetachDebugger();
    // SDL calls this even when SDL_AppInit never powered the system on (no ROM given, --help or
    // --version): the kernel is uninitialised then, and Pause()/ShutdownMainProcess() would
    // dereference it.
    if (state->system.IsPoweredOn()) {
        void(state->system.Pause());
        state->system.ShutdownMainProcess();
    }
    const bool network_initialized = state->network_initialized;
    delete state;
    if (network_initialized) {
        Network::Shutdown();
    }
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
