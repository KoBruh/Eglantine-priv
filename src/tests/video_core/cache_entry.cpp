// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "common/container/unordered_map.h"
#include "common/slot_vector.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/texture_cache/cache_entry.h"

namespace {

struct Payload {
    explicit Payload(int& live_) : live{live_} { ++live; }
    ~Payload() { --live; }
    int& live;
};

struct CacheRecord {
    explicit CacheRecord(int& live) : payload{std::make_unique<Payload>(live)} {}
    CacheRecord(CacheRecord&&) noexcept = default;
    CacheRecord& operator=(CacheRecord&&) noexcept = default;
    std::unique_ptr<Payload> payload;
};

} // namespace

TEST_CASE("Texture cache churn returns removed records to the slot pool", "[video_core][memory]") {
    int live{};
    Common::SlotVector<CacheRecord> slots;
    Common::unordered_map<u64, Common::SlotId> lookup;
    Common::SlotId previous{};
    for (u64 cycle = 0; cycle < 8192; ++cycle) {
        const auto id = slots.insert(live);
        if (previous) {
            REQUIRE(id == previous);
        }
        previous = id;
        const auto [entry, inserted] = lookup.emplace(cycle, id);
        REQUIRE(inserted);
        REQUIRE(live == 1);
        VideoCommon::EraseCachedSlot(slots, lookup, entry);
        REQUIRE(lookup.empty());
        REQUIRE(slots.size() == 0);
        REQUIRE(live == 0);
    }
    for (u64 key = 0; key < 128; ++key) {
        lookup.emplace(key, slots.insert(live));
    }
    auto entry = lookup.begin();
    while (entry != lookup.end()) {
        entry = VideoCommon::EraseCachedSlot(slots, lookup, entry);
        REQUIRE(slots.size() == lookup.size());
    }
    REQUIRE(live == 0);
}

TEST_CASE("Retired framebuffer resources survive reuse of their cache slot", "[video_core][memory]") {
    int live{};
    Common::SlotVector<CacheRecord> slots;
    Common::unordered_map<u64, Common::SlotId> lookup;
    VideoCommon::DelayedDestructionRing<CacheRecord, 6> retired;
    const auto old_id = slots.insert(live);
    lookup.emplace(1, old_id);
    retired.Push(std::move(slots[old_id]));
    VideoCommon::EraseCachedSlot(slots, lookup, lookup.find(1));
    REQUIRE(slots.size() == 0);
    REQUIRE(live == 1);
    const auto new_id = slots.insert(live);
    lookup.emplace(2, new_id);
    REQUIRE(new_id == old_id);
    REQUIRE(live == 2);
    for (int tick = 0; tick < 5; ++tick) {
        retired.Tick();
        REQUIRE(live == 2);
    }
    retired.Tick();
    REQUIRE(live == 1);
    VideoCommon::EraseCachedSlot(slots, lookup, lookup.find(2));
    REQUIRE(live == 0);
}
