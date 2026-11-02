// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::CacheTrackingDiagnostics {

enum class Event : size_t {
    TextureTrackedRead,
    TexturePartialRead,
    TexturePartialWrite,
    BufferInlineRetain,
    BufferCpuMark,
    Count,
};

inline constexpr std::array names{
    "texture_tracked_read", "texture_partial_read", "texture_partial_write",
    "buffer_inline_retain", "buffer_cpu_mark",
};

struct Range {
    u64 begin{};
    u64 size{};
    u64 related_begin{};
    u64 related_size{};
};

struct Measurement {
    u64 calls{};
    u64 bytes{};
    u64 related_bytes{};
    std::array<Range, 16> ranges{};
    size_t range_count{};
};

struct ThreadData {
    std::array<Measurement, static_cast<size_t>(Event::Count)> events{};
    std::chrono::steady_clock::time_point begin{};
};

inline thread_local ThreadData data;

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_CACHE_TRACK_DIAGNOSTICS") != nullptr;
    return enabled;
}

inline void Record(Event event, u64 begin, u64 size, u64 related_begin = 0, u64 related_size = 0) {
    if (!Enabled()) {
        return;
    }
    auto& entry = data.events[static_cast<size_t>(event)];
    ++entry.calls;
    entry.bytes += size;
    entry.related_bytes += related_size;
    if ((entry.calls & 1023) != 1) {
        return;
    }
    entry.ranges[entry.range_count++ % entry.ranges.size()] = {begin, size, related_begin, related_size};
    const auto now = std::chrono::steady_clock::now();
    if (data.begin == std::chrono::steady_clock::time_point{}) {
        data.begin = now;
    }
    if (now - data.begin < std::chrono::seconds{10}) {
        return;
    }
    for (size_t i = 0; i < data.events.size(); ++i) {
        const auto& value = data.events[i];
        if (value.calls == 0) {
            continue;
        }
        std::string ranges;
        for (size_t j = 0; j < (std::min)(value.range_count, value.ranges.size()); ++j) {
            const auto& range = value.ranges[j];
            ranges += fmt::format(" [{:#x},{};{:#x},{}]", range.begin, range.size,
                                  range.related_begin, range.related_size);
        }
        LOG_INFO(HW_GPU,
                 "CacheTrackingDiag thread={} window_ns={} event={} calls={} bytes={} related_bytes={} ranges={}",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()),
                 std::chrono::duration_cast<std::chrono::nanoseconds>(now - data.begin).count(),
                 names[i], value.calls, value.bytes, value.related_bytes, ranges);
    }
    data = {};
    data.begin = now;
}

} // namespace VideoCommon::CacheTrackingDiagnostics
