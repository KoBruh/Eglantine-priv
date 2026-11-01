// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/texture_cache/util.h"

using namespace VideoCommon;

namespace {
ImageInfo MakeImage(u32 width, u32 height, s32 levels, u32 block_height,
                    PixelFormat format = PixelFormat::R8_UNORM) {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = format;
    info.size = {width, height, 1};
    info.resources = {levels, 1};
    info.block = {0, block_height, 0};
    info.native_tiling = true;
    return info;
}

u64 Coverage(const ImageInfo& info, std::span<const LevelRect> rects) {
    u64 area{};
    for (size_t i = 0; i < rects.size(); ++i) {
        const auto& rect = rects[i];
        REQUIRE(rect.level >= 0);
        REQUIRE(rect.level < info.resources.levels);
        const auto size = MipSize(info.size, rect.level);
        REQUIRE(rect.width > 0);
        REQUIRE(rect.height > 0);
        REQUIRE(u64{rect.x} + rect.width <= size.width);
        REQUIRE(u64{rect.y} + rect.height <= size.height);
        for (size_t j = 0; j < i; ++j) {
            const auto& other = rects[j];
            if (other.level == rect.level) {
                REQUIRE((rect.x >= other.x + other.width || other.x >= rect.x + rect.width ||
                         rect.y >= other.y + other.height || other.y >= rect.y + rect.height));
            }
        }
        area += u64{rect.width} * rect.height;
    }
    return area;
}
} // namespace

TEST_CASE("Sparse tiles handle empty and overflowing ranges", "[sparse_tiles]") {
    const auto info = MakeImage(512, 512, 4, 3);
    const std::array zero{std::pair<u64, u64>{512, 0}};
    CHECK(SparseTileRects(info, zero, 128, 64, 2).empty());
    const std::array outside{std::pair<u64, u64>{std::numeric_limits<u64>::max() - 8, 32}};
    CHECK(SparseTileRects(info, outside, 128, 64, 2).empty());
    const std::array overflow{std::pair<u64, u64>{1, std::numeric_limits<u64>::max()}};
    const std::array bounded{std::pair<u64, u64>{1, CalculateGuestSizeInBytes(info) - 1}};
    CHECK(Coverage(info, SparseTileRects(info, overflow, 128, 64, 2)) ==
          Coverage(info, SparseTileRects(info, bounded, 128, 64, 2)));
}

TEST_CASE("Sparse tile rectangles cover padded mip layouts without overlap", "[sparse_tiles]") {
    std::mt19937 random{0x239};
    constexpr std::array formats{PixelFormat::R8_UNORM, PixelFormat::R8G8_UNORM,
                                 PixelFormat::A8B8G8R8_UNORM, PixelFormat::R16G16B16A16_FLOAT};
    for (unsigned i = 0; i < 400; ++i) {
        const auto info = MakeImage(1 + random() % 513, 1 + random() % 513, 1 + random() % 6,
                                    random() % 5, formats[i % formats.size()]);
        const std::array ranges{std::pair<u64, u64>{0, CalculateGuestSizeInBytes(info)}};
        const auto rects = SparseTileRects(info, ranges, 128, 64, info.resources.levels / 2);
        u64 expected{};
        for (s32 level = 0; level < info.resources.levels; ++level) {
            const auto size = MipSize(info.size, level);
            expected += u64{size.width} * size.height;
        }
        CHECK(Coverage(info, rects) == expected);
        CHECK(UnswizzledRectsBytes(info, rects) >=
              expected * VideoCore::Surface::BytesPerBlock(info.format));
    }
}

TEST_CASE("Sparse terrain masks prepare only their mapped tile area", "[sparse_tiles]") {
    const auto info = MakeImage(16384, 16384, 1, 4);
    const std::array mapped{std::pair<u64, u64>{0, 65536}};
    const auto rects = SparseTileRects(info, mapped, 128, 64, 1);
    CHECK(Coverage(info, rects) >= 65536);
    CHECK(UnswizzledRectsBytes(info, rects) < 1024 * 1024);
    CHECK(CalculateGuestSizeInBytes(info) >= 256 * 1024 * 1024);
}
