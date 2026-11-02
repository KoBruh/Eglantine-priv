// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

bool Contains(const std::vector<u64>& words, u64 word) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

// Encodings from Pokopia's night compute shaders, which were rejected before.
TEST_CASE("Ampere SHF.L.U32.HI with an immediate clamps the shift without .W",
          "[video_core][ampere]") {
    // SHF.L.U32.HI R12, RZ, R11, 0xff: imm << R11, 0 from a shift of 32 on.
    const std::array<Instruction, 2> code{{{{0xff0c7419, 0x000000ff, 0x0001060b, 0x080fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered.instructions, 0x010000000ff7f0faULL)); // MOV32I R250, 0xff
    REQUIRE(Contains(lowered.instructions, 0x5c48000000b7fa0cULL)); // SHL R12, R250, R11 (clamps)
}

TEST_CASE("Ampere SHF.L.W.U32.HI with an immediate still wraps", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{{{0xff0c7419, 0x000000ff, 0x00010e0b, 0x080fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered.instructions, 0x5c48008000b7fa0cULL)); // SHL.W R12, R250, R11
}

TEST_CASE("Ampere I2F.F32.U8 converts the selected source byte", "[video_core][ampere]") {
    // I2F.F32.U8 R12, R0.B1 and R59, R33.B2: Maxwell I2F's selector (bits 41-42) in bytes.
    const std::array<Instruction, 2> b1{{{{0x000c7306, 0x10000000, 0x00001000, 0x000e7000}}, Exit}};
    const auto lowered_b1 = LowerToMaxwell(b1);
    REQUIRE(static_cast<bool>(lowered_b1));
    REQUIRE(Contains(lowered_b1.instructions, 0x5cb802000007020cULL));

    const std::array<Instruction, 2> b2{{{{0x003b7306, 0x20000021, 0x00001000, 0x000e2200}}, Exit}};
    const auto lowered_b2 = LowerToMaxwell(b2);
    REQUIRE(static_cast<bool>(lowered_b2));
    REQUIRE(Contains(lowered_b2.instructions, 0x5cb804000217023bULL));
}

TEST_CASE("Ampere USHF by an immediate shifts the register a uniform one lives in",
          "[video_core][ampere]") {
    // USHF.R.U32.HI UR4, URZ, 0x2, UR16 = UR16 >> 2; UR4 is R220, UR16 R232.
    const std::array<Instruction, 2> code{{{{0x3f047899, 0x00000002, 0x08011610, 0x000fcc00}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered.instructions, 0x382800000027e8dcULL)); // SHR R220, R232, 2
}

TEST_CASE("Ampere UPRMT permutes uniform registers", "[video_core][ampere]") {
    // UPRMT UR6, UR8, 0x9910, URZ: UR8's low half sign-extended, into UR6 (R222).
    const std::array<Instruction, 2> code{{{{0x08067896, 0x00009910, 0x0800003f, 0x000fc800}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered.instructions, 0x5c1000000ff7fadeULL)); // IADD R222, R250, RZ
}

TEST_CASE("Ampere DEPBAR waits on any scoreboard set", "[video_core][ampere]") {
    // DEPBAR.LE SB0, 0x0, {3,2,1} and DEPBAR.LE SB5, 0x1: no code, the IR orders the results.
    for (const u32 word : {0x0000800eU, 0x0000d040U}) {
        const std::array<Instruction, 2> code{{{{0x0000791a, word, 0, 0x000fce00}}, Exit}};
        REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
    }
}

TEST_CASE("Ampere I2F.F32.U8 of the low byte is unchanged", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{{{0x000c7306, 0x00000000, 0x00001000, 0x000e7000}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered.instructions, 0x5cb800000007020cULL));
}
} // namespace
