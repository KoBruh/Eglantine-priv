// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/staging_buffer_policy.h"

namespace {
constexpr std::size_t MiB = 1ULL << 20;
}

TEST_CASE("Large staging buffers avoid power-of-two amplification", "[video_core][staging]") {
    REQUIRE(Vulkan::StagingBufferAllocationSize(0) == 1);
    REQUIRE(Vulkan::StagingBufferAllocationSize(15 * MiB) == 16 * MiB);
    REQUIRE(Vulkan::StagingBufferAllocationSize(341 * MiB) == 341 * MiB);
    REQUIRE(Vulkan::StagingBufferAllocationSize(512 * MiB + 1) == 512 * MiB + 65536);
    for (std::size_t requested = 16 * MiB; requested < 2ULL * 1024 * MiB; requested += 65537) {
        const auto capacity = Vulkan::StagingBufferAllocationSize(requested);
        REQUIRE(capacity >= requested);
        REQUIRE(capacity - requested < 65536);
        REQUIRE(capacity % 65536 == 0);
    }
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    REQUIRE(Vulkan::StagingBufferAllocationSize(maximum) == maximum);
    REQUIRE(Vulkan::StagingBufferAllocationSize(maximum - 1) >= maximum - 1);
}

TEST_CASE("Mapped staging views expose only the requested transfer", "[video_core][staging]") {
    std::array<unsigned char, 32> memory{};
    memory.fill(0xA5);
    auto transfer = Vulkan::StagingBufferMappedRange(std::span{memory}.subspan(4), 7);
    REQUIRE(transfer.data() == memory.data() + 4);
    REQUIRE(transfer.size() == 7);
    for (auto& value : transfer) {
        value = 0x42;
    }
    REQUIRE(memory[3] == 0xA5);
    REQUIRE(memory[10] == 0x42);
    REQUIRE(memory[11] == 0xA5);
    REQUIRE(Vulkan::StagingBufferMappedRange(std::span<unsigned char>{}, 341 * MiB).empty());
}

TEST_CASE("Idle staging eviction preserves deferred and in-flight transfers", "[video_core][staging]") {
    Vulkan::StagingBufferIdleBudget budget;
    REQUIRE_FALSE(budget.ShouldRelease(512 * MiB, false, false));
    REQUIRE_FALSE(budget.ShouldRelease(512 * MiB, true, true));
    REQUIRE_FALSE(budget.ShouldRelease(48 * MiB, false, true));
    REQUIRE(budget.ShouldRelease(32 * MiB, false, true));
    REQUIRE_FALSE(budget.ShouldRelease(16 * MiB, false, true));
    REQUIRE(budget.ShouldRelease(16 * MiB, false, true));
    REQUIRE(budget.ShouldRelease((std::numeric_limits<std::size_t>::max)(), false, true));
}
