// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Ampere HFMA2 .H0_NH1 swaps the second operand's halves and negates the low one",
          "[video_core][ampere]") {
    // HFMA2 R4 = R1 * R2.H0_NH1 + R3 (bit 86): the high lane reads R2.H0, the low lane -R2.H1.
    const std::array<Instruction, 2> code{{
        {{0x01047231, 0x00000002, 0x00400003, 0}},
        Exit,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    constexpr u64 always = u64{7} << 16;
    const std::array<u64, 4> expected{
        0x3828000000000000ULL | always | 250 | (u64{2} << 8) | (u64{16} << 20), // R250 = R2 >> 16
        0x3848000000000000ULL | always | 251 | (u64{2} << 8) | (u64{16} << 20), // R251 = R2 << 16
        0x5c40000000000000ULL | always | 251 | (u64{251} << 8) | (u64{250} << 20) |
            (u64{1} << 41) | (u64{7} << 48),                                     // R251 |= R250
        0x0400000000000000ULL | always | 251 | (u64{251} << 8) | (u64{0x8000} << 20) |
            (u64{2} << 53),                                                      // R251 ^= 0x8000
    };
    const auto& words = lowered.instructions;
    const auto found = std::search(words.begin(), words.end(), expected.begin(), expected.end());
    REQUIRE(found != words.end());
    // The multiply-add after it reads the swapped copy as its second operand.
    const auto after = found + expected.size();
    REQUIRE(after != words.end());
    REQUIRE(((*after >> 20) & 0xff) == 251);
    // Nothing negates the upper half alone any more.
    REQUIRE(std::none_of(words.begin(), words.end(), [](u64 word) {
        return (word >> 58) == (0x0400000000000000ULL >> 58) && ((word >> 20) & 0xffffffff) == 0x80000000;
    }));
}
} // namespace
