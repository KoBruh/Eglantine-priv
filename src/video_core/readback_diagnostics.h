// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::ReadbackDiagnostics {

enum class Event : size_t {
    OrderedRead,
    FlushArea,
    TextureReadback,
    BufferReadback,
    QueryReadback,
    DownloadStaging,
    DeferredDownloadStaging,
    UploadStaging,
    BufferFinish,
    Submit,
    DeviceFinish,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Event::Count)> names{
    "ordered_read", "flush_area", "texture_readback", "buffer_readback", "query_readback",
    "download_staging", "deferred_download_staging", "upload_staging", "buffer_finish",
    "submit", "device_finish",
};

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_READBACK_DIAGNOSTICS") != nullptr;
    return enabled;
}

struct Measurement {
    u64 calls{};
    u64 bytes{};
    u64 samples{};
    u64 sampled_ns{};
};

struct ThreadData {
    std::array<Measurement, static_cast<size_t>(Event::Count)> events{};
    std::chrono::steady_clock::time_point begin{};
    size_t depth{};
    bool report_due{};
};

inline thread_local ThreadData data;

class Scope {
public:
    explicit Scope(Event event_, u64 bytes = 0) : event{event_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        ++data.depth;
        auto& measurement = data.events[static_cast<size_t>(event)];
        measurement.bytes += bytes;
        sampled = (measurement.calls++ & 255) == 0;
        if (sampled) {
            start = std::chrono::steady_clock::now();
            if (data.begin == std::chrono::steady_clock::time_point{}) {
                data.begin = start;
            }
        }
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        if (sampled) {
            const auto now = std::chrono::steady_clock::now();
            auto& measurement = data.events[static_cast<size_t>(event)];
            ++measurement.samples;
            measurement.sampled_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
            data.report_due |= now - data.begin >= std::chrono::seconds{10};
        }
        if (--data.depth != 0 || !data.report_due) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::string summary;
        for (size_t i = 0; i < data.events.size(); ++i) {
            const auto& value = data.events[i];
            if (value.calls != 0) {
                summary += fmt::format(" | {} calls={} bytes={} samples={} sampled_ns={}", names[i],
                                       value.calls, value.bytes, value.samples, value.sampled_ns);
            }
        }
        LOG_INFO(HW_GPU, "ReadbackDiag window_ns={}{}",
                 std::chrono::duration_cast<std::chrono::nanoseconds>(now - data.begin).count(),
                 summary);
        data = {};
        data.begin = now;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Event event;
    bool enabled;
    bool sampled{};
    std::chrono::steady_clock::time_point start{};
};

} // namespace VideoCommon::ReadbackDiagnostics
