// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Scalar float and double operations: their operands come from lane 0 of a vector register (the
// other lanes hold garbage here), their results clear the rest of the register, and they reach
// general registers, memory and the flags. The x64 backend keeps such scalars in XMM registers.

#include <bit>
#include <cstdint>

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

u64 F(float value) {
    return std::bit_cast<u32>(value);
}

u64 D(double value) {
    return std::bit_cast<u64>(value);
}

}  // namespace

TEST_CASE("A64: scalar float and double operations on lane 0", "[a64]") {
    for (const bool cycle_counting : {false, true}) {
        HaltingEnv env;
        A64::UserConfig conf{};
        conf.callbacks = &env;
        conf.enable_cycle_counting = cycle_counting;
        A64::Jit jit{conf};
        env.jit = &jit;

        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        oaknut::Label less;
        code.FMUL(S0, S1, S2);       // -3.375
        code.FMADD(S3, S0, S1, S2);  // -3.375 * 1.5 - 2.25 = -7.3125
        code.FADD(S4, S3, S0);       // -10.6875
        code.FSUB(S5, S4, S1);       // -12.1875
        code.FMOV(W6, S5);
        code.STR(S4, X10);
        code.LDR(S7, X10);
        code.FMUL(S8, S7, S1);  // -16.03125
        code.FCMP(S8, S4);      // less
        code.FCSEL(S11, S1, S2, oaknut::Cond::LT);
        code.FMUL(D12, D9, D19);  // 1.5
        code.FADD(D13, D12, D9);  // 4.5
        code.FMOV(X14, D13);
        code.STR(D13, X10, 8);
        code.FCVT(D15, S4);
        code.FMOV(S16, S1);
        code.B(oaknut::Cond::LT, less);
        code.MOVZ(X17, 2);
        code.BRK(0);
        code.l(less);
        code.MOVZ(X17, 1);
        code.BRK(0);

        for (int run = 0; run < 2; ++run) {
            for (size_t i = 0; i < 32; ++i) {
                jit.SetVector(i, {0x5555'5555'5555'5555 + i, 0xAAAA'AAAA'AAAA'AAAA - i});
            }
            jit.SetVector(1, {0xDEAD'BEEF'0000'0000 | F(1.5f), 0x0123'4567'89AB'CDEF});
            jit.SetVector(2, {0xCAFE'F00D'0000'0000 | F(-2.25f), 0xFEDC'BA98'7654'3210});
            jit.SetVector(9, {D(3.0), 0x1111'2222'3333'4444});
            jit.SetVector(19, {D(0.5), 0x5555'6666'7777'8888});
            jit.SetRegister(10, 0x1000);
            jit.SetPC(0);
            env.ticks_left = 100;
            CheckedRun([&]() { jit.Run(); });

            INFO("cycle counting " << cycle_counting << " run " << run);
            CHECK(jit.GetVector(0) == Vector{F(-3.375f), 0});
            CHECK(jit.GetVector(3) == Vector{F(-7.3125f), 0});
            CHECK(jit.GetVector(4) == Vector{F(-10.6875f), 0});
            CHECK(jit.GetVector(5) == Vector{F(-12.1875f), 0});
            CHECK(jit.GetRegister(6) == F(-12.1875f));
            CHECK(env.MemoryRead(0x1000, 4) == F(-10.6875f));
            CHECK(jit.GetVector(7) == Vector{F(-10.6875f), 0});
            CHECK(jit.GetVector(8) == Vector{F(-16.03125f), 0});
            CHECK(jit.GetVector(11) == Vector{F(1.5f), 0});
            CHECK(jit.GetVector(12) == Vector{D(1.5), 0});
            CHECK(jit.GetVector(13) == Vector{D(4.5), 0});
            CHECK(jit.GetRegister(14) == D(4.5));
            CHECK(env.MemoryRead(0x1008, 8) == D(4.5));
            CHECK(jit.GetVector(15) == Vector{D(-10.6875), 0});
            CHECK(jit.GetVector(16) == Vector{F(1.5f), 0});
            CHECK(jit.GetRegister(17) == 1);
            CHECK(jit.GetPstate() == 0x80000000);  // N: less
            // Registers the code does not write keep their lanes.
            CHECK(jit.GetVector(1) == Vector{0xDEAD'BEEF'0000'0000 | F(1.5f), 0x0123'4567'89AB'CDEF});
            CHECK(jit.GetVector(20) == Vector{0x5555'5555'5555'5555 + 20, 0xAAAA'AAAA'AAAA'AAAA - 20});
        }
    }
}
