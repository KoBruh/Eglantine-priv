// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

// Tracks how many ASTC transcode bytes were submitted in the current tick, so a batch close to
// the budget can be flushed early instead of waiting for TickFrame.
class TranscodeSubmissionBatch {
public:
    constexpr void Record(u64 current_tick, u64 image_bytes) noexcept {
        if (tick != current_tick) {
            tick = current_tick;
            bytes = 0;
        }
        bytes += image_bytes;
    }

    [[nodiscard]] constexpr u64 BytesForTick(u64 current_tick) const noexcept {
        return tick == current_tick ? bytes : 0;
    }

    [[nodiscard]] constexpr bool ShouldSubmitEarly(u64 pending_bytes, u64 budget_bytes,
                                                   u64 oldest_tick, u64 current_tick) const noexcept {
        return budget_bytes != 0 && tick == current_tick && bytes != 0 &&
               bytes >= budget_bytes / 2 &&
               (pending_bytes <= budget_bytes || oldest_tick < current_tick);
    }

private:
    u64 tick{};
    u64 bytes{};
};

} // namespace Vulkan
