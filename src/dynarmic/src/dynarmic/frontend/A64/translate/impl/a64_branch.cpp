// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

// Halt checks (HaltExecution) go on backward edges only: every loop jumps backward somewhere, so
// each iteration still checks, while the forward edges of ifs and loop exits jump straight to
// the next block. Half a billion blocks a second end in a branch in some games. An indirect branch
// (BR, BLR), which may go either way, is checked where the backend dispatches it.
static IR::Term::LeafTerminal LinkTo(TranslatorVisitor& v, u64 target) {
    const IR::LocationDescriptor location = v.ir.current_location->SetPC(target);
    if (target <= v.ir.PC()) {
        return IR::Term::LinkBlock{location};
    }
    return IR::Term::LinkBlockFast{location};
}

static IR::Term::LeafTerminal LinkToNext(TranslatorVisitor& v) {
    return IR::Term::LinkBlockFast{v.ir.current_location->AdvancePC(4)};
}

bool TranslatorVisitor::B_cond(Imm<19> imm19, Cond cond) {
    const s64 offset = concatenate(imm19, Imm<2>{0}).SignExtend<s64>();
    const u64 target = ir.PC() + offset;

    const auto cond_pass = LinkTo(*this, target);
    const auto cond_fail = LinkToNext(*this);
    ir.SetTerm(IR::Term::If{cond, cond_pass, cond_fail});
    return false;
}

bool TranslatorVisitor::B_uncond(Imm<26> imm26) {
    const s64 offset = concatenate(imm26, Imm<2>{0}).SignExtend<s64>();
    const u64 target = ir.PC() + offset;
    // Backward, including the pattern to halt execution (B .), checks for a halt.
    ir.SetTerm(LinkTo(*this, target));
    return false;
}

bool TranslatorVisitor::BL(Imm<26> imm26) {
    const s64 offset = concatenate(imm26, Imm<2>{0}).SignExtend<s64>();

    X(64, Reg::R30, ir.Imm64(ir.PC() + 4));
    ir.PushRSB(ir.current_location->AdvancePC(4));

    const u64 target = ir.PC() + offset;
    ir.SetTerm(IR::Term::LinkBlockFast{ir.current_location->SetPC(target)});
    return false;
}

bool TranslatorVisitor::BLR(Reg Rn) {
    const auto target = X(64, Rn);

    X(64, Reg::R30, ir.Imm64(ir.PC() + 4));
    ir.PushRSB(ir.current_location->AdvancePC(4));

    ir.SetPC(target);
    ir.SetTerm(IR::Term::FastDispatchHint{});
    return false;
}

bool TranslatorVisitor::BR(Reg Rn) {
    const auto target = X(64, Rn);

    ir.SetPC(target);
    ir.SetTerm(IR::Term::FastDispatchHint{});
    return false;
}

bool TranslatorVisitor::RET(Reg Rn) {
    const auto target = X(64, Rn);

    ir.SetPC(target);
    ir.SetTerm(IR::Term::PopRSBHint{});
    return false;
}

bool TranslatorVisitor::CBZ(bool sf, Imm<19> imm19, Reg Rt) {
    const size_t datasize = sf ? 64 : 32;
    const s64 offset = concatenate(imm19, Imm<2>{0}).SignExtend<s64>();

    const IR::U32U64 operand1 = X(datasize, Rt);

    ir.SetCheckBit(ir.IsZero(operand1));

    const u64 target = ir.PC() + offset;
    const auto cond_pass = LinkTo(*this, target);
    const auto cond_fail = LinkToNext(*this);
    ir.SetTerm(IR::Term::CheckBit{cond_pass, cond_fail});
    return false;
}

bool TranslatorVisitor::CBNZ(bool sf, Imm<19> imm19, Reg Rt) {
    const size_t datasize = sf ? 64 : 32;
    const s64 offset = concatenate(imm19, Imm<2>{0}).SignExtend<s64>();

    const IR::U32U64 operand1 = X(datasize, Rt);

    ir.SetCheckBit(ir.IsZero(operand1));

    const u64 target = ir.PC() + offset;
    const auto cond_pass = LinkToNext(*this);
    const auto cond_fail = LinkTo(*this, target);
    ir.SetTerm(IR::Term::CheckBit{cond_pass, cond_fail});
    return false;
}

bool TranslatorVisitor::TBZ(Imm<1> b5, Imm<5> b40, Imm<14> imm14, Reg Rt) {
    const size_t datasize = b5 == 1 ? 64 : 32;
    const u8 bit_pos = concatenate(b5, b40).ZeroExtend<u8>();
    const s64 offset = concatenate(imm14, Imm<2>{0}).SignExtend<s64>();

    const auto operand = X(datasize, Rt);

    ir.SetCheckBit(ir.TestBit(operand, ir.Imm8(bit_pos)));

    const u64 target = ir.PC() + offset;
    const auto cond_1 = LinkToNext(*this);
    const auto cond_0 = LinkTo(*this, target);
    ir.SetTerm(IR::Term::CheckBit{cond_1, cond_0});
    return false;
}

bool TranslatorVisitor::TBNZ(Imm<1> b5, Imm<5> b40, Imm<14> imm14, Reg Rt) {
    const size_t datasize = b5 == 1 ? 64 : 32;
    const u8 bit_pos = concatenate(b5, b40).ZeroExtend<u8>();
    const s64 offset = concatenate(imm14, Imm<2>{0}).SignExtend<s64>();

    const auto operand = X(datasize, Rt);

    ir.SetCheckBit(ir.TestBit(operand, ir.Imm8(bit_pos)));

    const u64 target = ir.PC() + offset;
    const auto cond_1 = LinkTo(*this, target);
    const auto cond_0 = LinkToNext(*this);
    ir.SetTerm(IR::Term::CheckBit{cond_1, cond_0});
    return false;
}

}  // namespace Dynarmic::A64
