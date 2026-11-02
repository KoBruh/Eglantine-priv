// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Code emitted without cycle counting keeps values in the host flags between instructions: the
// guest NZCV for selects and conditional branches, the check bit of cbz/tbz. These run whole
// blocks that way (a BRK at the end halts) and check the results against the architecture.

#include <array>
#include <cstdint>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"

using namespace Dynarmic;
using namespace oaknut::util;

namespace {

class HaltingEnv final : public A64TestEnv {
public:
    A64::Jit* jit = nullptr;

    void ExceptionRaised(u64, A64::Exception) override {
        jit->HaltExecution();
    }
};

struct Flags {
    bool n, z, c, v;
};

Flags SubFlags64(u64 a, u64 b) {
    const u64 result = a - b;
    const bool overflow = ((a ^ b) & (a ^ result)) >> 63;
    return {bool(result >> 63), result == 0, a >= b, overflow};
}

Flags SubFlags32(u32 a, u32 b) {
    const u32 result = a - b;
    const bool overflow = ((a ^ b) & (a ^ result)) >> 31;
    return {bool(result >> 31), result == 0, a >= b, overflow};
}

bool Holds(oaknut::Cond cond, Flags f) {
    using C = oaknut::Cond;
    switch (cond) {
    case C::EQ: return f.z;
    case C::NE: return !f.z;
    case C::CS: return f.c;
    case C::CC: return !f.c;
    case C::MI: return f.n;
    case C::PL: return !f.n;
    case C::VS: return f.v;
    case C::VC: return !f.v;
    case C::HI: return f.c && !f.z;
    case C::LS: return !f.c || f.z;
    case C::GE: return f.n == f.v;
    case C::LT: return f.n != f.v;
    case C::GT: return !f.z && f.n == f.v;
    case C::LE: return f.z || f.n != f.v;
    default: return true;
    }
}

constexpr std::array conds{
    oaknut::Cond::EQ, oaknut::Cond::NE, oaknut::Cond::CS, oaknut::Cond::CC, oaknut::Cond::MI,
    oaknut::Cond::PL, oaknut::Cond::VS, oaknut::Cond::VC, oaknut::Cond::HI, oaknut::Cond::LS,
    oaknut::Cond::GE, oaknut::Cond::LT, oaknut::Cond::GT, oaknut::Cond::LE,
};

constexpr std::array<std::pair<u64, u64>, 8> operands{{
    {5, 5},
    {5, 7},
    {7, 5},
    {0, 1},
    {0x8000000000000000, 1},
    {0x7fffffffffffffff, 0xffffffffffffffff},
    {0xffffffff00000001, 0x0000000100000002},
    {0x80000000, 0x7fffffff},
}};

A64::UserConfig NoCycleCountingConfig(HaltingEnv& env) {
    A64::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    return conf;
}

}  // namespace

TEST_CASE("A64: selects and a branch on the flags of a compare", "[a64]") {
    for (const bool is_64 : {true, false}) {
        for (const auto cond : conds) {
            HaltingEnv env;
            A64::Jit jit{NoCycleCountingConfig(env)};
            env.jit = &jit;

            oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
            oaknut::Label taken;
            if (is_64) {
                code.CMP(X0, X1);
                code.CSEL(X2, X3, X4, cond);
                code.CSINC(X5, XZR, XZR, cond);  // zero operands, loaded without touching the flags
                code.CSEL(X6, X4, X3, cond);
            } else {
                code.CMP(W0, W1);
                code.CSEL(W2, W3, W4, cond);
                code.CSINC(W5, WZR, WZR, cond);
                code.CSEL(W6, W4, W3, cond);
            }
            code.B(cond, taken);
            code.MOVZ(X7, 2);
            code.BRK(0);
            code.l(taken);
            code.MOVZ(X7, 1);
            code.BRK(0);

            for (const auto& [a, b] : operands) {
                const Flags flags = is_64 ? SubFlags64(a, b) : SubFlags32(u32(a), u32(b));
                const bool holds = Holds(cond, flags);
                jit.SetPC(0);
                jit.SetRegister(0, a);
                jit.SetRegister(1, b);
                jit.SetRegister(3, 0x1111222233334444);
                jit.SetRegister(4, 0x5555666677778888);
                CheckedRun([&]() { jit.Run(); });

                INFO("cond " << int(cond) << " a " << a << " b " << b << " 64-bit " << is_64);
                const u64 mask = is_64 ? ~u64(0) : 0xffffffff;
                CHECK(jit.GetRegister(2) == ((holds ? 0x1111222233334444 : 0x5555666677778888) & mask));
                CHECK(jit.GetRegister(5) == (holds ? 0 : 1));
                CHECK(jit.GetRegister(6) == ((holds ? 0x5555666677778888 : 0x1111222233334444) & mask));
                CHECK(jit.GetRegister(7) == (holds ? 1 : 2));
                const u32 nzcv = (flags.n << 31) | (flags.z << 30) | (flags.c << 29) | (flags.v << 28);
                CHECK(jit.GetPstate() == nzcv);
            }
        }
    }
}

TEST_CASE("A64: cbz, cbnz, tbz and tbnz branch on the check bit in the host flags", "[a64]") {
    HaltingEnv env;
    A64::Jit jit{NoCycleCountingConfig(env)};
    env.jit = &jit;

    // Each test at 0x00 + 0x40 * n: branch on X0, fall through sets X1 = 2, the target X1 = 1.
    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    const auto test = [&](auto branch) {
        oaknut::Label taken;
        branch(taken);
        code.MOVZ(X1, 2);
        code.BRK(0);
        code.l(taken);
        code.MOVZ(X1, 1);
        code.BRK(0);
        while (env.code_mem.size() % 16 != 0) {
            code.NOP();
        }
    };
    test([&](oaknut::Label& l) { code.CBZ(X0, l); });
    test([&](oaknut::Label& l) { code.CBNZ(X0, l); });
    test([&](oaknut::Label& l) { code.CBZ(W0, l); });
    test([&](oaknut::Label& l) { code.CBNZ(W0, l); });
    test([&](oaknut::Label& l) { code.TBZ(X0, 3, l); });
    test([&](oaknut::Label& l) { code.TBNZ(X0, 3, l); });
    test([&](oaknut::Label& l) { code.TBZ(X0, 40, l); });
    test([&](oaknut::Label& l) { code.TBNZ(X0, 40, l); });

    for (const u64 x0 : {u64(0), u64(8), u64(1) << 40, u64(1) << 32, u64(0xfffffff7)}) {
        const bool zero64 = x0 == 0, zero32 = u32(x0) == 0, bit3 = (x0 >> 3) & 1, bit40 = (x0 >> 40) & 1;
        const std::array<bool, 8> taken{zero64, !zero64, zero32, !zero32, !bit3, bit3, !bit40, bit40};
        for (size_t i = 0; i < taken.size(); ++i) {
            jit.SetPC(0x40 * i);
            jit.SetRegister(0, x0);
            jit.SetRegister(1, 0);
            CheckedRun([&]() { jit.Run(); });
            INFO("test " << i << " x0 " << x0);
            CHECK(jit.GetRegister(1) == (taken[i] ? 1 : 2));
        }
    }
}

TEST_CASE("A64: a W register read back as X is zero-extended", "[a64]") {
    HaltingEnv env;
    A64::Jit jit{NoCycleCountingConfig(env)};
    env.jit = &jit;

    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    code.LDRB(W1, X0);              // the test memory holds each address's low byte
    code.LSR(X2, X1, 2);            // read as X right after the W write
    code.ADD(W3, W4, W5);           // wraps at 32 bits
    code.ADD(X6, X3, X7);           // X3's upper half is zero
    code.LDR(W8, X0, 4);
    code.ADD(X9, X8, X8);
    code.ORR(W10, WZR, W11);        // MOV W10, W11 of a register with an upper half
    code.ADD(X12, X10, X10);
    code.ADD(W13, W11, 1);          // W13 = W11 + 1 while X11 is still needed
    code.ADD(X14, X11, X13);
    code.BRK(0);

    jit.SetPC(0);
    jit.SetRegister(0, 0x1234);
    jit.SetRegister(4, 0xffffffffffffffff);
    jit.SetRegister(5, 2);
    jit.SetRegister(7, 0x100000000);
    jit.SetRegister(11, 0xabcdef0012345678);
    CheckedRun([&]() { jit.Run(); });

    CHECK(jit.GetRegister(1) == 0x34);
    CHECK(jit.GetRegister(2) == 0x34 >> 2);
    CHECK(jit.GetRegister(3) == 1);
    CHECK(jit.GetRegister(6) == 0x100000001);
    CHECK(jit.GetRegister(8) == 0x3b3a3938);
    CHECK(jit.GetRegister(9) == 2 * u64(0x3b3a3938));
    CHECK(jit.GetRegister(10) == 0x12345678);
    CHECK(jit.GetRegister(12) == 2 * u64(0x12345678));
    CHECK(jit.GetRegister(13) == 0x12345679);
    CHECK(jit.GetRegister(14) == 0xabcdef0012345678 + 0x12345679);
}

TEST_CASE("A64: 64-bit add, subtract and compare with an immediate, and shifts", "[a64]") {
    HaltingEnv env;
    A64::Jit jit{NoCycleCountingConfig(env)};
    env.jit = &jit;

    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    code.SUBS(X1, X0, 0x1f);
    code.CSET(X2, oaknut::Cond::HI);
    code.ADDS(X3, X0, 0xfff000);
    code.CSET(X4, oaknut::Cond::CS);
    code.CMN(X0, 1);
    code.CSET(X5, oaknut::Cond::EQ);
    code.ADD(X6, X0, 8);
    code.ADD(X7, X0, X6);            // X0 and X6 both still needed below
    code.ADD(X8, X6, X0);
    code.LSR(X9, X0, 1);             // UBFM: a rotate and a mask, folded to a shift
    code.LSR(W10, W0, 31);
    code.UBFX(X11, X0, 4, 60);
    code.UBFX(X12, X0, 4, 59);       // not a plain shift
    code.BRK(0);

    for (const u64 x0 : {u64(0), u64(0x1f), u64(0x20), u64(0xffffffffffffffff), u64(0xfffffffffff00000), u64(0x8000000000000000)}) {
        jit.SetPC(0);
        jit.SetRegister(0, x0);
        CheckedRun([&]() { jit.Run(); });
        INFO("x0 " << x0);
        CHECK(jit.GetRegister(1) == x0 - 0x1f);
        CHECK(jit.GetRegister(2) == (x0 > 0x1f ? 1 : 0));
        CHECK(jit.GetRegister(3) == x0 + 0xfff000);
        CHECK(jit.GetRegister(4) == (x0 + 0xfff000 < x0 ? 1 : 0));
        CHECK(jit.GetRegister(5) == (x0 + 1 == 0 ? 1 : 0));
        CHECK(jit.GetRegister(6) == x0 + 8);
        CHECK(jit.GetRegister(7) == x0 + x0 + 8);
        CHECK(jit.GetRegister(8) == x0 + x0 + 8);
        CHECK(jit.GetRegister(9) == x0 >> 1);
        CHECK(jit.GetRegister(10) == u32(x0) >> 31);
        CHECK(jit.GetRegister(11) == x0 >> 4);
        CHECK(jit.GetRegister(12) == ((x0 >> 4) & ((u64(1) << 59) - 1)));
    }
}

TEST_CASE("A64: calls, returns and indirect branches through the return stack buffer and fast dispatch", "[a64]") {
    HaltingEnv env;
    A64::Jit jit{NoCycleCountingConfig(env)};
    env.jit = &jit;

    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    oaknut::Label add_one, double_it, done;
    code.MOVZ(X0, 5);
    code.BL(add_one);              // pushes the return onto the RSB
    code.BL(double_it);
    code.MOVZ(X3, 6 * 4);          // the address of done
    code.BR(X3);                   // fast dispatch
    code.MOVZ(X0, 0);              // skipped
    code.l(done);
    code.ADD(X1, X0, 100);
    code.BRK(0);
    code.l(add_one);
    code.ADD(X0, X0, 1);
    code.RET();
    code.l(double_it);
    code.ADD(X0, X0, X0);
    code.RET();

    for (int run = 0; run < 3; ++run) {  // the second run finds every block and RSB entry compiled
        jit.SetPC(0);
        jit.SetRegister(0, 0);
        jit.SetRegister(1, 0);
        CheckedRun([&]() { jit.Run(); });
        CHECK(jit.GetRegister(0) == 12);
        CHECK(jit.GetRegister(1) == 112);
    }
}
