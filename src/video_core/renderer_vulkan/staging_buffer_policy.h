// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <span>

namespace Vulkan {

inline constexpr std::size_t LargeStagingBufferSize = 16ULL << 20;

// Below 16 MiB, round up to the next power of two as before. From 16 MiB up, round to 64 KiB
// steps instead: doubling a large buffer wastes far more than a 64 KiB-aligned allocation does.
[[nodiscard]] constexpr std::size_t StagingBufferAllocationSize(std::size_t requested) {
    if (requested < LargeStagingBufferSize) {
        return std::bit_ceil((std::max)(requested, std::size_t{1}));
    }
    constexpr std::size_t granularity = 64ULL << 10;
    const std::size_t padding = (granularity - requested % granularity) % granularity;
    return padding <= (std::numeric_limits<std::size_t>::max)() - requested
               ? requested + padding
               : requested;
}

// The mapped span handed to callers covers only what was requested, not the buffer's rounded
// capacity, so callers never read past the request into memory that was never written.
template <typename T>
[[nodiscard]] std::span<T> StagingBufferMappedRange(std::span<T> mapping, std::size_t requested) {
    return mapping.empty() ? mapping : mapping.first(requested);
}

// Caps how many idle large buffers (>= LargeStagingBufferSize) a staging cache level keeps
// around per usage type, so a transient spike of large transfers doesn't pin tens of MiB idle.
class StagingBufferIdleBudget {
public:
    [[nodiscard]] bool ShouldRelease(std::size_t capacity, bool deferred, bool gpu_complete) {
        if (deferred || !gpu_complete) {
            return false;
        }
        if (capacity > MaxIdleBytes - retained_bytes) {
            return true;
        }
        retained_bytes += capacity;
        return false;
    }

private:
    static constexpr std::size_t MaxIdleBytes = 64ULL << 20;
    std::size_t retained_bytes{};
};

} // namespace Vulkan
