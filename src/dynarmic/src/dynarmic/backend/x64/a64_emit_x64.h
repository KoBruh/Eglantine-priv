// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <tuple>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <boost/container/static_vector.hpp>

#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::X64 {

struct A64EmitContext final : public EmitContext {
    A64EmitContext(const A64::UserConfig& conf, RegAlloc& reg_alloc, IR::Block& block, boost::container::stable_vector<Xbyak::Label>& shared_labels);

    A64::LocationDescriptor Location() const;
    bool IsSingleStep() const;
    FP::FPCR FPCR(bool fpcr_controlled = true) const override;

    bool HasOptimization(OptimizationFlag flag) const override {
        return conf.HasOptimization(flag);
    }

    const A64::UserConfig& conf;
    /// The instruction emitted just before the current one, and whether the current one is the
    /// block's last and the CheckBit terminal follows it with nothing that changes the host flags.
    const IR::Inst* previous_inst = nullptr;
    bool check_bit_terminal_next = false;
};

class A64EmitX64 final : public EmitX64 {
public:
    A64EmitX64(BlockOfCode& code, A64::UserConfig conf, A64::Jit* jit_interface);
    ~A64EmitX64() override;

    /// Emit host machine code for a basic block with intermediate representation `block`.
    /// @note block is modified.
    BlockDescriptor Emit(IR::Block& block) noexcept;

    void ClearCache() override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

//protected:
    struct FastDispatchEntry {
        u64 location_descriptor = 0xFFFF'FFFF'FFFF'FFFFull;
        const void* code_ptr = nullptr;
    };
    static_assert(sizeof(FastDispatchEntry) == 0x10);
    static constexpr u64 fast_dispatch_table_mask = 0xFFFFF0;
    static constexpr size_t fast_dispatch_table_size = 0x100000;

    void ClearFastDispatchTable();
    void GenMemory128Accessors();
    void GenFastmemFallbacks();
    void GenTerminalHandlers();

    // Microinstruction emitters
    void EmitPushRSB(EmitContext& ctx, IR::Inst* inst);
#define OPCODE(...)
#define A32OPC(...)
#define A64OPC(name, type, ...) void EmitA64##name(A64EmitContext& ctx, IR::Inst* inst);
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

    // Helpers
    std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const override;

    // Fastmem information
    using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;
    /// Where a fastmem access that faults goes: code addresses are offsets in the code space.
    struct FastmemPatchInfo {
        u32 rip;
        u32 resume_rip;
        u32 callback;
        u32 inst_name : 31;
        u32 recompile : 1;
        IR::LocationDescriptor location;
    };
    std::optional<DoNotFastmemMarker> ShouldFastmem(A64EmitContext& ctx, IR::Inst* inst) const;
    void AddFastmemPatch(CodePtr location, CodePtr resume_location, void (*callback)(), const DoNotFastmemMarker& marker, bool recompile);
    FakeCall FastmemCallback(u64 rip);

    // Memory access helpers
    void FindFoldedAddresses(A64EmitContext& ctx);
    void DropDeadNZCVStore(IR::Block& block);
    void EmitCheckMemoryAbort(A64EmitContext& ctx, IR::Inst* inst, Xbyak::Label* end = nullptr);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryRead(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryWrite(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemoryInline(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemoryInline(A64EmitContext& ctx, IR::Inst* inst);

    // Terminal instruction emitters
    bool EmitLeafTerminal(IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;
    bool EmitTerminal(IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;

    // Patching
    void Unpatch(const IR::LocationDescriptor& target_desc) override;
    void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) override;

    /// Where the CheckBit terminal finds the check bit: in the stack layout, or in the host flags
    /// when the block's last instruction set it (NotZF after a test of the bit; ZF and CF are the
    /// flags of the IsZero or TestBit it came from).
    enum class CheckBitLocation : u8 { Stack, NotZF, ZF, CF };

//data
    const A64::UserConfig conf;
    CheckBitLocation check_bit_location = CheckBitLocation::Stack;
    RegAlloc reg_alloc; //reusable reg alloc
    BlockRangeInformation<u64> block_ranges;
    std::array<FastDispatchEntry, fast_dispatch_table_size> fast_dispatch_table;
    std::vector<FastmemPatchInfo> fastmem_patch_info;  // sorted by rip
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> read_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> write_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> exclusive_write_fallbacks;
    ::Common::unordered_set<DoNotFastmemMarker> do_not_fastmem;
    boost::container::stable_vector<Xbyak::Label> shared_labels;
    const void* terminal_handler_pop_rsb_hint = nullptr;
    const void* terminal_handler_fast_dispatch_hint = nullptr;
    /// The same handlers entered with rbx already holding the location hash of the pc stored in
    /// A64JitState: a block that cannot have changed the FPCR builds it from its own.
    const void* terminal_handler_pop_rsb_hint_rbx = nullptr;
    const void* terminal_handler_fast_dispatch_hint_rbx = nullptr;
    /// An indirect branch's own one-entry cache missed: entered with rbx the location hash and rsi
    /// the branch's entry in site_caches, which it fills from the fast dispatch table.
    const void* terminal_handler_fast_dispatch_site_miss = nullptr;
    /// One entry per indirect branch emitted (BR, BLR): the last target it went to, so that each
    /// branch has its own indirect jump for the host's predictor and skips the table's hashing.
    /// Cleared with the fast dispatch table's entries when blocks go.
    std::deque<FastDispatchEntry> site_caches;
    /// While emitting a terminal: the block has nothing that changes the FPCR (MSR FPCR, a
    /// supervisor call or an exception), so the FPCR at its end is the one in its location.
    bool fpcr_is_block_fpcr = false;
    /// DropDeadNZCVStore removed the block's last SetNZCV.
    bool dropped_nzcv_store = false;
    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64) = nullptr;
    A64::Jit* jit_interface = nullptr;
    void (*memory_read_128)() = nullptr;
    void (*memory_write_128)() = nullptr;
    void (*memory_exclusive_write_128)() = nullptr;
};

}  // namespace Dynarmic::Backend::X64
