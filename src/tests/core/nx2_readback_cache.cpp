// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/read_traps.h"
#include "core/memory/readback_cache.h"

namespace {
using Core::Memory::HostReadbackCache;
using Core::Memory::ReadTraps;
using Core::Memory::YUZU_PAGESIZE;

struct Readback {
    int owner{};
    std::array<u8, YUZU_PAGESIZE * 2> backing{};
    HostReadbackCache::Page page{&owner, backing.data(), 0x8000, 3, 1};
    VideoCore::RasterizerDownloadArea area{0x8000, 0x9000, false, 11};
    HostReadbackCache cache;
};
} // namespace

TEST_CASE("NX2 host readback cache requires an actual completed whole page",
          "[core][nx2][readback_cache]") {
    Readback read;
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 4, 4));
    SECTION("the preflushable policy bit is false") {
        read.area.preemtive = false;
    }
    SECTION("the preflushable policy bit is true") {
        read.area.preemtive = true;
    }
    read.cache.Remember(read.page, read.area, read.page, 1, 11);
    REQUIRE(read.cache.CanReuse(read.page, 1, 11, 4, 4));
    REQUIRE(read.cache.CanReuse(read.page, 1, 11, YUZU_PAGESIZE - 1, 1));
    REQUIRE(read.cache.CanReuse(read.page, 1, 11, 0, YUZU_PAGESIZE));
}

TEST_CASE("NX2 host readback cache rejects partial and uncertified downloads",
          "[core][nx2][readback_cache]") {
    Readback read;
    SECTION("an area beginning inside the page") {
        ++read.area.start_address;
    }
    SECTION("an area ending inside the page") {
        --read.area.end_address;
    }
    SECTION("an empty area") {
        read.area.end_address = read.area.start_address;
    }
    SECTION("a policy-only area") {
        read.area.preemtive = true;
        read.area.completed_generation = 0;
    }
    SECTION("an invalid current GPU generation") {
        read.area.completed_generation = 0;
        read.cache.Remember(read.page, read.area, read.page, 1, 0);
        REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 0, 0, 4));
        return;
    }
    read.cache.Remember(read.page, read.area, read.page, 1, 11);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
}

TEST_CASE("NX2 host readback cache keeps every page identity component",
          "[core][nx2][readback_cache]") {
    Readback read;
    auto changed = read.page;
    int other_owner{};
    SECTION("another MemoryImpl owns the read") {
        changed.owner = &other_owner;
    }
    SECTION("a new session reuses the MemoryImpl address") {
        ++changed.owner_instance;
    }
    SECTION("the CPU page has different physical backing") {
        changed.host_page += YUZU_PAGESIZE;
    }
    SECTION("the same backing has a different device alias") {
        changed.device_page += YUZU_PAGESIZE;
    }
    SECTION("a write or mapping change advanced the trap epoch") {
        ++changed.trap_generation;
    }
    read.cache.Remember(read.page, read.area, read.page, 1, 11);
    REQUIRE_FALSE(read.cache.CanReuse(changed, 1, 11, 0, 4));
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    read.cache.Remember(read.page, read.area, changed, 1, 11);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
}

TEST_CASE("NX2 host readback cache invalidates on zero or multiple current aliases",
          "[core][nx2][readback_cache]") {
    Readback read;
    for (const size_t aliases : {0, 2, 3}) {
        CAPTURE(aliases);
        read.cache.Remember(read.page, read.area, read.page, 1, 11);
        REQUIRE_FALSE(read.cache.CanReuse(read.page, aliases, 11, 0, 4));
        REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
        read.cache.Remember(read.page, read.area, read.page, aliases, 11);
        REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    }
}

TEST_CASE("NX2 host readback cache cannot span physical pages or overflow a range",
          "[core][nx2][readback_cache]") {
    Readback read;
    for (const auto [offset, size] : std::array<std::array<u64, 2>, 7>{{
             {YUZU_PAGESIZE - 1, 2},
             {0, YUZU_PAGESIZE + 1},
             {YUZU_PAGESIZE, 1},
             {0, 0},
             {1, std::numeric_limits<u64>::max()},
             {std::numeric_limits<u64>::max(), 1},
             {std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max()},
         }}) {
        CAPTURE(offset, size);
        read.cache.Remember(read.page, read.area, read.page, 1, 11);
        REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, offset, size));
        REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    }
    for (u64 offset = 0; offset < YUZU_PAGESIZE; ++offset) {
        REQUIRE(HostReadbackCache::FitsInPage(offset, YUZU_PAGESIZE - offset));
        REQUIRE_FALSE(HostReadbackCache::FitsInPage(offset, YUZU_PAGESIZE - offset + 1));
    }
}

TEST_CASE("NX2 host readback cache cannot absorb a writer published after its read",
          "[core][nx2][readback_cache]") {
    Readback read;
    read.cache.Remember(read.page, read.area, read.page, 1, 12);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 12, 0, 4));
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    read.area.completed_generation = 12;
    read.cache.Remember(read.page, read.area, read.page, 1, 12);
    REQUIRE(read.cache.CanReuse(read.page, 1, 12, 0, 4));
    for (u64 writer = 13; writer < 20; ++writer) {
        REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, writer, 0, 4));
        read.area.completed_generation = writer;
        read.cache.Remember(read.page, read.area, read.page, 1, writer);
        REQUIRE(read.cache.CanReuse(read.page, 1, writer, 0, 4));
    }
}

TEST_CASE("NX2 host readback cache keeps the command's captured generation",
          "[core][nx2][readback_cache]") {
    Readback read;
    read.area.completed_generation = 12;
    read.cache.Remember(read.page, read.area, read.page, 1, 12);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    read.cache.Remember(read.page, read.area, read.page, 1, 12);
    REQUIRE(read.cache.CanReuse(read.page, 1, 12, 0, 4));
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 13, 0, 4));
}

TEST_CASE("NX2 host readback cache refuses active or pending asynchronous writers",
          "[core][nx2][readback_cache]") {
    Readback read;
    read.cache.Remember(read.page, read.area, read.page, 1, 11);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 0, 0, 4));
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    read.cache.Remember(read.page, read.area, read.page, 1, 0);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    read.cache.Remember(read.page, read.area, read.page, 1, 12);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 12, 0, 4));
    read.area.completed_generation = 12;
    read.cache.Remember(read.page, read.area, read.page, 1, 12);
    REQUIRE(read.cache.CanReuse(read.page, 1, 12, 0, 4));
}

TEST_CASE("NX2 host readback cache rejects mapping and trap changes during completion",
          "[core][nx2][readback_cache]") {
    Readback read;
    ReadTraps traps;
    traps.Arm(8, traps.NewGeneration());
    read.page.trap_generation = traps.Generation();
    SECTION("another GPU write arms a read trap") {
        traps.Arm(8, traps.NewGeneration());
    }
    SECTION("a mapping is dropped with no remaining armed pages") {
        traps.Drop(8, 9);
        REQUIRE(traps.Empty());
    }
    SECTION("the page table changes") {
        traps.Clear();
    }
    auto current = read.page;
    current.trap_generation = traps.Generation();
    read.cache.Remember(read.page, read.area, current, 1, 11);
    REQUIRE_FALSE(read.cache.CanReuse(current, 1, 11, 0, 4));
}

TEST_CASE("NX2 host readback cache remembers only one page and supports invalidation",
          "[core][nx2][readback_cache]") {
    Readback read;
    read.cache.Remember(read.page, read.area, read.page, 1, 11);
    auto next = read.page;
    next.host_page += YUZU_PAGESIZE;
    next.device_page += YUZU_PAGESIZE;
    read.area.start_address = next.device_page;
    read.area.end_address = next.device_page + YUZU_PAGESIZE;
    read.cache.Remember(next, read.area, next, 1, 11);
    REQUIRE(read.cache.CanReuse(next, 1, 11, 0, 4));
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
    read.cache.Remember(next, read.area, next, 1, 11);
    read.cache.Invalidate();
    REQUIRE_FALSE(read.cache.CanReuse(next, 1, 11, 0, 4));
}

TEST_CASE("NX2 host readback cache rejects invalid page certificates",
          "[core][nx2][readback_cache]") {
    Readback read;
    SECTION("no owner") {
        read.page.owner = nullptr;
    }
    SECTION("no session owner token") {
        read.page.owner_instance = 0;
    }
    SECTION("no physical page") {
        read.page.host_page = nullptr;
    }
    SECTION("no device alias") {
        read.page.device_page = 0;
        read.area.start_address = 0;
    }
    SECTION("an unaligned device page") {
        ++read.page.device_page;
        read.area.end_address += YUZU_PAGESIZE;
    }
    SECTION("the device page end would overflow") {
        read.page.device_page = std::numeric_limits<u64>::max() - YUZU_PAGESIZE + 1;
        read.area.end_address = std::numeric_limits<u64>::max();
    }
    read.cache.Remember(read.page, read.area, read.page, 1, 11);
    REQUIRE_FALSE(read.cache.CanReuse(read.page, 1, 11, 0, 4));
}
