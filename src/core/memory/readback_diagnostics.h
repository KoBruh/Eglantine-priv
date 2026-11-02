// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <thread>

#include "common/logging.h"
#include "core/hardware_properties.h"

namespace Core::Memory::ReadbackDiagnostics {

enum class Caller : size_t { ReadBlock, CopyBlock, InvalidateDataCache, ScalarRead, Count };
enum class Event : size_t {
    HostGate,
    HostEligible,
    HostHit,
    HostMiss,
    FastmemFallback,
    MultiAliasFallback,
    OtherFallback,
    AreaHit,
    AreaMiss,
    GpuPage,
    GpuFallback,
    GpuCore,
    RememberRejected,
    CoreCacheHit,
    Count,
};

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_READBACK_DIAGNOSTICS") != nullptr;
    return enabled;
}

struct Counts {
    u64 calls{};
    u64 bytes{};
    std::array<u64, static_cast<size_t>(Event::Count)> events{};
    std::array<u64, 5> aliases{};
    u64 samples{};
    u64 sampled_ns{};
};

struct State {
    using Clock = std::chrono::steady_clock;
    static constexpr size_t CoreSlots = Hardware::NUM_CPU_CORES + 1;
    static constexpr u64 SampleStride = 1024;
    std::array<Counts, static_cast<size_t>(Caller::Count) * CoreSlots> rows{};
    u64 sequence{};
    Clock::time_point last_report = Clock::now();

    void Report(Clock::time_point now) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();
        if (elapsed_ms < 10000) {
            return;
        }
        constexpr std::array names{"ReadBlock", "CopyBlock", "InvalidateDataCache", "Read<T>"};
        const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (row.calls == 0) {
                continue;
            }
            const size_t core = i % CoreSlots;
            LOG_INFO(HW_Memory,
                     "ReadbackDiag thread={} interval_ms={} caller={} guestcore={} core={} calls={} "
                     "requested_bytes={} events[host_gate,host_eligible,host_hit,host_miss,"
                     "fastmem_fallback,multialias_fallback,other_fallback,area_hit,area_miss,"
                     "gpu_page,gpu_fallback,gpu_core,remember_rejected,core_cache_hit]=[{}] aliases[0,1,2,3,4+]=[{}] "
                     "sample_fraction=1/{} samples={} sampled_ns={}",
                     thread, elapsed_ms, names[i / CoreSlots], core < Hardware::NUM_CPU_CORES,
                     core, row.calls, row.bytes, fmt::join(row.events, ","),
                     fmt::join(row.aliases, ","), SampleStride, row.samples, row.sampled_ns);
        }
        rows = {};
        last_report = now;
    }
};

inline State& ThreadState() {
    thread_local State state;
    return state;
}

class Scope {
public:
    explicit Scope(Caller caller_, size_t size_) : caller{caller_}, size{size_} {
        if (Enabled()) {
            state = &ThreadState();
            sampled = (state->sequence++ % State::SampleStride) == 0;
            if (sampled) {
                start = State::Clock::now();
            }
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void BindCore(size_t core) {
        if (state) {
            const size_t slot = core < Hardware::NUM_CPU_CORES ? core : Hardware::NUM_CPU_CORES;
            row = &state->rows[static_cast<size_t>(caller) * State::CoreSlots + slot];
            ++row->calls;
            row->bytes += size;
        }
    }

    void Add(Event event) {
        if (row) {
            ++row->events[static_cast<size_t>(event)];
        }
    }

    void SetAliases(size_t count) {
        if (row) {
            aliases = count;
        }
    }

    void AddAlias() {
        if (row) {
            ++aliases;
        }
    }

    ~Scope() {
        if (!row) {
            return;
        }
        ++row->aliases[aliases < 4 ? aliases : 4];
        if (sampled) {
            const auto end = State::Clock::now();
            ++row->samples;
            row->sampled_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            state->Report(end);
        }
    }

private:
    State* state{};
    Counts* row{};
    Caller caller;
    size_t size;
    size_t aliases{};
    bool sampled{};
    State::Clock::time_point start{};
};

} // namespace Core::Memory::ReadbackDiagnostics
