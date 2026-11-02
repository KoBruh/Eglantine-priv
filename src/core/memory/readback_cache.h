// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/memory.h"
#include "video_core/rasterizer_download_area.h"

namespace Core::Memory {

class HostReadbackCache {
public:
    struct Page {
        const void* owner{};
        const u8* host_page{};
        DAddr device_page{};
        u64 trap_generation{};
        u64 owner_instance{};

        bool operator==(const Page&) const = default;
    };

    static bool FitsInPage(u64 offset, u64 size) {
        return offset < YUZU_PAGESIZE && size != 0 && size <= YUZU_PAGESIZE - offset;
    }

    void Invalidate() {
        completed_generation = 0;
    }

    bool CanReuse(Page current, size_t alias_count, u64 current_generation, u64 offset, u64 size) {
        if (completed_generation != 0 && completed_generation == current_generation &&
            alias_count == 1 && page == current && FitsInPage(offset, size)) {
            return true;
        }
        Invalidate();
        return false;
    }

    void Remember(Page captured, VideoCore::RasterizerDownloadArea area, Page current,
                  size_t alias_count, u64 current_generation) {
        Invalidate();
        if (captured != current || alias_count != 1 || captured.owner == nullptr ||
            captured.owner_instance == 0 || captured.host_page == nullptr ||
            captured.device_page == 0 || (captured.device_page & YUZU_PAGEMASK) != 0 ||
            captured.device_page > ~u64{0} - YUZU_PAGESIZE || area.completed_generation == 0 ||
            area.completed_generation != current_generation ||
            area.start_address > captured.device_page ||
            area.end_address < captured.device_page + YUZU_PAGESIZE) {
            return;
        }
        page = captured;
        completed_generation = area.completed_generation;
    }

private:
    Page page{};
    u64 completed_generation{};
};

} // namespace Core::Memory
