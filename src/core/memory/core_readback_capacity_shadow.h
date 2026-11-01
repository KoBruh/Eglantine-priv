#pragma once

#include <chrono>

#include "common/logging.h"
#include "core/memory/core_readback_cache.h"
#include "core/memory/core_readback_shadow.h"

namespace Core::Memory::CoreReadbackCapacityShadow {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_CORE_READBACK_CAPACITY_SHADOW");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct State {
    using Snapshot = CoreReadbackShadow::Snapshot;
    struct Counts {
        u64 baseline_hits{};
        u64 baseline_misses{};
        std::array<u64, 2> potential_hits{};
        std::array<u64, 2> misses_on_hits{};
        std::array<u64, 2> lookup_races{};
        std::array<u64, 2> fill_accepted{};
    } counts;
    CoreReadbackCacheImpl<8> cache512;
    CoreReadbackCacheImpl<10> cache2048;
    u64 sequence{};
    std::chrono::steady_clock::time_point last_report{std::chrono::steady_clock::now()};

    template <typename Capture>
    void Before(const Snapshot& captured, Capture&& capture, u64 offset,
                size_t size, bool baseline_hit, size_t core) {
        ++(baseline_hit ? counts.baseline_hits : counts.baseline_misses);
        const std::array hits{
            cache512.CanReuse(captured.page, captured.aliases, captured.generation, offset, size),
            cache2048.CanReuse(captured.page, captured.aliases, captured.generation, offset, size)};
        const auto confirmed = capture();
        const bool stable = confirmed.page == captured.page && confirmed.aliases == 1 &&
                            confirmed.generation == captured.generation;
        for (size_t index = 0; index < hits.size(); ++index) {
            const bool hit = hits[index];
            if (hit && !stable) {
                ++counts.lookup_races[index];
            } else if (hit && !baseline_hit) {
                ++counts.potential_hits[index];
            } else if (!hit && baseline_hit) {
                ++counts.misses_on_hits[index];
            }
        }
        if ((sequence++ & 1023) != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();
        if (elapsed < 10000) {
            return;
        }
        LOG_INFO(HW_Memory,
                 "CoreReadbackCapacityShadow core={} interval_ms={} baseline_hits={} baseline_misses={} "
                 "potential512={} potential2048={} miss_on_hit512={} miss_on_hit2048={} "
                 "race512={} race2048={} fill512={} fill2048={}",
                 core, elapsed, counts.baseline_hits, counts.baseline_misses,
                 counts.potential_hits[0], counts.potential_hits[1],
                 counts.misses_on_hits[0], counts.misses_on_hits[1],
                 counts.lookup_races[0], counts.lookup_races[1],
                 counts.fill_accepted[0], counts.fill_accepted[1]);
        counts = {};
        last_report = now;
    }

    void After(const Snapshot& captured, VideoCore::RasterizerDownloadArea area,
               const Snapshot& current) {
        counts.fill_accepted[0] += cache512.Remember(captured.page, area, current.page,
                                                   current.aliases, current.generation);
        counts.fill_accepted[1] += cache2048.Remember(captured.page, area, current.page,
                                                    current.aliases, current.generation);
    }
};

}
