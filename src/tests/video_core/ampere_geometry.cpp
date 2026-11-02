// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
std::vector<Instruction> GeometryProgram() {
    return {{{{0x0000734e, 0, 0, 0}}},          {{{0x00037802, 0x70, 0xf00, 0}}},
            {{{0x00007210, 3, 0x07f1e0ff, 0}}}, {{{0x01017210, 0xff, 0x007fe4ff, 0}}},
            {{{0x00007357, 0, 0, 0}}},          {{{0x0000794d, 0, 0x03800000, 0}}},
            {{{0x00007918, 0, 0, 0}}},          {{{0x00ff7324, 255, 0, 0}}},
            {{{0x00007957, 0, 0, 0}}},          {{{0x0000794d, 0, 0x03800000, 0}}}};
}
} // namespace

TEST_CASE("Native capture retains ATEXIT finalization", "[ampere-geometry]") {
    const auto code = GeometryProgram();
    Shader::Ampere::ProgramCaptureState capture;
    for (u32 i = 0; i < code.size(); ++i) {
        const auto result = capture.Observe(code[i], i);
        REQUIRE(result == (i + 1 == code.size()
                               ? Shader::Ampere::ProgramCaptureState::Result::Complete
                               : Shader::Ampere::ProgramCaptureState::Result::Continue));
    }
}

TEST_CASE("Native geometry finalization needs a geometry stage and a verified exit envelope",
          "[ampere-geometry]") {
    // The single-invocation lowering this program was written for (LEPC materializing
    // code_address, OUT.FINAL as EndPrimitive) was dropped; those options are unused. Outside a
    // geometry stage the static ATEXIT path routes the EXIT into the handler and stops at its OUT.
    // A geometry stage takes the exit envelope, which needs an EXIT right before the handler.
    const auto code = GeometryProgram();
    const auto lowered = Shader::Ampere::LowerToMaxwell(
        code, {.code_address = 0x11fffffff0ULL, .single_invocation_geometry = true});
    REQUIRE_FALSE(lowered);
    REQUIRE(lowered.error_offset == 7 * 16);
    REQUIRE(lowered.error == "native geometry output outside geometry stage");
    const auto geometry = Shader::Ampere::LowerToMaxwell(code, {.geometry_stage = true});
    REQUIRE_FALSE(geometry);
    REQUIRE(geometry.error_offset == 6 * 16);
    REQUIRE(geometry.error == "native exit handler reached by fallthrough");
}

TEST_CASE("Native ATEXIT requires a complete verified epilogue", "[ampere-geometry]") {
    auto code = GeometryProgram();
    code.pop_back();
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
    code = GeometryProgram();
    code[8].words[1] = 1;
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
    code = GeometryProgram();
    code[1].words[1] = 0x71;
    // An unaligned handler is no static ATEXIT registration: outside a geometry stage the prologue
    // lowers as ordinary code and the program ends at its first EXIT. The geometry exit envelope
    // rejects the handler location.
    const auto unaligned = Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true});
    REQUIRE(unaligned);
    REQUIRE(unaligned.source_size == 6 * 16);
    const auto geometry = Shader::Ampere::LowerToMaxwell(code, {.geometry_stage = true});
    REQUIRE_FALSE(geometry);
    REQUIRE(geometry.error_offset == 0x10);
    REQUIRE(geometry.error == "native exit handler location");
}

TEST_CASE("Native input entry reads lower as moves only in a geometry stage", "[ampere-geometry]") {
    std::vector<Instruction> code{{{0x00007919, 0, 0x1d00, 0}},
                                  {{0x00007816, 0x7770, 255, 0}},
                                  {{0x01007224, 0, 0x078e02ff, 0}},
                                  {{0x00077923, 0, 0, 0}},
                                  {{0x0000794d, 0, 0x03800000, 0}}};
    // The single-invocation proof was dropped (the option is unused): a geometry stage lowers
    // ISBERD as a move of its index, for any input entry; outside one the invocation read rejects.
    const auto lowered = Shader::Ampere::LowerToMaxwell(code, {.geometry_stage = true});
    REQUIRE(lowered);
    REQUIRE(lowered.instructions[lowered.source_instruction_offsets[3]] ==
            (0x5c98000000070000ULL | 7ULL | (15ULL << 39)));
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code));
    REQUIRE_FALSE(Shader::Ampere::LowerToMaxwell(code, {.single_invocation_geometry = true}));
    auto with_zero_pair = code;
    with_zero_pair.insert(with_zero_pair.begin() + 1, Instruction{{0x00287805, 0, 0x0001ff00, 0}});
    REQUIRE(Shader::Ampere::LowerToMaxwell(with_zero_pair, {.geometry_stage = true}));
    code[1].words[1] = 0x7772;
    REQUIRE(Shader::Ampere::LowerToMaxwell(code, {.geometry_stage = true}));
}

TEST_CASE("Geometry local-memory uniforms retain real addresses", "[ampere-geometry]") {
    const std::array store{Instruction{{0xff007987, 0x4004, 0x08100c08, 0}},
                           Instruction{{0x794d, 0, 0x03800000, 0}}};
    const auto written = Shader::Ampere::LowerToMaxwell(store);
    REQUIRE(written);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE((written.instructions[i] & 255) == 4 + i);
        REQUIRE(((written.instructions[i] >> 8) & 255) == 224);
        REQUIRE(((written.instructions[i] >> 20) & 0xffffff) == 0x40 + i * 4);
    }
    const std::array load{Instruction{{0xff337983, 0x6c08, 0x08100800, 0}},
                          Instruction{{0x794d, 0, 0x03800000, 0}}};
    const auto read = Shader::Ampere::LowerToMaxwell(load);
    REQUIRE(read);
    REQUIRE((read.instructions.front() & 255) == 51);
    REQUIRE(((read.instructions.front() >> 8) & 255) == 224);
    REQUIRE(((read.instructions.front() >> 20) & 0xffffff) == 0x6c);
}

TEST_CASE("Geometry wide funnel shift retains both input words", "[ampere-geometry]") {
    for (const u32 shift : {0U, 1U, 16U, 31U, 32U}) {
        const std::array code{Instruction{{0x100f7819, shift, 0x0001020f, 0}},
                              Instruction{{0x794d, 0, 0x03800000, 0}}};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE(lowered);
        REQUIRE(static_cast<u32>(lowered.instructions.front() >> 20) == shift);
        const u64 word = lowered.instructions[1];
        REQUIRE(((word >> 8) & 255) == 16);
        REQUIRE(((word >> 39) & 255) == 15);
        REQUIRE(((word >> 37) & 3) == 2);
        REQUIRE(((word >> 20) & 255) == 251);
        for (const u32 low : {0U, 0x12345678U, 0xffffffffU})
            for (const u32 high : {0U, 0x87654321U, 0xffffffffU}) {
                const u32 result = static_cast<u32>(((u64{high} << 32 | low) << shift) >> 32);
                const u32 separate = shift == 0    ? high
                                     : shift == 32 ? low
                                                   : (high << shift) | (low >> (32 - shift));
                REQUIRE(result == separate);
            }
    }
}

TEST_CASE("Geometry half-word local stores preserve adjacent bytes", "[ampere-geometry]") {
    const std::array code{Instruction{{0xff007987, 0x9488, 0x08100408, 0}},
                          Instruction{{0x794d, 0, 0x03800000, 0}}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(lowered);
    const u64 word = lowered.instructions.front();
    REQUIRE((word & 255) == 136);
    REQUIRE(((word >> 8) & 255) == 224);
    REQUIRE(((word >> 20) & 0xffffff) == 0x94);
    REQUIRE(((word >> 48) & 7) == 2);
}

TEST_CASE("Geometry indexed vector loads preserve aliased addresses", "[ampere-geometry]") {
    for (u32 address : {26U, 43U}) {
        const std::array code{Instruction{{0x001a7983 | (address << 24), 0xc800, 0x00100a00, 0}},
                              Instruction{{0x794d, 0, 0x03800000, 0}}};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE(lowered);
        const u32 start = address == 26 ? 1 : 0;
        if (start) {
            REQUIRE((lowered.instructions[0] & 255) == 251);
            REQUIRE(((lowered.instructions[0] >> 20) & 255) == address);
        }
        for (u32 i = 0; i < 2; ++i) {
            const auto word = lowered.instructions[start + i];
            REQUIRE((word & 255) == 26 + i);
            REQUIRE(((word >> 8) & 255) == (start ? 251 : address));
            REQUIRE(((word >> 20) & 0xffffff) == 0xc8 + i * 4);
        }
    }
}

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::ProgramCaptureState;

constexpr std::array<Instruction, 10> ExitHandlerProgram{{
    {{0x0000734e, 0, 0, 0x000fe400}},
    {{0x00037802, 0x70, 0x00000f00, 0x000fc800}},
    {{0x00007210, 3, 0x07f1e0ff, 0x000fc800}},
    {{0x01017210, 0xff, 0x007fe4ff, 0x000fc800}},
    {{0x00007357, 0, 0, 0x0001e400}},
    {{0x0000894d, 0, 0x03800000, 0x021fea00}},
    {{0x0000794d, 0, 0x03800000, 0x003fea00}},
    {{0x00ff7324, 0xff, 0, 0x0001e200}},
    {{0x00007957, 0, 0, 0x000fe200}},
    {{0x0000794d, 0, 0x03800000, 0x001fea00}},
}};

constexpr auto BananzaExitHandlerProgram = [] {
    std::array<Instruction, 0x740 / 16> code{};
    code.fill(Instruction{{0x00007918, 0, 0, 0x000fc000}});
    std::copy_n(ExitHandlerProgram.begin(), 5, code.begin());
    code[1].words[1] = 0x710;
    code[2].words[3] = 0x000fca00;
    code[3] = Instruction{{0xff017224, 0xff, 0x000e0401, 0x000fc800}};
    code[0x6e0 / 16] = ExitHandlerProgram[6];
    code[0x6f0 / 16] = Instruction{{0xff007322, 0x0003a0ff, 0, 0x0001e400}};
    code[0x700 / 16] = ExitHandlerProgram[6];
    std::copy_n(ExitHandlerProgram.begin() + 7, 3, code.begin() + 0x710 / 16);
    return code;
}();

TEST_CASE("Ampere IMAD exit capture includes the complete Bananza handler",
          "[video_core][ampere]") {
    ProgramCaptureState capture;
    for (u32 index = 0; index < BananzaExitHandlerProgram.size(); ++index) {
        const auto expected = index + 1 == BananzaExitHandlerProgram.size()
                                  ? ProgramCaptureState::Result::Complete
                                  : ProgramCaptureState::Result::Continue;
        REQUIRE(capture.Observe(BananzaExitHandlerProgram[index], index) == expected);
    }
    REQUIRE(sizeof(BananzaExitHandlerProgram) == 0x740);
}

TEST_CASE("Ampere IMAD exit capture requires exact carry fields", "[video_core][ampere]") {
    for (u32 word = 0; word < 3; ++word) {
        for (u32 bit = 0; bit < 32; ++bit) {
            auto code = BananzaExitHandlerProgram;
            code[3].words[word] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index <= 0x6e0 / 16; ++index) {
                const auto expected = index == 0x6e0 / 16
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere IMAD exit capture bounds the handler destination", "[video_core][ampere]") {
    for (const u32 target : {0U, 0x40U, 0x711U, ProgramCaptureState::MaximumInstructions * 16}) {
        auto code = BananzaExitHandlerProgram;
        code[1].words[1] = target;
        ProgramCaptureState capture;
        for (u32 index = 0; index < 4; ++index)
            REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
        REQUIRE(capture.Observe(code[4], 4) == ProgramCaptureState::Result::InvalidTarget);
    }
}

TEST_CASE("Ampere IMAD exit capture validates extended instruction modes",
          "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 30U, 31U}) {
            auto code = BananzaExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index <= 0x6e0 / 16; ++index) {
                const auto expected = index == 0x6e0 / 16
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere IMAD exit capture ignores scheduling fields", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit = 9; bit < 30; ++bit) {
            auto code = BananzaExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index < code.size(); ++index) {
                const auto expected = index + 1 == code.size()
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere IMAD exit capture does not bypass unsupported lowering",
          "[video_core][ampere]") {
    // A geometry stage takes the exit envelope, which rejects the unmodeled NOP the body reaches
    // while the exit setup state is still live.
    const auto geometry = Shader::Ampere::LowerToMaxwell(BananzaExitHandlerProgram, true);
    REQUIRE_FALSE(static_cast<bool>(geometry));
    REQUIRE(geometry.instructions.empty());
    REQUIRE(geometry.error_offset == 0x50);
    // Outside a geometry stage LEPC lowers as ordinary code (the envelope is gated on the stage)
    // and the IMAD form is no static ATEXIT registration, so the program ends at its first EXIT.
    const auto lowered = Shader::Ampere::LowerToMaxwell(BananzaExitHandlerProgram);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == 0x6f0);
}

TEST_CASE("Ampere capture includes the static geometry exit handler", "[video_core][ampere]") {
    ProgramCaptureState capture;
    for (u32 index = 0; index < ExitHandlerProgram.size(); ++index) {
        const auto expected = index + 1 == ExitHandlerProgram.size()
                                  ? ProgramCaptureState::Result::Complete
                                  : ProgramCaptureState::Result::Continue;
        REQUIRE(capture.Observe(ExitHandlerProgram[index], index) == expected);
    }
}

TEST_CASE("Ampere capture bounds the static exit destination", "[video_core][ampere]") {
    for (const u32 target : {0U, 0x40U, 0x71U, ProgramCaptureState::MaximumInstructions * 16}) {
        auto code = ExitHandlerProgram;
        code[1].words[1] = target;
        ProgramCaptureState capture;
        for (u32 index = 0; index < 4; ++index)
            REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
        REQUIRE(capture.Observe(code[4], 4) == ProgramCaptureState::Result::InvalidTarget);
    }
}

TEST_CASE("Ampere capture does not guess other exit setup forms", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        auto code = ExitHandlerProgram;
        code[setup].words[0] ^= 1U << 12;
        ProgramCaptureState capture;
        for (u32 index = 0; index < 6; ++index)
            REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
        REQUIRE(capture.Observe(code[6], 6) == ProgramCaptureState::Result::Complete);
    }
}

TEST_CASE("Ampere static exit capture does not bypass unsupported lowering", "[video_core][ampere]") {
    // Outside a geometry stage the static ATEXIT path routes the exits into the handler, whose
    // OUT is rejected there.
    const auto lowered = Shader::Ampere::LowerToMaxwell(ExitHandlerProgram);
    REQUIRE_FALSE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.empty());
    REQUIRE(lowered.error_offset == 7 * 16);
    REQUIRE(lowered.error == "native geometry output outside geometry stage");
}

TEST_CASE("Ampere static exit capture validates extended instruction modes", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 30U, 31U}) {
            auto code = ExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index < 6; ++index)
                REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
            REQUIRE(capture.Observe(code[6], 6) == ProgramCaptureState::Result::Complete);
        }
    }
}

TEST_CASE("Ampere static exit capture ignores scheduling fields", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit = 9; bit < 30; ++bit) {
            auto code = ExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index < code.size(); ++index) {
                const auto expected = index + 1 == code.size()
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere geometry loop transfer is bounded to a nonreturning call pattern",
          "[video_core][ampere]") {
    const std::array<Instruction, 4> code{{
        {{0x00003944, 16, 0x03c00000, 0}},
        {{0x00007947, 0xffffffe0, 0x0383ffff, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    REQUIRE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(code)));
    auto returning = code;
    returning[2].words[0] = 0x00007950;
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(returning)));
    auto conditional_branch = code;
    conditional_branch[1].words[0] = 0x00001947;
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(conditional_branch)));
    auto extra_condition = code;
    extra_condition[0].words[2] ^= 1U << 24;
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(extra_condition)));
    for (u32 instruction = 0; instruction < 2; ++instruction) {
        for (u32 bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 30U, 31U}) {
            auto extended_mode = code;
            extended_mode[instruction].words[3] ^= 1U << bit;
            REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(extended_mode)));
        }
    }
}

constexpr std::array<Instruction, 8> TerminalFloatTailProgram{{
    {{0x00027802, 0x20, 0x00000f00, 0}},
    {{0x00007944, 0x50, 0x03c00000, 0}},
    {{0x00000944, 0x10, 0x03c00000, 0}},
    {{0x00007947, 0xffffffe0, 0x0383ffff, 0}},
    {{0xff007209, 0x21, 0x07810000, 0x000fe400}},
    {{0x01017809, 0x447a0000, 0x03810000, 0x000fe400}},
    {{0x0000794d, 0, 0x03800000, 0x000fec00}},
    {{0x02007950, 0xffffff80, 0x03c3ffff, 0}},
}};

TEST_CASE("Ampere terminal minmax tails allow an unrelated linked return",
          "[video_core][ampere]") {
    const auto lowered = Shader::Ampere::LowerToMaxwell(TerminalFloatTailProgram);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(TerminalFloatTailProgram));
    REQUIRE(lowered.instructions.back() == 0xe32000000007000fULL);
}

TEST_CASE("Ampere terminal minmax proof rejects intervening control and unknown state",
          "[video_core][ampere]") {
    for (const Instruction intervening : {
             Instruction{{0x02007950, 0xffffffb0, 0x03c3ffff, 0}},
             Instruction{{0x00007944, 0, 0x03c00000, 0}},
             Instruction{{0x00007947, 0, 0x03800000, 0}},
             Instruction{{0x00007949, 0xffffffb0, 0x0383ffff, 0}},
             Instruction{{0x00007958, 0xffffffb0, 0x0b83ffff, 0}},
             Instruction{{0x0000734e, 0, 0, 0}},
             Instruction{{0x00007357, 0, 0, 0}},
             Instruction{{0x00007957, 0, 0, 0}},
             Instruction{{0x00007fff, 0, 0, 0}},
         }) {
        auto code = TerminalFloatTailProgram;
        code[4] = intervening;
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error_offset == 0x20);
    }
}

TEST_CASE("Ampere terminal minmax proof requires an exact unconditional exit",
          "[video_core][ampere]") {
    for (const Instruction exit : {
             Instruction{{0x0000194d, 0, 0x03800000, 0}},
             Instruction{{0x0000f94d, 0, 0x03800000, 0}},
             Instruction{{0x0000794d, 0, 0x03900000, 0}},
             Instruction{{0x0000794d, 0, 0x03000000, 0}},
             Instruction{{0x0000794d, 1, 0x03800000, 0}},
         }) {
        auto code = TerminalFloatTailProgram;
        code[6] = exit;
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error_offset == 0x20);
    }
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(
        std::span<const Instruction>{TerminalFloatTailProgram}.first(4))));
    auto distant_target = TerminalFloatTailProgram;
    distant_target[2].words[1] = 0x1000;
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(distant_target)));
}

TEST_CASE("Ampere terminal minmax proof rejects extended instruction modes",
          "[video_core][ampere]") {
    for (u32 instruction = 2; instruction <= 6; ++instruction) {
        for (u32 bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 30U, 31U}) {
            auto code = TerminalFloatTailProgram;
            code[instruction].words[3] ^= 1U << bit;
            REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(code)));
        }
    }
    auto predicated = TerminalFloatTailProgram;
    predicated[4].words[0] ^= 1U << 12;
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(predicated)));
    auto unknown_float_mode = TerminalFloatTailProgram;
    unknown_float_mode[4].words[2] ^= 1U << 10;
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(unknown_float_mode)));
    auto invalid_register_form = TerminalFloatTailProgram;
    invalid_register_form[4].words[1] |= 1U << 8;
    REQUIRE_FALSE(static_cast<bool>(Shader::Ampere::LowerToMaxwell(invalid_register_form)));
}
} // namespace
