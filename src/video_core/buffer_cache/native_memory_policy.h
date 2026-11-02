// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace VideoCommon {

inline std::uint64_t NativeBufferCacheThreshold(std::uint64_t budget, std::uint64_t usage,
                                                std::uint64_t free_commit) {
    constexpr std::uint64_t GiB = std::uint64_t{1} << 30;
    if (budget >= 12 * GiB && budget >= usage && budget - usage >= 2 * GiB &&
        free_commit >= 8 * GiB) {
        return GiB;
    }
    return GiB / 2;
}

} // namespace VideoCommon
