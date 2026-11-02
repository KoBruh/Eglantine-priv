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

}  // namespace Dynarmic::Common
