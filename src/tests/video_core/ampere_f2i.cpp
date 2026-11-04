// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <utility>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere byte F2I clamps before converting and preserves predicates",
          "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        for (const u32 ftz : {0U, 0x10000U}) {
            for (const u32 predicate : {1U, 7U, 9U}) {
                const std::array<Instruction, 2> code{{
                    {{0x00150305U | (predicate << 12), 3,
                      0x0020e000U | ftz | (is_signed ? 0x100U : 0U), 0x0001e400}},
                    {{0x0000794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(static_cast<bool>(lowered));
                const u64 guard = u64{predicate} << 16;
                const u32 lower = is_signed ? 0xc3000000U : 0U;
                const u32 upper = is_signed ? 0x42fe0000U : 0x437f0000U;
                const std::array<u64, 7> expected{
                    0x5800000000000000ULL | guard | 250 | (3ULL << 8) | (3ULL << 20) |
                        (7ULL << 39) | (7ULL << 48),
                    0x5c40000000000000ULL | guard | 250 | (3ULL << 8) | (250ULL << 20) |
                        (7ULL << 48),
                    0x0100000000000000ULL | guard | 252 | (15ULL << 12) | (u64{lower} << 20),
                    0x5c60000000000000ULL | guard | 250 | (250ULL << 8) | (252ULL << 20) |
                        (15ULL << 39),
                    0x0100000000000000ULL | guard | 252 | (15ULL << 12) | (u64{upper} << 20),
                    0x5c60000000000000ULL | guard | 250 | (250ULL << 8) | (252ULL << 20) |
                        (7ULL << 39),
                    // The clamped value converts with Maxwell's byte destination size (0 at bits
                    // 8-9), which yields the same register as a 32-bit conversion after the clamp.
                    0x5cb0000000000000ULL | guard | 21 | (2ULL << 10) |
                        (u64{is_signed} << 12) | (250ULL << 20) | (3ULL << 39) |
                        (u64{ftz != 0} << 44),
                };
                REQUIRE(lowered.instructions.size() == expected.size() + 1);
                REQUIRE(std::equal(expected.begin(), expected.end(), lowered.instructions.begin()));
                REQUIRE(std::bit_cast<float>(lower) == (is_signed ? -128.0f : 0.0f));
                REQUIRE(std::bit_cast<float>(upper) == (is_signed ? 127.0f : 255.0f));
            }
        }
    }
}

TEST_CASE("Ampere byte F2I materializes constant and uniform sources",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x00157b05, 0x00400400, 0x0021e100, 0x0001e400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto constant = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(constant));
    REQUIRE(constant.instructions.front() ==
            (0x4c98000000070000ULL | 252 | (15ULL << 39) | (4ULL << 20) | (1ULL << 34)));
    for (const u32 source : {0U, 21U, 22U, 27U, 63U}) {
        auto uniform = code;
        uniform[0].words = {0x00157d05, source, 0x0821e100, 0x0001e400};
        const auto lowered = LowerToMaxwell(uniform);
        REQUIRE(static_cast<bool>(lowered));
        const u32 mapped = source < 22 ? source + 216 : source < 28 ? source + 218 : 255;
        REQUIRE(((lowered.instructions.front() >> 8) & 255) == mapped);
        REQUIRE(((lowered.instructions.front() >> 20) & 255) == mapped);
    }
}

TEST_CASE("Ampere byte F2I rejects other widths, rounding, modifiers and register aliases",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x00157305, 3, 0x0021e100, 0x0001e400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 mode : {0x0021c100U, 0x0021a100U, 0x00216100U, 0x0021e300U,
                           0x0061e100U, 0x0821e100U}) {
        auto invalid = code;
        invalid[0].words[2] = mode;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 source : {246U, 254U, 0x10003U}) {
        auto invalid = code;
        invalid[0].words[1] = source;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 source : {28U, 62U, 64U, 216U}) {
        auto invalid = code;
        invalid[0].words = {0x00157d05, source, 0x0821e100, 0x0001e400};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 source : {0x00400440U, 0x04800400U}) {
        auto invalid = code;
        invalid[0].words = {0x00157b05, source, 0x0021e100, 0x0001e400};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto missing_uniform_mode = code;
    missing_uniform_mode[0].words[0] = 0x00157d05;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(missing_uniform_mode)));
    auto invalid = code;
    invalid[0].words[0] |= 1U << 24;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[3] |= 1;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[0] = 0x00f67305;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
}
TEST_CASE("Ampere ceiling F2I preserves rounding, signedness and unsigned saturation",
          "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        for (const u32 ftz : {0U, 0x10000U}) {
            for (const u32 predicate : {2U, 7U, 10U}) {
                const std::array<Instruction, 2> code{{
                    {{0x00020305U | (predicate << 12), 2,
                      0x0020b000U | ftz | (is_signed ? 0x100U : 0U), 0x000e3000}},
                    {{0x0000794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(static_cast<bool>(lowered));
                REQUIRE(lowered.instructions.size() == (is_signed ? 2U : 9U));
                const u64 guard = u64{predicate} << 16;
                const size_t conversion = lowered.instructions.size() - (is_signed ? 2 : 3);
                REQUIRE(lowered.instructions[conversion] ==
                        (0x5cb0000000000000ULL | guard | 2 | (2ULL << 8) | (2ULL << 10) |
                         (u64{is_signed} << 12) | (u64{is_signed ? 2U : 250U} << 20) |
                         (2ULL << 39) | (u64{ftz != 0} << 44)));
                for (size_t index = 0; index + 1 < lowered.instructions.size(); ++index)
                    REQUIRE(((lowered.instructions[index] >> 16) & 15) == predicate);
                if (!is_signed) {
                    REQUIRE(lowered.instructions[2] ==
                            (0x0100000000000000ULL | guard | 252 | (15ULL << 12) |
                             (0x4f800000ULL << 20)));
                    REQUIRE(lowered.instructions[4] ==
                            (0x0100000000000000ULL | guard | 252 | (15ULL << 12) |
                             (0x4f7fffffULL << 20)));
                    REQUIRE(lowered.instructions[7] ==
                            (0x5c40000000000000ULL | guard | 2 | (2ULL << 8) | (251ULL << 20) |
                             (1ULL << 41) | (7ULL << 48)));
                }
            }
        }
    }
}

TEST_CASE("Ampere ceiling F2I rejects non-NTZ, other widths and unknown modifiers",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x00027305, 2, 0x0020b000, 0x000e3000}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 mode : {0x00209000U, 0x0020a000U, 0x0020b800U, 0x0020b200U,
                           0x0060b000U, 0x0820b000U}) {
        auto invalid = code;
        invalid[0].words[2] = mode;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto invalid = code;
    invalid[0].words[1] = 0x00010002;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[3] |= 1;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
}

TEST_CASE("Ampere byte and ceiling F2I bound registers and extended instruction modes",
          "[video_core][ampere]") {
    for (const u32 mode : {0x0021e100U, 0x0020b000U}) {
        const std::array<Instruction, 2> code{{
            {{0x00027305, 2, mode, 0x000e3000}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        for (const u32 source : {0U, 245U, 255U}) {
            auto valid = code;
            valid[0].words[1] = source;
            REQUIRE(static_cast<bool>(LowerToMaxwell(valid)));
        }
        for (const u32 bit : {0U, 8U, 30U, 31U}) {
            auto invalid = code;
            invalid[0].words[3] ^= 1U << bit;
            REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
        }
        for (u32 bit = 9; bit < 30; ++bit) {
            auto scheduling = code;
            scheduling[0].words[3] ^= 1U << bit;
            REQUIRE(static_cast<bool>(LowerToMaxwell(scheduling)));
        }
    }
}
TEST_CASE("Ampere indexed constants preserve narrow widths and signed byte offsets",
          "[video_core][ampere]") {
    for (u32 width = 0; width <= 5; ++width) {
        for (const u32 mode : {0U, 2U}) {
            for (const u32 offset : {0U, 1U, 0x20U, 0x8000U, 0xffffU}) {
                for (const u32 source : {3U, 255U}) {
                    const std::array<Instruction, 2> code{{
                        {{0x00049b82U | (source << 24), (15U << 22) | (offset << 6),
                          (width << 9) | (mode << 14), 0x000e6200}},
                        {{0x0000794d, 0, 0x03800000, 0}},
                    }};
                    const auto lowered = LowerToMaxwell(code);
                    REQUIRE(static_cast<bool>(lowered));
                    REQUIRE(lowered.instructions.size() == 2);
                    REQUIRE(lowered.instructions.front() ==
                            (0xef90000000090000ULL | 4 | (u64{source} << 8) |
                             (u64{offset} << 20) | (15ULL << 36) | (u64{mode} << 44) |
                             (u64{width} << 48)));
                }
            }
        }
    }
}

TEST_CASE("Ampere indexed constants reject unsupported widths modes and operand fields",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0xff059b82, 0x03c00800, 0, 0x000e6200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
    for (const u32 word : {0x0c00U, 0x0e00U, 0x4000U, 0xc000U, 0xa00U}) {
        auto invalid = code;
        invalid[0].words[2] = word;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 bit : {32U, 37U, 59U, 63U, 64U, 72U, 76U, 80U, 91U}) {
        auto invalid = code;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto invalid = code;
    invalid[0].words[1] = (18U << 22) | (0x20U << 6);
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[0] = 0xf6059b82;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
}

TEST_CASE("Ampere uniform votes use the complete mapped uniform register bank",
          "[video_core][ampere]") {
    for (u32 target = 0; target <= 63; ++target) {
        if (target >= 28 && target != 63) continue;
        for (u32 output = 0; output < 8; ++output) {
            for (u32 mode = 0; mode < 3; ++mode) {
                const std::array<Instruction, 2> code{{
                    {{0x00007886U | (target << 16), 0,
                      0x03800000U | (output << 17) | (mode << 8), 0x000fe400}},
                    {{0x0000794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(static_cast<bool>(lowered));
                const u32 mapped = target < 22 ? target + 216 : target < 28 ? target + 218 : 255;
                const u32 temporary = output == 7 ? 7 : 6 - output;
                const u32 mask = 1U << temporary;
                REQUIRE(lowered.instructions.size() == (output == 7 ? 2U : 5U));
                REQUIRE(lowered.instructions[output == 7 ? 0 : 1] ==
                        (0x50d8000000070000ULL | mapped | (7ULL << 39) |
                         (u64{temporary} << 45) | (u64{mode} << 48)));
                if (output != 7) {
                    REQUIRE(lowered.instructions[0] ==
                            (0x38e8000000070000ULL | 253 | (255ULL << 8) | (u64{mask} << 20)));
                    REQUIRE(lowered.instructions[2] ==
                            (0x38e8000000070000ULL | 239 | (239ULL << 8) | (u64{mask} << 20)));
                    REQUIRE(lowered.instructions[3] ==
                            (0x38f0000000070000ULL | (253ULL << 8) | (u64{mask} << 20)));
                }
            }
        }
    }
}

TEST_CASE("Ampere uniform votes reject unmapped registers and unsupported fields",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x00107886, 0, 0x038e0100, 0x000fe400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 target : {28U, 62U, 64U}) {
        auto invalid = code;
        invalid[0].words[0] = 0x00007886U | (target << 16);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 bit : {12U, 24U, 32U, 64U, 73U, 74U, 80U, 84U, 91U}) {
        auto invalid = code;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
}

TEST_CASE("Ampere half register comparisons preserve captured swizzles and predicates",
          "[video_core][ampere]") {
    for (const u32 source : {4U, 8U, 12U, 40U, 52U}) {
        for (u32 output = 0; output < 4; ++output) {
            for (const u32 guard : {1U, 7U, 9U}) {
                const std::array<Instruction, 2> code{{
                    {{0x00000234U | (guard << 12) | (source << 24), 0x30000000U | source,
                      0x00702800U | (output << 17) | (output << 23), 0x000fe400}},
                    {{0x0000794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(static_cast<bool>(lowered));
                REQUIRE(lowered.instructions.size() == 2);
                REQUIRE(lowered.instructions.front() ==
                        (0x5d20000000000000ULL | (u64{guard} << 16) | 7 |
                         (u64{output} << 3) | (u64{source} << 8) | (u64{source} << 20) |
                         (3ULL << 28) | (2ULL << 35) | (u64{output} << 39) | (2ULL << 47)));
            }
        }
    }
}

TEST_CASE("Ampere half register comparisons map every semantic modifier bit",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x28007234, 0x30000028, 0x00702800, 0x000fe400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto baseline = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(baseline));
    const std::array<std::array<u32, 2>, 26> fields{{
        {60, 28}, {61, 29}, {62, 30}, {63, 31}, {69, 45}, {70, 46}, {71, 49},
        {72, 43}, {73, 44}, {74, 47}, {75, 48}, {76, 35}, {77, 36}, {78, 37},
        {79, 38}, {80, 6}, {81, 3}, {82, 4}, {83, 5}, {84, 0}, {85, 1},
        {86, 2}, {87, 39}, {88, 40}, {89, 41}, {90, 42},
    }};
    for (const auto& field : fields) {
        auto modified = code;
        modified[0].words[field[0] / 32] ^= 1U << (field[0] % 32);
        const auto lowered = LowerToMaxwell(modified);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.front() == (baseline.instructions.front() ^ (1ULL << field[1])));
    }
    for (const u32 bit : {16U, 32U + 8U, 59U, 64U, 68U, 91U}) {
        auto invalid = code;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto invalid = code;
    invalid[0].words[2] |= 3U << 5;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[0] = 0xf6007234;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[1] = 0x300000f6;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
}
TEST_CASE("Ampere formatted buffer stores preserve descriptors data and predicates",
          "[video_core][ampere]") {
    for (const u32 guard : {1U, 7U, 9U}) {
        for (const u32 dimension : {1U, 3U, 4U, 5U}) {
            const std::array<Instruction, 2> code{{
                {{0x00000b9bU | (guard << 12), 0x00c00004U | (dimension << 29),
                  0x00100f00, 0x000fe200}},
                {{0x0000794d, 0, 0x03800000, 0}},
            }};
            const auto lowered = LowerToMaxwell(code);
            REQUIRE(static_cast<bool>(lowered));
            REQUIRE(lowered.required_surface_buffer == 3);
            REQUIRE(lowered.required_texture_buffer == ~0U);
            REQUIRE(lowered.instructions.size() == 2);
            REQUIRE(lowered.instructions.front() ==
                    (0xeb20000000000000ULL | (u64{guard} << 16) | 4 | (15ULL << 20) |
                     (u64{dimension} << 33) | (1ULL << 51)));
        }
    }
}

TEST_CASE("Ampere formatted buffer stores retain strict mode and operand bounds",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x00007b9b, 0x20c00004, 0x00100f00, 0x000fe200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 dimension : {2U, 6U, 7U}) {
        auto invalid = code;
        invalid[0].words[1] = 0x00c00004U | (dimension << 29);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    // 1D (dimension 0) shares Maxwell's numbering and lowers like the other dimensions.
    auto one_dimensional = code;
    one_dimensional[0].words[1] = 0x00c00004U;
    const auto one_dimensional_store = LowerToMaxwell(one_dimensional);
    REQUIRE(static_cast<bool>(one_dimensional_store));
    REQUIRE(one_dimensional_store.instructions.front() ==
            (0xeb20000000070000ULL | 4 | (15ULL << 20) | (1ULL << 51)));
    for (const u32 bit : {59U, 60U, 80U, 84U, 91U}) {
        auto invalid = code;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    // A partial channel mask (72-75) carries over to Maxwell's (20-23), whose store keeps the
    // texel's other channels; an empty mask stores nothing and is rejected.
    auto partial = code;
    partial[0].words[2] ^= 1U << 8;
    const auto partial_store = LowerToMaxwell(partial);
    REQUIRE(static_cast<bool>(partial_store));
    REQUIRE(((partial_store.instructions.front() >> 20) & 0xf) == 0xe);
    auto empty_mask = code;
    empty_mask[0].words[2] &= ~0xf00U;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(empty_mask)));
    auto invalid = code;
    invalid[0].words[1] = 0x20c000fd;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[1] = 0x24800004;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[1] |= 0x2000U << 8;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
}

TEST_CASE("Ampere per-tap gathers preserve XY offsets split destinations and predicates",
          "[video_core][ampere]") {
    for (const u32 guard : {1U, 7U, 9U}) {
        for (u32 component = 0; component < 4; ++component) {
            for (const u32 packed : {0xff00ff00U, 0x1ff0002U, 0x1fe01fe0U}) {
                const std::array<Instruction, 4> code{{
                    {{0x00087802, packed, 0x00000f00, 0x000fe400}},
                    {{0x00097802, 0x01000001, 0x00000f00, 0x000fe400}},
                    {{0x040a0b63U | (guard << 12), 0x33800408,
                      0x041e2f08U | (component << 23), 0x100fc200}},
                    {{0x0000794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(static_cast<bool>(lowered));
                REQUIRE(lowered.required_texture_buffer == 14);
                const u64 pred = u64{guard} << 16;
                const auto copy = [pred](u32 target, u32 source) {
                    return 0x5c98000000000000ULL | pred | target | (15ULL << 39) |
                           (u64{source} << 20);
                };
                // The two packed offset words are forwarded from Rb/Rb+1 into the meta registers.
                const std::array<u64, 9> expected{
                    copy(250, 4), copy(251, 5), copy(246, 8), copy(247, 9),
                    0xc800000000000000ULL | pred | 250 | (250ULL << 8) | (246ULL << 20) |
                        (2ULL << 28) | (15ULL << 31) | (4ULL << 36) | (1ULL << 49) |
                        (7ULL << 51) | (2ULL << 54) | (u64{component} << 56),
                    copy(10, 250), copy(11, 251), copy(8, 252), copy(9, 253),
                };
                const auto begin = lowered.instructions.begin() + lowered.source_instruction_offsets[2];
                REQUIRE(std::equal(expected.begin(), expected.end(), begin));
                REQUIRE(begin + expected.size() + 1 == lowered.instructions.end());
            }
        }
    }
}

TEST_CASE("Ampere per-tap gathers forward run-time offsets across arithmetic and gathers",
          "[video_core][ampere]") {
    const std::array<Instruction, 6> code{{
        {{0x00067802, 0x01ff0002, 0x00000f00, 0x000fe400}},
        {{0x00077802, 0x02020200, 0x00000f00, 0x000fe400}},
        {{0x00057421, 0xbf000000, 0x00010000, 0x002fc800}},
        {{0x04037b63, 0x33800405, 0x041e02ff, 0x000f4200}},
        {{0x04007b63, 0x33800406, 0x041e2f06, 0x000f4400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    // The offsets are not proven constant: the gather reads R6/R7 as they are when it runs,
    // so a prior write to them (arithmetic, another gather) or a branch does not reject it.
    const auto forwards_offsets = [](std::span<const Instruction> program) {
        const auto lowered = LowerToMaxwell(program);
        REQUIRE(static_cast<bool>(lowered));
        const u32 gather = lowered.source_instruction_offsets[4];
        REQUIRE(lowered.instructions[gather + 2] ==
                (0x5c98000000070000ULL | 246 | (15ULL << 39) | (6ULL << 20)));
        REQUIRE(lowered.instructions[gather + 3] ==
                (0x5c98000000070000ULL | 247 | (15ULL << 39) | (7ULL << 20)));
    };
    forwards_offsets(code);
    auto clobber = code;
    clobber[2].words[0] = 0x00067421;
    forwards_offsets(clobber);
    clobber = code;
    clobber[3].words[0] = 0x04067b63;
    forwards_offsets(clobber);
    clobber = code;
    clobber[3].words[2] = 0x041e0f06;
    forwards_offsets(clobber);
    auto branch = code;
    branch[2].words = {0x00007947, 0, 0x03800000, 0};
    forwards_offsets(branch);
}

TEST_CASE("Ampere per-tap gathers reject unknown modes and register aliases",
          "[video_core][ampere]") {
    const std::array<Instruction, 4> code{{
        {{0x00087802, 0xff00ff00, 0x00000f00, 0x000fe400}},
        {{0x00097802, 0x01000001, 0x00000f00, 0x000fe400}},
        {{0x040a7b63, 0x33800408, 0x041e2f08, 0x100fc200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    // Offset words are forwarded as registers, not proven constants: Maxwell TLD4.PTP reads
    // each byte's low six bits as a signed offset, so any packed value lowers.
    for (const u32 value : {32U, 127U, 128U, 223U}) {
        for (u32 shift = 0; shift < 32; shift += 8) {
            auto wide = code;
            wide[0].words[1] = (0xff00ff00U & ~(255U << shift)) | (value << shift);
            REQUIRE(static_cast<bool>(LowerToMaxwell(wide)));
        }
    }
    for (const u32 bit : {76U, 78U, 79U, 81U, 89U}) {
        auto invalid = code;
        invalid[2].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto partial = code;
    partial[2].words[2] ^= 1U << (72 - 64); // A three-component mask is a supported gather.
    REQUIRE(static_cast<bool>(LowerToMaxwell(partial)));
    auto predicated = code;
    predicated[0].words[0] = 0x00081802;
    REQUIRE(static_cast<bool>(LowerToMaxwell(predicated)));
    auto other_register = code;
    other_register[1].words[0] = 0x000a7802;
    REQUIRE(static_cast<bool>(LowerToMaxwell(other_register)));
    auto invalid = code;
    for (const u32 target : {0xf50a7b63U, 0x04f57b63U}) {
        invalid = code;
        invalid[2].words[0] = target;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    invalid = code;
    invalid[2].words[1] = 0x338004f5;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[2].words[2] = 0x041e2ff5;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    // Without a constant proof, branches around the gather need no control-flow proof either.
    const Instruction branch{{0x00007947, 0, 0x03800000, 0}};
    const std::array<Instruction, 5> prefix_branch{branch, code[0], code[1], code[2], code[3]};
    REQUIRE(static_cast<bool>(LowerToMaxwell(prefix_branch)));
    const std::array<Instruction, 5> suffix_branch{code[0], code[1], code[2], branch, code[3]};
    REQUIRE(static_cast<bool>(LowerToMaxwell(suffix_branch)));
}

// The gather lowering splits its operands as .SCR (bit 60) packs them and models neither the
// .F16 field (bits 79-80) nor bit 91, so it rejects these encodings instead of lowering them as
// if the bits were clear.
TEST_CASE("Ampere per-tap gathers reject unvalidated control bits", "[video_core][ampere]") {
    const std::array<Instruction, 4> code{{
        {{0x00087802, 0xff00ff00, 0x00000f00, 0x000fe400}},
        {{0x00097802, 0x01000001, 0x00000f00, 0x000fe400}},
        {{0x040a7b63, 0x33800408, 0x041e2f08, 0x100fc200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 bit : {60U, 80U, 91U}) {
        CAPTURE(bit);
        auto invalid = code;
        invalid[2].words[bit / 32] ^= 1U << (bit % 32);
        CHECK_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
}

TEST_CASE("Ampere mixed texture bindings materialize only the secondary descriptor handle",
          "[video_core][ampere]") {
    const std::array<Instruction, 3> code{{
        {{0x06027b60, 0x71001824, 0x049e0709, 0x000fc200}},
        {{0x00007b60, 0x32c0000e, 0x051e070e, 0x000f4200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 4);
    const auto begin = lowered.instructions.begin() + lowered.source_instruction_offsets[1];
    const auto copy = [](u32 target, u32 source) {
        return 0x5c98000000070000ULL | target | (15ULL << 39) | (u64{source} << 20);
    };
    // The bias sample from bank 11 loads its handle into R248 and the bias into R249, the
    // meta registers after the coordinates, which TEX.B reads in place from R0/R1.
    const std::array<u64, 6> expected{
        0x4c98000000070000ULL | 248 | (15ULL << 39) | (11ULL << 34),
        copy(249, 14),
        0xde80000000070000ULL | 250 | (0ULL << 8) | (248ULL << 20) |
            (2ULL << 28) | (7ULL << 31) | (1ULL << 49) | (7ULL << 51) | (2ULL << 37),
        copy(0, 250), copy(1, 251), copy(14, 252),
    };
    REQUIRE(std::equal(expected.begin(), expected.end(), begin));
    REQUIRE(begin + expected.size() + 1 == lowered.instructions.end());
}

TEST_CASE("Ampere array depth comparisons support nonlegacy descriptor bindings",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x08057b60, 0xb1012400, 0x041e41ff, 0x10114400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == 4);
    // Layer, x and y are gathered into R250-R252; the depth reference stays in R1, and the
    // single-component result is written straight to R5.
    const std::array<u32, 3> sources{8, 9, 0};
    for (u32 i = 0; i < sources.size(); ++i) {
        REQUIRE(lowered.instructions[i] == (0x5c98000000070000ULL | (250 + i) |
                                            (15ULL << 39) | (u64{sources[i]} << 20)));
    }
    REQUIRE(lowered.instructions.size() == 5);
    REQUIRE(lowered.instructions[3] ==
            (0xc000000000070000ULL | 5 | (250ULL << 8) | (1ULL << 20) |
             (3ULL << 28) | (1ULL << 31) | (0x124ULL << 36) | (1ULL << 49) |
             (1ULL << 50) | (7ULL << 51)));
}

TEST_CASE("Ampere register bindless textures preserve scalar and vector operand packing",
          "[video_core][ampere]") {
    const std::array<Instruction, 4> samples{{
        {{0x16167361, 0x78000018, 0x041e0718, 0x108f4400}},
        {{0x00007361, 0x38000027, 0x041e0726, 0x002f4400}},
        {{0x0c0d7361, 0xa800001e, 0x041e41ff, 0x001f4400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    // {target, source} copies before TEX.B. The scalar cube takes x, y from Ra/Ra+1 and z and the
    // handle from Rb/Rb+1, coordinates into R252-R254 and the handle into R250. The per-lane 2D
    // form reads `d, u, v, handle` (bits 64, 16, 24, 32), the order Kirby Air Riders renders with.
    const std::array<std::array<std::array<u32, 2>, 4>, 2> copies{{
        {{{252, 22}, {253, 23}, {254, 24}, {250, 25}}},
        {{{250, 0}, {251, 0}, {246, 39}, {255, 255}}},
    }};
    for (u32 i = 0; i < 2; ++i) {
        const std::array<Instruction, 2> code{samples[i], samples[3]};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.required_texture_buffer == ~0U);
        const u32 inputs = i == 0 ? 4 : 3;
        for (u32 j = 0; j < inputs; ++j) {
            REQUIRE(lowered.instructions[j] ==
                    (0x5c98000000070000ULL | copies[i][j][0] | (15ULL << 39) |
                     (u64{copies[i][j][1]} << 20)));
        }
        const u64 texture = lowered.instructions[inputs];
        REQUIRE(((texture >> 8) & 255) == (i == 0 ? 252U : 250U));
        REQUIRE(((texture >> 20) & 255) == (i == 0 ? 250U : 246U));
        REQUIRE(((texture >> 28) & 7) == (i == 0 ? 6U : 2U));
        REQUIRE(((texture >> 50) & 1) == 0);
        REQUIRE((texture >> 56) == 0xde);
    }
}

// The TEX.B forms case 0x361 does not take itself (the per-lane 2D and the cube ones) go to
// 0xb60's general form, as they did before merge 79f5354b1a: this vector-packed (no .SCR, bit 60
// clear) array depth-compare TEX.B has its coordinates in Ra..Ra+2 and the handle and reference
// in Rb/Rb+1.
TEST_CASE("Ampere register bindless textures lower vector-packed operands",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x0c0d7361, 0xa800001e, 0x041e41ff, 0x001f4400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == ~0U);
    const std::array<u32, 5> sources{12, 13, 14, 30, 31};
    for (u32 j = 0; j < sources.size(); ++j) {
        const u32 target = j < 3 ? 250 + j : 246 + j - 3;
        REQUIRE(lowered.instructions[j] ==
                (0x5c98000000070000ULL | target | (15ULL << 39) | (u64{sources[j]} << 20)));
    }
    const u64 texture = lowered.instructions[sources.size()];
    REQUIRE(((texture >> 20) & 255) == 246);
    REQUIRE(((texture >> 28) & 7) == 3U);
    REQUIRE(((texture >> 50) & 1) == 1U);
    REQUIRE((texture >> 56) == 0xde);
}

TEST_CASE("Ampere register bindless textures reject unsupported modifiers and scratch spans",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x16167361, 0x78000018, 0x041e0718, 0x108f4400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 bit : {40U, 58U, 59U, 77U, 81U}) {
        auto invalid = code;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    for (const u32 word : {0xf5167361U, 0x16f57361U}) {
        auto invalid = code;
        invalid[0].words[0] = word;
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto invalid = code;
    invalid[0].words[1] = 0x780000f5;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
}

// The cube form rejects bits 80 and 91 and leaves the instruction to the general TEX.B form,
// which rejects them too (bit 80 is the high bit of the .F16 field) instead of lowering the
// instruction as if they were clear.
TEST_CASE("Ampere register bindless textures reject unvalidated control bits",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x16167361, 0x78000018, 0x041e0718, 0x108f4400}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 bit : {80U, 91U}) {
        CAPTURE(bit);
        auto invalid = code;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        CHECK_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
}

TEST_CASE("Ampere half explicit texel fetches retain LOD and pack RGB into split registers",
          "[video_core][ampere]") {
    for (const u32 guard : {1U, 7U, 9U}) {
        const std::array<Instruction, 2> code{{
            {{0x04040b66U | (guard << 12), 0x33c0440e, 0x059e8705, 0x002f4200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.required_texture_buffer == 15);
        REQUIRE(lowered.instructions.size() == 11);
        const u64 pred = u64{guard} << 16;
        REQUIRE(lowered.instructions[3] ==
                (0xdc00000000000000ULL | pred | 250 | (250ULL << 8) | (246ULL << 20) |
                 (2ULL << 28) | (7ULL << 31) | (0x44ULL << 36) | (1ULL << 49) |
                 (7ULL << 51) | (1ULL << 55)));
        for (u32 pair = 0; pair < 2; ++pair) {
            const u32 low = pair == 0 ? 250 : 252, high = pair == 0 ? 251 : 255;
            const u32 offset = 4 + pair * 3;
            REQUIRE(lowered.instructions[offset] ==
                    (0x5ca8000000000000ULL | pred | 246 | (1ULL << 8) | (2ULL << 10) |
                     (u64{low} << 20)));
            REQUIRE(lowered.instructions[offset + 1] ==
                    (0x5ca8000000000000ULL | pred | 247 | (1ULL << 8) | (2ULL << 10) |
                     (u64{high} << 20)));
            REQUIRE(lowered.instructions[offset + 2] ==
                    (0x5c18000000000000ULL | pred | (4 + pair) | (247ULL << 8) |
                     (246ULL << 20) | (16ULL << 39)));
        }
    }
}

TEST_CASE("Ampere half texture results keep the first component in the low lane",
          "[video_core][ampere][half_texture]") {
    const std::array<Instruction, 2> samples{{
        {{0x04047b66, 0x33c0440e, 0x059e8705, 0x002f4200}},
        {{0x06007b60, 0x33c0440f, 0x059e8702, 0x101fc200}},
    }};
    for (const auto& sample : samples) {
        for (const bool split : {false, true}) {
            for (const bool rgba : {false, true}) {
                const bool texel_fetch = sample.Bits(0, 12) == 0xb66;
                if (texel_fetch && (!split || rgba)) {
                    continue;
                }
                auto instruction = sample;
                if (!texel_fetch) {
                    instruction.words[2] = (instruction.words[2] & ~0xfffU) |
                                           (split ? 2U : 255U) | (rgba ? 0xf00U : 0x700U);
                }
                const std::array<Instruction, 2> code{{
                    instruction,
                    {{0x0000794d, 0, 0x03800000, 0}},
                }};
                const auto lowered = LowerToMaxwell(code);
                REQUIRE(static_cast<bool>(lowered));
                std::array<u32, 256> converted_components{};
                converted_components[250] = 0x3c00;
                converted_components[251] = 0x3800;
                converted_components[252] = 0x3400;
                converted_components[253] = 0x3a00;
                std::array<u32, 256> registers{};
                u32 packed_registers{};
                for (const u64 word : lowered.instructions) {
                    const u32 dest = word & 255;
                    if ((word >> 48) == 0x5ca8) {
                        registers[dest] = converted_components[(word >> 20) & 255];
                    } else if ((word >> 48) == 0x5c18) {
                        registers[dest] = (registers[(word >> 8) & 255] << ((word >> 39) & 31)) +
                                          registers[(word >> 20) & 255];
                        ++packed_registers;
                    } else if ((word >> 56) == 0xd0) {
                        // TEXS.F16 (split RGB/RGBA): Maxwell packs the first two components into
                        // Rd, the first in the low lane, and the rest into Rb (bits 28-35).
                        REQUIRE(((word >> 50) & 7) == (rgba ? 4U : 0U)); // RGBA or RGB swizzle
                        registers[dest] = (converted_components[251] << 16) | converted_components[250];
                        registers[(word >> 28) & 255] =
                            (rgba ? converted_components[253] << 16 : 0U) | converted_components[252];
                        packed_registers += 2;
                    }
                }
                REQUIRE(packed_registers == 2);
                REQUIRE(registers[instruction.Bits(16, 8)] == 0x38003c00);
                const u32 second = split ? instruction.Bits(64, 8) : instruction.Bits(16, 8) + 1;
                REQUIRE(registers[second] == (rgba ? 0x3a003400U : 0x00003400U));
            }
        }
    }
}

TEST_CASE("Ampere half explicit texel fetches keep unsupported formats and register spans rejected",
          "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x04047b66, 0x33c0440e, 0x059e8705, 0x002f4200}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    for (const u32 bit : {59U, 60U, 62U, 76U, 77U, 78U, 80U, 81U, 91U}) {
        auto invalid = code;
        invalid[0].words[bit / 32] ^= 1U << (bit % 32);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    auto invalid = code;
    invalid[0].words[0] = 0xf5047b66;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    invalid = code;
    invalid[0].words[1] = 0x33c044f6;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
}

TEST_CASE("Ampere F2I round up preserves conversion mode", "[video_core][ampere]") {
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    for (const bool is_signed : {false, true}) {
        const Instruction convert{{0x00067305, 0x00000009,
                                   is_signed ? 0x0021b100U : 0x0020b000U, 0}};
        const auto lowered = LowerToMaxwell(std::array{convert, exit});
        REQUIRE(static_cast<bool>(lowered));
        const auto instruction = std::ranges::find_if(lowered.instructions, [](u64 word) {
            return (word & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
        });
        REQUIRE(instruction != lowered.instructions.end());
        REQUIRE((*instruction & 0xff) == 6);
        REQUIRE(((*instruction >> 8) & 3) == 2);
        REQUIRE(((*instruction >> 10) & 3) == 2);
        REQUIRE(((*instruction >> 12) & 1) == static_cast<u64>(is_signed));
        REQUIRE(((*instruction >> 39) & 3) == 2);
        REQUIRE(((*instruction >> 44) & 1) == static_cast<u64>(is_signed));
    }
    const Instruction unsupported{{0x00067305, 0x00000009, 0x0020b001, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit})));
}

TEST_CASE("Ampere F2I supports 16-bit truncation", "[video_core][ampere]") {
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    for (const bool is_signed : {false, true}) {
        const Instruction convert{{0x00067305, 9, 0x0021e800U | (is_signed ? 0x100U : 0), 0}};
        const auto result = LowerToMaxwell(std::array{convert, exit});
        REQUIRE(static_cast<bool>(result));
        const auto word = std::ranges::find_if(result.instructions, [](u64 value) {
            return (value & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
        });
        REQUIRE(word != result.instructions.end());
        REQUIRE(((*word >> 8) & 3) == 1);
        REQUIRE(((*word >> 12) & 1) == static_cast<u64>(is_signed));
        REQUIRE(((*word >> 39) & 3) == 3);
        REQUIRE(((*word >> 44) & 1) == 1);
    }
    const Instruction unsupported{{0x00067305, 9, 0x0021e801, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit})));
}

/// The lowered instruction with these top 12 opcode bits that writes `target`.
const u64* FindWrite(const Shader::Ampere::LoweredProgram& lowered, u64 opcode, u32 target) {
    for (const u64& word : lowered.instructions) {
        if ((word >> 52) == opcode && (word & 0xff) == target) return &word;
    }
    return nullptr;
}

TEST_CASE("Ampere uniform moves write and read every mapped uniform register",
          "[video_core][ampere]") {
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    const auto mapped = [](u32 reg) { return reg < 22 ? reg + 216 : reg + 218; };
    for (const u32 reg : {0U, 15U, 16U, 21U, 22U, 27U}) {
        // UMOV URn, 0x1234
        const Instruction immediate{{0x00007882U | (reg << 16), 0x1234, 0, 0}};
        auto lowered = LowerToMaxwell(std::array{immediate, exit});
        REQUIRE(static_cast<bool>(lowered));
        const u64* move = FindWrite(lowered, 0x010, mapped(reg));
        REQUIRE(move != nullptr);
        REQUIRE(((*move >> 20) & 0xffffffffULL) == 0x1234);
        // UMOV URn, UR3 and UMOV UR3, URn
        const Instruction to{{0x00007c82U | (reg << 16), 3, 0x08000000, 0}};
        lowered = LowerToMaxwell(std::array{to, exit});
        REQUIRE(static_cast<bool>(lowered));
        move = FindWrite(lowered, 0x5c9, mapped(reg));
        REQUIRE(move != nullptr);
        REQUIRE(((*move >> 20) & 0xff) == mapped(3));
        const Instruction from{{0x00037c82U, reg, 0x08000000, 0}};
        lowered = LowerToMaxwell(std::array{from, exit});
        REQUIRE(static_cast<bool>(lowered));
        move = FindWrite(lowered, 0x5c9, mapped(3));
        REQUIRE(move != nullptr);
        REQUIRE(((*move >> 20) & 0xff) == mapped(reg));
        // MOV R5, URn
        const Instruction to_register{{0x00057c02U, reg, 0x08000f00, 0}};
        lowered = LowerToMaxwell(std::array{to_register, exit});
        REQUIRE(static_cast<bool>(lowered));
        move = FindWrite(lowered, 0x5c9, 5);
        REQUIRE(move != nullptr);
        REQUIRE(((*move >> 20) & 0xff) == mapped(reg));
    }
    // URZ as a destination is dropped; UR28 and up are not mapped.
    const auto urz = LowerToMaxwell(std::array{Instruction{{0x003f7882U, 0x1234, 0, 0}}, exit});
    REQUIRE(static_cast<bool>(urz));
    REQUIRE(urz.instructions.size() == 1);
    REQUIRE_FALSE(static_cast<bool>(
        LowerToMaxwell(std::array{Instruction{{0x001c7882U, 0x1234, 0, 0}}, exit})));
    REQUIRE_FALSE(static_cast<bool>(
        LowerToMaxwell(std::array{Instruction{{0x001c7c82U, 3, 0x08000000, 0}}, exit})));
    REQUIRE_FALSE(static_cast<bool>(
        LowerToMaxwell(std::array{Instruction{{0x00057c02U, 28, 0x08000f00, 0}}, exit})));
}

TEST_CASE("Ampere texel fetches of R+B or G+B take alpha into scratch", "[video_core][ampere]") {
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    // TLD.SCR.LZ R8, R10, 2D, with only two of the four channels.
    for (const auto& [mask, swizzle] : {std::pair{5U, 2U}, std::pair{6U, 3U}}) {
        const Instruction fetch{{0x08087b66, 0x1101800a, 0x009e0000U | (mask << 8) | 0xffU,
                                 0x004f4a00}};
        const auto lowered = LowerToMaxwell(std::array{fetch, exit});
        REQUIRE(static_cast<bool>(lowered));
        const auto tlds = std::ranges::find_if(lowered.instructions, [](u64 word) {
            return (word >> 56) == 0xda;
        });
        REQUIRE(tlds != lowered.instructions.end());
        REQUIRE((*tlds & 0xff) == 8);
        REQUIRE(((*tlds >> 28) & 0xff) == 246);
        REQUIRE(((*tlds >> 50) & 7) == swizzle);
    }
}
} // namespace
