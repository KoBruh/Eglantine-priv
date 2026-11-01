// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
public:
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
    }

    void Reset() noexcept {
        std::ranges::fill(pages, 0ULL);
        recent.fill({~u64{0}, 0});
    }

    void Track(u64 offset, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        // Tracking is idempotent until Reset: skip ranges tracked just before. Draws bind the
        // same (often multi-megabyte) storage buffers again and again, and each Track wrote a
        // word per 4 KiB of the range.
        for (const auto& [recent_offset, recent_size] : recent) {
            if (recent_offset == offset && recent_size == size) {
                return;
            }
        }
        recent[recent_cursor++ % recent.size()] = {offset, size};
        const u64 last = offset + size - 1;
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = last >> BUFFER_PAGE_SHIFT;
        if (last < offset || page_end >= pages.size()) {
            std::ranges::fill(pages, ~u64{0});
            return;
        }
        const size_t first_bit = (offset % BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t last_bit = (last % BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        if (page == page_end) {
            pages[page] |= RangeMask(first_bit, last_bit);
            return;
        }
        pages[page] |= ~u64{0} << first_bit;
        for (size_t i = page + 1; i < page_end; i++) {
            pages[i] = ~u64{0};
        }
        pages[page_end] |= ~u64{0} >> (63 - last_bit);
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        if (size == 0) {
            return false;
        }
        const u64 last = offset + size - 1;
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = last >> BUFFER_PAGE_SHIFT;
        if (last < offset || page_end >= pages.size()) {
            return true;
        }
        const size_t first_bit = (offset % BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t last_bit = (last % BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        if (page == page_end) {
            return (pages[page] & RangeMask(first_bit, last_bit)) != 0;
        }
        if ((pages[page] & (~u64{0} << first_bit)) != 0) {
            return true;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            if (pages[i] != 0) {
                return true;
            }
        }
        return (pages[page_end] & (~u64{0} >> (63 - last_bit))) != 0;
    }

private:
    static u64 RangeMask(size_t first_bit, size_t last_bit) noexcept {
        return (~u64{0} << first_bit) & (~u64{0} >> (63 - last_bit));
    }

private:
    std::vector<u64> pages;
    /// Ranges tracked last, (offset, size).
    std::array<std::pair<u64, u64>, 4> recent{{{~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}}};
    size_t recent_cursor{};
};

} // namespace VideoCommon
