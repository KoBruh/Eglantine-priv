#pragma once

#include <array>
#include <chrono>
#include <cstdlib>

#include "common/logging.h"
#include "core/memory/readback_cache.h"

namespace Core::Memory::CoreReadbackShadow {

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_CORE_READBACK_SHADOW") != nullptr;
    return enabled;
}

struct Snapshot {
    HostReadbackCache::Page page{};
    u64 generation{};
    size_t aliases{};
};

struct State {
    struct Entry {
        HostReadbackCache certificate;
        HostReadbackCache::Page page{};
        u64 age{};
    };
    struct Counts {
        u64 old_area_misses{};
        u64 eligible{};
        u64 resident_page{};
        u64 would_hit{};
        u64 epoch_miss{};
        u64 identity_miss{};
        u64 zero_generation{};
        u64 lookup_race{};
        u64 set_conflict{};
        u64 remember_attempts{};
        u64 remember_accepted{};
        u64 rejected_zero{};
        u64 rejected_generation{};
        u64 rejected_identity_or_coverage{};
    } counts;
    std::array<Entry, 64> entries{};
    u64 age{};
    u64 sequence{};
    std::chrono::steady_clock::time_point last_report{};

    static bool SamePhysicalPage(const HostReadbackCache::Page& a,
                                 const HostReadbackCache::Page& b) {
        return a.owner_instance == b.owner_instance && a.owner == b.owner &&
               a.host_page == b.host_page && a.device_page == b.device_page;
    }

    static size_t SetOffset(const HostReadbackCache::Page& page) {
        return (((page.device_page >> 12) * 0x9e3779b97f4a7c15ULL) >> 59) * 2;
    }

    bool Before(const Snapshot& current, u64 offset, size_t size) {
        ++counts.old_area_misses;
        if (current.page.owner == nullptr || current.aliases != 1 ||
            !HostReadbackCache::FitsInPage(offset, size)) {
            return false;
        }
        ++counts.eligible;
        if (current.generation == 0) {
            ++counts.zero_generation;
        }
        const size_t first = SetOffset(current.page);
        for (size_t i = first; i < first + 2; ++i) {
            auto& entry = entries[i];
            if (!SamePhysicalPage(entry.page, current.page)) {
                continue;
            }
            ++counts.resident_page;
            if (entry.page != current.page) {
                ++counts.identity_miss;
            } else if (entry.certificate.CanReuse(current.page, current.aliases,
                                                  current.generation, offset, size)) {
                entry.age = ++age;
                return true;
            } else {
                ++counts.epoch_miss;
            }
            return false;
        }
        if (entries[first].page.owner != nullptr && entries[first + 1].page.owner != nullptr) {
            ++counts.set_conflict;
        }
        return false;
    }

    void Confirm(bool stable) {
        if (stable) {
            ++counts.would_hit;
        } else {
            ++counts.lookup_race;
        }
    }

    void After(const Snapshot& captured, VideoCore::RasterizerDownloadArea area,
               const Snapshot& current, size_t core) {
        ++counts.remember_attempts;
        HostReadbackCache certificate;
        certificate.Remember(captured.page, area, current.page, current.aliases,
                             current.generation);
        if (certificate.CanReuse(current.page, current.aliases, current.generation,
                                 0, YUZU_PAGESIZE)) {
            ++counts.remember_accepted;
            const size_t first = SetOffset(current.page);
            size_t victim = first;
            for (size_t i = first; i < first + 2; ++i) {
                if (SamePhysicalPage(entries[i].page, current.page) || entries[i].page.owner == nullptr) {
                    victim = i;
                    break;
                }
                if (entries[i].age < entries[victim].age) {
                    victim = i;
                }
            }
            entries[victim] = {certificate, current.page, ++age};
        } else if (area.completed_generation == 0) {
            ++counts.rejected_zero;
        } else if (area.completed_generation != current.generation) {
            ++counts.rejected_generation;
        } else {
            ++counts.rejected_identity_or_coverage;
        }
        if ((sequence++ & 1023) != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (last_report == std::chrono::steady_clock::time_point{}) {
            last_report = now;
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report).count();
        if (elapsed < 10000) {
            return;
        }
        LOG_INFO(HW_Memory,
                 "CoreReadbackShadow core={} interval_ms={} capacity=64 policy=32sets_2way_lru "
                 "old_area_misses={} eligible={} resident_page={} would_hit={} epoch_miss={} "
                 "identity_miss={} zero_generation={} lookup_race={} set_conflict={} remember_attempts={} remember_accepted={} "
                 "rejected_zero={} rejected_generation={} rejected_identity_or_coverage={}",
                 core, elapsed, counts.old_area_misses, counts.eligible, counts.resident_page,
                 counts.would_hit, counts.epoch_miss, counts.identity_miss, counts.zero_generation,
                 counts.lookup_race, counts.set_conflict, counts.remember_attempts, counts.remember_accepted, counts.rejected_zero,
                 counts.rejected_generation, counts.rejected_identity_or_coverage);
        counts = {};
        last_report = now;
    }
};

}
