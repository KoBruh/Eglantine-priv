// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/rasterizer_download_area.h"

namespace VideoCommon::ReadbackDependencyDiagnostics {

inline u64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Window {
    static constexpr u64 Delay = 60'000'000'000;
    static constexpr u64 Duration = 30'000'000'000;
    static constexpr u64 Limit = 4096;
    std::atomic<u64> next_slot{};

    bool Select(u64 elapsed) {
        if (elapsed < Delay) {
            return false;
        }
        if (elapsed - Delay >= Duration) {
            next_slot.store(Limit, std::memory_order_relaxed);
            return false;
        }
        const u64 slot = (elapsed - Delay) * Limit / Duration;
        u64 next = next_slot.load(std::memory_order_relaxed);
        while (next <= slot) {
            if (next_slot.compare_exchange_weak(next, slot + 1, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
};

struct Record {
    u64 core{}, address{}, size{}, fence{}, published_sequence{};
    u64 requested_generation{}, backend_generation{}, checked_generation{}, checked_sequence{};
    u64 return_generation{}, before_lock{}, enqueued{}, front_popped{}, backend_start{};
    u64 area_end{}, flush_end{}, cpu_return{};
    u32 docked_mode{}, gpu_accuracy{}, runtime_gpu_accuracy{}, dma_accuracy{}, cpu_accuracy{};
    bool preemptive_flag{}, split{};
};

inline std::optional<Record> Begin() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_READBACK_DEPENDENCY_DIAGNOSTICS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!enabled) {
        return std::nullopt;
    }
    static const u64 origin = Now();
    static Window window;
    thread_local u64 requests{};
    if ((requests++ & 63) != 0) {
        return std::nullopt;
    }
    if (window.next_slot.load(std::memory_order_relaxed) == Window::Limit ||
        !window.Select(Now() - origin)) {
        return std::nullopt;
    }
    return Record{};
}

inline void Report(const Record& record, const VideoCore::RasterizerDownloadArea& result) {
    LOG_INFO(HW_GPU,
             "ReadbackDependency core={} addr={:#x} size={} fence={} sequence={} split={} "
             "requested_generation={} backend_generation={} checked_generation={} "
             "checked_sequence={} completed_generation={} return_generation={} "
             "result_begin={:#x} result_end={:#x} preemptive_flag={} "
             "before_lock_ns={} enqueued_ns={} front_popped_ns={} backend_start_ns={} "
             "area_end_ns={} flush_end_ns={} cpu_return_ns={} docked_mode={} gpu_accuracy={} "
             "runtime_gpu_accuracy={} dma_accuracy={} cpu_accuracy={}",
             record.core, record.address, record.size, record.fence, record.published_sequence,
             record.split, record.requested_generation, record.backend_generation,
             record.checked_generation, record.checked_sequence, result.completed_generation,
             record.return_generation, result.start_address, result.end_address,
             record.preemptive_flag, record.before_lock, record.enqueued, record.front_popped,
             record.backend_start, record.area_end, record.flush_end, record.cpu_return,
             record.docked_mode, record.gpu_accuracy, record.runtime_gpu_accuracy,
             record.dma_accuracy, record.cpu_accuracy);
}

} // namespace VideoCommon::ReadbackDependencyDiagnostics
