// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdio>
#include <cstdlib>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

int main() {
    // Real call/return encodings from the upstream ampere_returns regression cases.
    for (const u32 link : {0U, 55U}) {
        const std::array<Instruction, 4> code{{
            {{0x00007802U | (link << 16), 0x20, 0x00000f00, 0}},
            {{0x00007944, 0x10, 0x03c00000, 0}},
            {{0x0000794d, 0, 0x03800000, 0}},
            {{0x00007950U | (link << 24), 0xffffffc0, 0x03c3ffff, 0}},
        }};
        const auto lowered = LowerToMaxwell(code);
        Check(static_cast<bool>(lowered), "valid native call/return was rejected");
        Check(lowered.source_size == sizeof(code), "native callee was truncated");
    }
    const std::array<Instruction, 5> bad_link{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x00007802, 0, 0x00000f00, 0}},
        {{0x00007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    Check(!static_cast<bool>(LowerToMaxwell(bad_link)), "callee overwrote its return address");
    for (const u32 opcode : {0x419U, 0x927U, 0xfffU}) {
        const std::array<Instruction, 2> unsupported{{
            {{0x7000U | opcode, 0, 0, 0}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        Check(!static_cast<bool>(LowerToMaxwell(unsupported)),
              "unsupported or malformed instruction was silently accepted");
    }
    for (const u32 opcode : {0x207U, 0x208U}) {
        for (u32 modifiers = 0; modifiers < 16; ++modifiers) {
            const std::array<Instruction, 2> code{{
                {{0x01007000U | opcode,
                  2U | ((modifiers & 4) ? 1U << 30 : 0) | ((modifiers & 8) ? 1U << 31 : 0),
                  ((modifiers & 1) ? 1U << 9 : 0) | ((modifiers & 2) ? 1U << 8 : 0), 0}},
                {{0x0000794d, 0, 0x03800000, 0}},
            }};
            const auto lowered = LowerToMaxwell(code);
            Check(static_cast<bool>(lowered), "valid FSEL/SEL rejected");
            for (const u32 value : {0x3f800000U, 0xbf800000U, 0x80000000U, 0xffc00001U}) {
                for (const bool select_a : {false, true}) {
                    std::array<u32, 256> registers{};
                    registers[1] = value;
                    registers[2] = value ^ 0x12345678U;
                    for (const u64 instruction : lowered.instructions) {
                        const u32 dst = instruction & 0xff;
                        if ((instruction & 0xffff000000000000ULL) == 0x5c98000000000000ULL) {
                            registers[dst] = registers[(instruction >> 20) & 0xff];
                        } else if ((instruction & 0xff00000000000000ULL) == 0x0400000000000000ULL) {
                            const u32 source = registers[(instruction >> 8) & 0xff];
                            const u32 immediate = static_cast<u32>(instruction >> 20);
                            const u32 op = (instruction >> 53) & 3;
                            Check(op == 0 || op == 2, "unexpected LOP32I");
                            registers[dst] = op == 0 ? source & immediate : source ^ immediate;
                        } else if ((instruction & 0xffff000000000000ULL) == 0x5ca0000000000000ULL) {
                            registers[dst] = registers[(instruction >> (select_a ? 8 : 20)) & 0xff];
                        } else {
                            Check((instruction & 0xffff000000000000ULL) == 0xe300000000000000ULL,
                                  "unrecognized instruction in FSEL reference interpreter");
                        }
                    }
                    u32 expected = select_a ? value : value ^ 0x12345678U;
                    if (opcode == 0x208) {
                        if (modifiers & (select_a ? 1 : 4)) expected &= 0x7fffffffU;
                        if (modifiers & (select_a ? 2 : 8)) expected ^= 0x80000000U;
                    }
                    Check(registers[0] == expected, "FSEL/SEL changed source sign or payload bits");
                }
            }
        }
    }
    std::puts("Ampere lowering: return guards, rejected opcodes and 256 FSEL/SEL comparisons passed.");
}
