// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>

#include "video_core/nx2.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

int main() {
    constexpr u64 Page = 4096;
    constexpr GPUVAddr Guest = 0x8600000000;
    constexpr DAddr Device = 0x120000000;
    DAddr backing = Device;
    size_t translations{};
    const auto translate = [&](GPUVAddr address) -> std::optional<DAddr> {
        ++translations;
        return backing + address - Guest;
    };
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkGpuWritten(Device + Page, 1);
    Check(!Tegra::Nx2HasGpuWrittenPage(Guest + Page - 4, 4, translate),
          "scan included the page after its exclusive end");
    Check(Tegra::Nx2HasGpuWrittenPage(Guest + Page - 4, 5, translate),
          "scan missed a partially covered written page");
    backing += Page * 8;
    Check(!Tegra::Nx2HasGpuWrittenPage(Guest + Page, 4, translate),
          "scan reused a stale physical mapping");
    backing = Device;
    Check(Tegra::Nx2HasGpuWrittenPage(Guest + Page, 4, translate),
          "historical write was lost when restoring the mapping");
    Tegra::Nx2ClearGpuWrittenPages();
    Check(!Tegra::Nx2HasGpuWrittenPage(Guest + Page, 4, translate),
          "session reset retained GPU writes");

    for (const u64 first_page : {0ULL, 1ULL, 63ULL, 64ULL, 127ULL, 130ULL}) {
        for (const u64 pages : {1ULL, 2ULL, 63ULL, 64ULL, 65ULL, 130ULL}) {
            Tegra::Nx2ClearGpuWrittenPages();
            Tegra::Nx2MarkGpuWritten(first_page * Page + 17, pages * Page - 18);
            for (u64 page = 0; page < 300; ++page) {
                const bool expected = page >= first_page && page < first_page + pages;
                Check(Tegra::Nx2IsGpuWritten(page * Page) == expected,
                      "batched bitmap write changed the wrong pages");
            }
        }
    }
    translations = 0;
    Check(!Tegra::Nx2HasGpuWrittenPage(Guest, 0, translate), "empty scan was not empty");
    Check(Tegra::Nx2HasGpuWrittenPage(std::numeric_limits<GPUVAddr>::max() - 1, 4, translate),
          "overflowing command range was treated as clean");
    Check(translations == 0, "invalid range was translated");
    Check(!Tegra::Nx2HasGpuWrittenPage(Guest, Page,
                                     [](GPUVAddr) -> std::optional<DAddr> { return {}; }),
          "unmapped command memory was treated as mapped");
    std::puts("GPU command ranges: remapping, boundaries, bitmap batches, reset, and overflow passed.");
}
