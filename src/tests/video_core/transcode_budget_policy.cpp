// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/transcode_budget_policy.h"

TEST_CASE("Transcode batches are submitted before the memory limit", "[video_core][memory]") {
    constexpr u64 budget = 480;
    Vulkan::TranscodeSubmissionBatch batch;
    batch.Record(11, 239);

    REQUIRE_FALSE(batch.ShouldSubmitEarly(239, budget, 11, 11));
    batch.Record(11, 1);
    REQUIRE(batch.ShouldSubmitEarly(240, budget, 11, 11));

    // A submission advances the tick, so a new batch starts at zero even while the old one runs.
    REQUIRE_FALSE(batch.ShouldSubmitEarly(240, budget, 11, 12));
    REQUIRE(batch.BytesForTick(12) == 0);
    batch.Record(12, 239);
    REQUIRE_FALSE(batch.ShouldSubmitEarly(479, budget, 11, 12));
    batch.Record(12, 1);
    REQUIRE(batch.ShouldSubmitEarly(480, budget, 11, 12));

    // An over-budget queue may submit the current batch while waiting for an older batch.
    batch.Record(13, 240);
    REQUIRE(batch.ShouldSubmitEarly(600, budget, 11, 13));
    // If the current batch is oldest, the existing hard-budget wait will submit it instead.
    batch.Record(14, 481);
    REQUIRE_FALSE(batch.ShouldSubmitEarly(481, budget, 14, 14));
    REQUIRE_FALSE(batch.ShouldSubmitEarly(481, 0, 14, 14));
    batch.Record(15, 0);
    REQUIRE_FALSE(batch.ShouldSubmitEarly(0, 1, 15, 15));
}
