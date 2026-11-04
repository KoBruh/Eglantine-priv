// SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <random>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x794d, 0, 0x03800000, 0}};

// The Maxwell instructions the integer add, IMAD-move and ISETP.EX lowerings emit, run on one
// thread: R0-R254 (RZ reads zero), P0-P6 (PT is true) and the condition-code carry.
struct Machine {
    std::array<u32, 256> registers{};
    std::array<bool, 7> predicates{};
    bool carry{};

    u32 Read(u32 index) const {
        return index == 255 ? 0 : registers[index];
    }
    bool Predicate(u32 index) const {
        return index == 7 || predicates[index];
    }
    void SetPredicate(u32 index, bool value) {
        if (index != 7)
            predicates[index] = value;
    }

    void Run(std::span<const u64> code) {
        for (const u64 word : code) {
            const auto bits = [&](u32 offset, u32 count) {
                return static_cast<u32>((word >> offset) & ((u64{1} << count) - 1));
            };
            if ((word >> 52) == 0xe30)
                break; // EXIT
            if (Predicate(bits(16, 3)) == (bits(19, 1) != 0))
                continue;
            const u32 dest = bits(0, 8);
            const u32 a = Read(bits(8, 8)), b = Read(bits(20, 8));
            const u64 opcode = word & 0xfff8000000000000ULL;
            if (opcode == 0x5c10000000000000ULL) { // IADD
                REQUIRE(bits(48, 2) != 3); // .PO is not emitted
                const u64 sum = u64{bits(49, 1) ? 0U - a : a} + (bits(48, 1) ? 0U - b : b) +
                                (bits(43, 1) && carry);
                if (bits(47, 1))
                    carry = (sum >> 32) != 0;
                Write(dest, static_cast<u32>(sum));
            } else if ((word >> 57) == 0x0e) { // IADD32I
                REQUIRE(bits(52, 5) == (bits(56, 1) << 4));
                Write(dest, (bits(56, 1) ? 0U - a : a) + bits(20, 32));
            } else if ((word >> 58) == 0x01) { // LOP32I
                REQUIRE(bits(55, 2) == 0);
                const u32 imm = bits(20, 32), op = bits(53, 2);
                Write(dest, op == 0 ? a & imm : op == 1 ? (a | imm) : op == 2 ? (a ^ imm) : imm);
            } else if ((word >> 52) == 0x010) { // MOV32I
                Write(dest, bits(20, 32));
            } else if (opcode == 0x5c98000000000000ULL) { // MOV
                REQUIRE(bits(39, 4) == 15);
                Write(dest, b);
            } else if (opcode == 0x5ca0000000000000ULL) { // SEL
                Write(dest, Predicate(bits(39, 3)) != (bits(42, 1) != 0) ? a : b);
            } else if ((word >> 52) == 0x5b6) { // ISETP
                REQUIRE(bits(43, 1) == 0);
                const bool is_signed = bits(48, 1) != 0;
                const s64 x = is_signed ? s64{std::bit_cast<s32>(a)} : s64{a};
                const s64 y = is_signed ? s64{std::bit_cast<s32>(b)} : s64{b};
                const u32 compare = bits(49, 3);
                const bool result = compare == 1 ? x < y : compare == 2 ? x == y
                                  : compare == 3 ? x <= y : compare == 4 ? x > y
                                  : compare == 5 ? x != y : compare == 6 ? x >= y : compare == 7;
                const bool input = Predicate(bits(39, 3)) != (bits(42, 1) != 0);
                const u32 combine = bits(45, 2);
                REQUIRE(combine < 3);
                const auto apply = [&](bool value) {
                    return combine == 0 ? value && input : combine == 1 ? value || input : value != input;
                };
                const bool first = apply(result), second = apply(!result);
                SetPredicate(bits(3, 3), first);
                SetPredicate(bits(0, 3), second);
            } else if ((word & 0xfef8000000000000ULL) == 0x38e8000000000000ULL) { // P2R
                const u32 mask = bits(20, 8);
                u32 value = 0;
                for (u32 i = 0; i < 7; ++i)
                    value |= u32{predicates[i]} << i;
                Write(dest, (a & ~mask) | (value & mask));
            } else if ((word & 0xfef8000000000000ULL) == 0x38f0000000000000ULL) { // R2P
                const u32 mask = bits(20, 8);
                for (u32 i = 0; i < 7; ++i)
                    if (mask & (1U << i))
                        predicates[i] = ((a >> i) & 1) != 0;
            } else {
                FAIL("Unexpected emitted instruction " << std::hex << word);
            }
        }
    }

private:
    void Write(u32 index, u32 value) {
        if (index != 255)
            registers[index] = value;
    }
};

Machine Run(const Machine& start, std::span<const Instruction> code) {
    const auto lowered = LowerToMaxwell(code);
    INFO("lowering error: " << lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    Machine machine = start;
    machine.Run(lowered.instructions);
    return machine;
}

// IADD3 Rd, [Pu, Pv,] [-]Ra, [-]b, [-]Rc[, Pp, Pq] in the register (0x210) or immediate (0x810)
// form. Carry predicates are 0-6 or 7 (PT); carry inputs are 4-bit (bit 3 negates, 15 = !PT).
struct Iadd3 {
    u32 dest, a, b, c = 255;
    bool negate_a = false, negate_b = false, negate_c = false, extended = false, immediate = false;
    u32 first_output = 7, second_output = 7, first_input = 15, second_input = 15;

    Instruction Encode() const {
        return {{(immediate ? 0x7810U : 0x7210U) | (dest << 16) | (a << 24),
                 immediate ? b : b | (u32{negate_b} << 31),
                 c | (u32{negate_a} << 8) | (u32{extended} << 10) | (u32{negate_c} << 11) |
                     (second_input << 13) | (first_output << 17) | (second_output << 20) |
                     (first_input << 23),
                 0}};
    }
};

// What the native instruction computes (Mesa NAK's IADD3/IADD3.X folding): a modified source
// counts as ~x, plus 1 for IADD3; the outputs are set when the 34-bit sum reaches 2^32 / 2^33.
struct Sum {
    u32 value;
    bool first, second;
};
Sum Reference(const Iadd3& insn, const Machine& machine) {
    const auto source = [&](u32 value, bool modified) {
        return modified ? u64{static_cast<u32>(~value)} + (insn.extended ? 0 : 1) : u64{value};
    };
    u64 sum = source(machine.Read(insn.a), insn.negate_a) +
              source(insn.immediate ? insn.b : machine.Read(insn.b), insn.negate_b) +
              source(machine.Read(insn.c), insn.negate_c);
    if (insn.extended) {
        for (const u32 input : {insn.first_input, insn.second_input}) {
            if (input != 15)
                sum += machine.Predicate(input & 7) != ((input & 8) != 0);
        }
    }
    return {static_cast<u32>(sum), sum >= (u64{1} << 32), sum >= (u64{2} << 32)};
}

Machine Seed(std::mt19937& random) {
    Machine machine;
    for (u32 i = 0; i < 246; ++i)
        machine.registers[i] = random();
    for (bool& predicate : machine.predicates)
        predicate = (random() & 1) != 0;
    return machine;
}

// Operand values that reach every carry case: zero (whose negation carries), equal and
// adjacent values, the extremes and random words.
std::vector<std::array<u32, 3>> Operands(std::mt19937& random) {
    std::vector<std::array<u32, 3>> values;
    constexpr std::array<u32, 6> special{0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff};
    for (const u32 x : special)
        for (const u32 y : special)
            for (const u32 z : {0U, 1U, 0xffffffffU})
                values.push_back({x, y, z});
    for (u32 i = 0; i < 64; ++i)
        values.push_back({static_cast<u32>(random()), static_cast<u32>(random()),
                          static_cast<u32>(random())});
    return values;
}

void CheckIadd3(const Iadd3& insn, std::mt19937& random) {
    const std::array code{insn.Encode(), Exit};
    for (const auto& [x, y, z] : Operands(random)) {
        Machine start = Seed(random);
        start.registers[insn.a] = x;
        if (!insn.immediate)
            start.registers[insn.b] = y;
        if (insn.c != 255)
            start.registers[insn.c] = z;
        const Sum expected = Reference(insn, start);
        const Machine end = Run(start, code);
        CAPTURE(x, y, z);
        REQUIRE(end.registers[insn.dest] == expected.value);
        if (insn.first_output != 7)
            REQUIRE(end.predicates[insn.first_output] == expected.first);
        if (insn.second_output != 7)
            REQUIRE(end.predicates[insn.second_output] == expected.second);
    }
}
} // namespace

TEST_CASE("Ampere IADD3 keeps the negation of b in its carry forms",
          "[video_core][ampere][integer_add]") {
    std::mt19937 random{0x1add3};
    // The low word of a 64-bit subtraction: IADD3 R4, P0, R0, -R2, RZ.
    CheckIadd3({.dest = 4, .a = 0, .b = 2, .negate_b = true, .first_output = 0}, random);
    // Three sources with one or two carry outputs, b or c negated.
    CheckIadd3({.dest = 4, .a = 0, .b = 2, .c = 3, .first_output = 0}, random);
    CheckIadd3({.dest = 4, .a = 0, .b = 2, .c = 3, .negate_b = true, .first_output = 1,
                .second_output = 5},
               random);
    CheckIadd3({.dest = 4, .a = 0, .b = 2, .c = 3, .negate_c = true, .first_output = 0,
                .second_output = 1},
               random);
    // IADD3.X R5, R1, ~R3, RZ, P0: the high word, where the modifier is a bitwise NOT.
    CheckIadd3({.dest = 5, .a = 1, .b = 3, .negate_b = true, .extended = true, .first_input = 0},
               random);

    // Both halves together subtract 64-bit values.
    const std::array code{
        Iadd3{.dest = 4, .a = 0, .b = 2, .negate_b = true, .first_output = 0}.Encode(),
        Iadd3{.dest = 5, .a = 1, .b = 3, .negate_b = true, .extended = true, .first_input = 0}
            .Encode(),
        Exit,
    };
    for (const auto& [x, y, z] : Operands(random)) {
        Machine start = Seed(random);
        start.registers[0] = x;
        start.registers[1] = z;
        start.registers[2] = y;
        start.registers[3] = static_cast<u32>(random());
        const Machine end = Run(start, code);
        const u64 left = (u64{z} << 32) | x, right = (u64{start.registers[3]} << 32) | y;
        REQUIRE(((u64{end.registers[5]} << 32) | end.registers[4]) == left - right);
    }
}

TEST_CASE("Ampere IADD3.X inverts a modified source instead of negating it",
          "[video_core][ampere][integer_add]") {
    std::mt19937 random{0x1add3e};
    // IADD3.X R4, R0, R2, ~R3, P0 and IADD3.X R4, R0, ~R2, R3, !P1.
    CheckIadd3({.dest = 4, .a = 0, .b = 2, .c = 3, .negate_c = true, .extended = true,
                .first_input = 0},
               random);
    CheckIadd3({.dest = 4, .a = 0, .b = 2, .c = 3, .negate_b = true, .extended = true,
                .first_input = 9},
               random);
}

TEST_CASE("Ampere IADD3 with an immediate and a third source keeps the immediate's sign",
          "[video_core][ampere][integer_add]") {
    std::mt19937 random{0x1add31};
    // IADD3 R43, R35, -0x7e, -R36 from a Pokopia compute program, and the other signs.
    for (const u32 immediate : {0xffffff82U, 0x7eU, 0x80000000U, 0U}) {
        CAPTURE(immediate);
        CheckIadd3({.dest = 43, .a = 35, .b = immediate, .c = 36, .negate_c = true,
                    .immediate = true},
                   random);
        CheckIadd3({.dest = 43, .a = 35, .b = immediate, .c = 36, .immediate = true}, random);
        CheckIadd3({.dest = 43, .a = 35, .b = immediate, .c = 36, .negate_a = true,
                    .immediate = true},
                   random);
    }
}
