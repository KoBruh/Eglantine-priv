// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

namespace Dynarmic::Common {

// The in-memory map behind Dynarmic::JitMapLookup (DYNARMIC_JIT_MAP=1).
bool JitMapEnabled();
void JitMapRegister(const void* start, const void* end, std::string_view name);
// Forgets the blocks emitted in [code_begin, code_end): one JIT's code buffer, on a cache clear.
void JitMapClear(const void* code_begin, const void* code_end);

// DYNARMIC_BLOCK_COUNTS=1: a counter for a new block, incremented by its emitted code on entry.
bool BlockCountsEnabled();
unsigned long long* BlockCountsRegister(unsigned long long pc, unsigned long long end, unsigned terminal);
// Records where the counted block's code was emitted, once it is.
void BlockCountsSetHost(unsigned long long* counter, const void* host, std::size_t size);

// DYNARMIC_AB=<ms>: an A/B comparison inside one run. The JITs alternate between phase 0 and
// phase 1 every <ms> milliseconds of wall time (checked when a run starts); blocks are compiled
// apart for each phase (A64::LocationDescriptor's experiment bit), so both stay in the cache.
// Phase 1 turns off the features in DYNARMIC_AB_FEATURES (a mask, all by default); phase 0 does
// instead with DYNARMIC_AB_INVERT=1, which cancels any difference between the phases themselves. The cycles
// spent running JIT code and, with DYNARMIC_BLOCK_COUNTS=1, the block executions are recorded
// per phase.
enum AbFeature : unsigned {
    AbReturnHash = 2,
    AbLogicalHostFlags = 4,
    AbDeadNZCV = 8,
    AbIndirectSiteCache = 16,
    AbInlineReturn = 32,
    AbScalarsInXmm = 64,
    AbFpCompareFlags = 128,
};
bool AbEnabled();
unsigned AbCurrentPhase();
// Whether code compiled for this phase (the experiment bit of its location) leaves the feature off.
bool AbOff(AbFeature feature, bool experiment);
void AbAddCycles(unsigned phase, unsigned long long cycles);

}  // namespace Dynarmic::Common
