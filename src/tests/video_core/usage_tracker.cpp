// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"

namespace {
constexpr u64 BufferBytes = 8192;

class IntervalOracle {
public:
    void Reset() {
        std::ranges::fill(lines, false);
    }

    void Mark(u64 offset, u64 size) {
        if (size == 0) {
            return;
        }
        for (u64 byte = offset; byte < offset + size; ++byte) {
            lines[static_cast<size_t>(byte / 64)] = true;
        }
    }

    bool Used(u64 offset, u64 size) const {
        if (size == 0) {
            return false;
        }
        for (u64 byte = offset; byte < offset + size; ++byte) {
            if (lines[static_cast<size_t>(byte / 64)]) {
                return true;
            }
        }
        return false;
    }

private:
    std::array<bool, static_cast<size_t>(BufferBytes / 64)> lines{};
};

std::vector<std::string> CompareRanges(VideoCommon::UsageTracker& tracker,
                                       const IntervalOracle& oracle,
                                       std::span<const std::pair<u64, u64>> queries) {
    std::vector<std::string> failures;
    for (const auto& [offset, size] : queries) {
        const bool expected = oracle.Used(offset, size);
        const bool actual = tracker.IsUsed(offset, size);
        if (actual != expected) {
            failures.push_back(std::to_string(offset) + "+" + std::to_string(size) + " expected=" +
                               std::to_string(expected) + " actual=" + std::to_string(actual));
        }
    }
    return failures;
}
} // namespace

TEST_CASE("UsageTracker covers touched 64-byte ranges", "[video_core][usage_tracker]") {
    constexpr std::array<std::pair<u64, u64>, 13> ranges{{
        {1, 1},
        {32, 64},
        {63, 2},
        {64, 64},
        {65, 63},
        {4095, 2},
        {4032, 128},
        {0, 4096},
        {64, 4096},
        {4096, 4096},
        {0, BufferBytes},
        {8191, 1},
        {0, 0},
    }};
    constexpr std::array<u64, 14> offsets{
        0, 1, 31, 32, 63, 64, 65, 127, 128, 4095, 4096, 4097, 8189, 8191,
    };
    constexpr std::array<u64, 10> sizes{0, 1, 2, 31, 63, 64, 65, 127, 4095, 8192};
    std::vector<std::pair<u64, u64>> queries;
    for (const u64 offset : offsets) {
        for (const u64 size : sizes) {
            if (size <= BufferBytes - offset) {
                queries.emplace_back(offset, size);
            }
        }
    }
    for (const auto& range : ranges) {
        queries.push_back(range);
    }

    std::string first_failures;
    size_t failure_count = 0;
    for (const auto& [offset, size] : ranges) {
        VideoCommon::UsageTracker tracker{BufferBytes};
        IntervalOracle oracle;
        tracker.Track(offset, size);
        oracle.Mark(offset, size);
        const auto failures = CompareRanges(tracker, oracle, queries);
        failure_count += failures.size();
        for (const auto& failure : failures) {
            if (first_failures.size() < 900) {
                first_failures += "track=" + std::to_string(offset) + "+" + std::to_string(size) +
                                  " query=" + failure + "\n";
            }
        }
    }
    INFO("interval mismatches=" << failure_count << "\n" << first_failures);
    CHECK(failure_count == 0);
}

TEST_CASE("UsageTracker reset clears every tracked interval", "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{BufferBytes};
    IntervalOracle oracle;
    tracker.Track(32, 64);
    oracle.Mark(32, 64);
    CHECK(tracker.IsUsed(64, 64)); // The second touched 64-byte line is used.

    tracker.Reset();
    oracle.Reset();
    constexpr std::array<std::pair<u64, u64>, 5> queries{{
        {0, 1},
        {32, 64},
        {64, 64},
        {4095, 2},
        {BufferBytes - 1, 1},
    }};
    const auto failures = CompareRanges(tracker, oracle, queries);
    INFO("reset mismatches=" << failures.size());
    CHECK(failures.empty());

    tracker.Track(32, 64);
    oracle.Mark(32, 64);
    CHECK(tracker.IsUsed(64, 64));
}
