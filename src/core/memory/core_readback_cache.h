#pragma once

#include <array>
#include <cstdlib>
#include <cstring>

#include "core/memory/readback_cache.h"

namespace Core::Memory {

template <size_t SetBits>
class CoreReadbackCacheImpl {
    static_assert(SetBits > 0 && SetBits < 63);
public:
    static bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_CORE_READBACK_CACHE");
            return value == nullptr || std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }

    bool CanReuse(HostReadbackCache::Page page, size_t aliases, u64 generation,
                  u64 offset, size_t size) {
        if (page.owner == nullptr || aliases != 1 || generation == 0 ||
            !HostReadbackCache::FitsInPage(offset, size)) {
            return false;
        }
        const size_t first = SetOffset(page);
        for (size_t i = first; i < first + 2; ++i) {
            auto& entry = entries[i];
            if (SamePhysicalPage(entry.page, page)) {
                if (entry.certificate.CanReuse(page, aliases, generation, offset, size)) {
                    entry.age = ++age;
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    bool Remember(HostReadbackCache::Page captured, VideoCore::RasterizerDownloadArea area,
                  HostReadbackCache::Page current, size_t aliases, u64 generation) {
        HostReadbackCache certificate;
        certificate.Remember(captured, area, current, aliases, generation);
        if (!certificate.CanReuse(current, aliases, generation, 0, YUZU_PAGESIZE)) {
            return false;
        }
        const size_t first = SetOffset(current);
        size_t victim = first;
        for (size_t i = first; i < first + 2; ++i) {
            if (SamePhysicalPage(entries[i].page, current) || entries[i].page.owner == nullptr) {
                victim = i;
                break;
            }
            if (entries[i].age < entries[victim].age) {
                victim = i;
            }
        }
        entries[victim] = {certificate, current, ++age};
        return true;
    }

private:
    struct Entry {
        HostReadbackCache certificate;
        HostReadbackCache::Page page{};
        u64 age{};
    };

    static bool SamePhysicalPage(const HostReadbackCache::Page& a,
                                 const HostReadbackCache::Page& b) {
        return a.owner_instance == b.owner_instance && a.owner == b.owner &&
               a.host_page == b.host_page && a.device_page == b.device_page;
    }

    static size_t SetOffset(const HostReadbackCache::Page& page) {
        return (((page.device_page >> 12) * 0x9e3779b97f4a7c15ULL) >> (64 - SetBits)) * 2;
    }

    std::array<Entry, size_t{2} << SetBits> entries{};
    u64 age{};
};

using CoreReadbackCache = CoreReadbackCacheImpl<5>;

}
