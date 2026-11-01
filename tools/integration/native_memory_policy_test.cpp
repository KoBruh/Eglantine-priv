// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <iostream>
#include <limits>
#include "video_core/buffer_cache/native_memory_policy.h"

int main() {
    constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
    using VideoCommon::NativeBufferCacheThreshold;
    assert(NativeBufferCacheThreshold(47 * GiB, 14 * GiB, 13 * GiB) == GiB);
    assert(NativeBufferCacheThreshold(8 * GiB, 2 * GiB, 16 * GiB) == GiB / 2);
    assert(NativeBufferCacheThreshold(47 * GiB, 46 * GiB, 13 * GiB) == GiB / 2);
    assert(NativeBufferCacheThreshold(47 * GiB, 14 * GiB, 7 * GiB) == GiB / 2);
    assert(NativeBufferCacheThreshold(0, 0, 0) == GiB / 2);
    assert(NativeBufferCacheThreshold(12 * GiB, 10 * GiB, 8 * GiB) == GiB);
    assert(NativeBufferCacheThreshold(12 * GiB, 13 * GiB, 16 * GiB) == GiB / 2);
    assert(NativeBufferCacheThreshold(std::numeric_limits<std::uint64_t>::max(), 0, 0) == GiB / 2);
    std::cout << "Native memory policy: target VM, budget pressure and unknown "
                 "budgets passed\n";
}
