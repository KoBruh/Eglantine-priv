// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/page_table.h"

namespace {
using Common::PageTable;
using Common::PageType;

// What the JIT's page table lookup computes from an entry: drop the attributes, sign-extend from
// the configured bit (shl/sar by 63 - bit on x64, SBFM on arm64).
u64 JitPointer(u64 raw) {
    const u64 masked = raw & PageTable::ATTRIBUTE_MASK;
    const int shift = 63 - static_cast<int>(PageTable::SIGN_BIT);
    return static_cast<u64>(static_cast<s64>(masked << shift) >> shift);
}

u64 RawOf(const PageTable::PageEntryData& entry) {
    return std::bit_cast<u64>(entry.Raw());
}

TEST_CASE("Page table entries give the JIT the pointer ExtractPointer gives", "[common]") {
    // Absolute offsets (host page minus guest page): negative when the memory backing lies below
    // the guest addresses, as with Switch 2's 12 GiB layout on Windows (backing near 13 GB, guest
    // heap at 0x10'8580'0000).
    for (const u64 pointer : {0xfffffff2956ac000ULL, 0xffffffe23a7f0000ULL, 0x29966f000ULL,
                              0x00007ff000000000ULL, 0xffff800000001000ULL}) {
        for (const u16 block : {u16{0}, u16{0x1ff}, u16{0xffff}}) {
            PageTable::PageEntryData entry;
            entry.Store(false, PageType::Memory, block, pointer);
            REQUIRE(entry.Pointer() == pointer);
            REQUIRE(JitPointer(RawOf(entry)) == pointer);
        }
    }
}

TEST_CASE("Marked page table entries stay marked for the JIT", "[common]") {
    PageTable::PageEntryData entry;
    entry.Store(false, PageType::Memory, 3, 0xfffffff2956ac000ULL);
    entry.MarkRasterizerCached();
    REQUIRE((RawOf(entry) & 1) != 0); // page_table_marked_bit 0: the JIT takes the slow path
    REQUIRE(entry.Pointer() == 0);
    REQUIRE(entry.Pointer(true) == 0xfffffff2956ac000ULL);
}
} // namespace
