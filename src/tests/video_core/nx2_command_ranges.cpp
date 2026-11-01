// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

namespace {
constexpr GPUVAddr GpuBase = 0x8600000000;
constexpr DAddr DeviceBase = 0x120000000;
constexpr size_t PageSize = size_t{1} << Tegra::Nx2WrittenPageBits;
}

TEST_CASE("GPU-written command ranges translate pages before checking the bitmap",
          "[video_core][nx2_command_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    const auto translate = [](GPUVAddr page) -> std::optional<DAddr> {
        return DeviceBase + page - GpuBase;
    };
    CHECK_FALSE(Tegra::Nx2HasGpuWrittenPage(GpuBase, PageSize, translate));
    Tegra::Nx2MarkGpuWritten(DeviceBase, PageSize);
    CHECK(Tegra::Nx2HasGpuWrittenPage(GpuBase, sizeof(u32), translate));
    CHECK(Tegra::Nx2HasGpuWrittenPage(GpuBase + PageSize - sizeof(u32), sizeof(u32), translate));
    CHECK_FALSE(Tegra::Nx2HasGpuWrittenPage(GpuBase + PageSize, sizeof(u32), translate));
}

TEST_CASE("GPU-written command ranges include partial pages but exclude the end boundary",
          "[video_core][nx2_command_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkGpuWritten(DeviceBase + PageSize, 1);
    size_t translations{};
    const auto translate = [&](GPUVAddr page) -> std::optional<DAddr> {
        ++translations;
        return DeviceBase + page - GpuBase;
    };
    CHECK_FALSE(Tegra::Nx2HasGpuWrittenPage(GpuBase + PageSize - 4, 4, translate));
    CHECK(translations == 1);
    translations = 0;
    CHECK(Tegra::Nx2HasGpuWrittenPage(GpuBase + PageSize - 4, 5, translate));
    CHECK(translations == 2);
    CHECK(Tegra::Nx2HasGpuWrittenPage(GpuBase + PageSize + 1, 1, translate));
}

TEST_CASE("GPU-written command ranges skip unmapped pages and retain historical writes",
          "[video_core][nx2_command_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkGpuWritten(DeviceBase, 1);
    const auto translate = [](GPUVAddr page) -> std::optional<DAddr> {
        if (page == GpuBase) {
            return std::nullopt;
        }
        return DeviceBase;
    };
    CHECK_FALSE(Tegra::Nx2HasGpuWrittenPage(GpuBase, PageSize, translate));
    for (u32 frame = 0; frame < 3; ++frame) {
        CHECK(Tegra::Nx2HasGpuWrittenPage(GpuBase, PageSize + 1, translate));
    }
    Tegra::Nx2ClearGpuWrittenPages();
    CHECK_FALSE(Tegra::Nx2HasGpuWrittenPage(GpuBase, PageSize + 1, translate));
}

TEST_CASE("GPU-written command range guards avoid empty scans and wrapped ranges",
          "[video_core][nx2_command_sync]") {
    size_t translations{};
    const auto translate = [&](GPUVAddr) -> std::optional<DAddr> {
        ++translations;
        return std::nullopt;
    };
    CHECK_FALSE(Tegra::Nx2HasGpuWrittenPage(GpuBase, 0, translate));
    CHECK(Tegra::Nx2HasGpuWrittenPage(std::numeric_limits<GPUVAddr>::max() - 1, 4, translate));
    CHECK(translations == 0);
}

TEST_CASE("GPU-written command ranges retranslate after a GPU address is remapped",
          "[video_core][nx2_command_sync][mkw_sync]") {
    Tegra::Nx2ClearGpuWrittenPages();
    DAddr backing = DeviceBase;
    const auto translate = [&](GPUVAddr page) -> std::optional<DAddr> {
        return backing + page - GpuBase;
    };
    Tegra::Nx2MarkGpuWritten(DeviceBase, PageSize);
    REQUIRE(Tegra::Nx2HasGpuWrittenPage(GpuBase, 5 * sizeof(u32), translate));
    backing += PageSize;
    REQUIRE_FALSE(Tegra::Nx2HasGpuWrittenPage(GpuBase, 5 * sizeof(u32), translate));
    Tegra::Nx2MarkGpuWritten(backing, 5 * sizeof(u32));
    REQUIRE(Tegra::Nx2HasGpuWrittenPage(GpuBase, 5 * sizeof(u32), translate));
    backing = DeviceBase;
    REQUIRE(Tegra::Nx2HasGpuWrittenPage(GpuBase, 5 * sizeof(u32), translate));
}
