// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include "video_core/buffer_cache/usage_tracker.h"

int main() {
    constexpr u64 Size = 16384;
    VideoCommon::UsageTracker tracker{Size};
    std::array<bool, Size / 64> touched{};
    std::mt19937 random{0x1234};
    size_t checks = 0;
    for (size_t iteration = 0; iteration < 1000; ++iteration) {
        if (iteration % 7 == 0) {
            tracker.Reset();
            touched.fill(false);
        }
        const u64 begin = random() % Size;
        const u64 length = random() % (Size - begin + 1);
        tracker.Track(begin, length);
        for (u64 byte = begin; byte < begin + length; ++byte) touched[byte / 64] = true;
        for (size_t query = 0; query < 100; ++query) {
            const u64 offset = random() % Size;
            const u64 size = random() % (Size - offset + 1);
            bool expected = false;
            for (u64 byte = offset; byte < offset + size; ++byte) expected |= touched[byte / 64];
            if (tracker.IsUsed(offset, size) != expected) {
                std::fprintf(stderr, "Usage mismatch at %llu+%llu\n",
                             static_cast<unsigned long long>(offset),
                             static_cast<unsigned long long>(size));
                return 1;
            }
            ++checks;
        }
    }
    for (const u64 offset : {0ULL, 1ULL, 63ULL, 64ULL, 4095ULL, 4096ULL, Size - 1}) {
        tracker.Reset();
        tracker.Track(offset, 1);
        if (!tracker.IsUsed(offset, 1) || tracker.IsUsed(offset, 0)) return 2;
    }
    tracker.Reset();
    tracker.Track(std::numeric_limits<u64>::max() - 1, 4);
    if (!tracker.IsUsed(0, Size)) return 3;
    tracker.Reset();
    tracker.Track(Size, 0);
    if (tracker.IsUsed(0, Size)) return 4;
    std::printf("Usage tracker: %zu differential queries, boundary and overflow checks passed.\n", checks);
}
