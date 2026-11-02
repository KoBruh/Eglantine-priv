// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace Dynarmic::Common {

// The in-memory map behind Dynarmic::JitMapLookup (DYNARMIC_JIT_MAP=1).
bool JitMapEnabled();
void JitMapRegister(const void* start, const void* end, std::string_view name);
// Forgets the blocks emitted in [code_begin, code_end): one JIT's code buffer, on a cache clear.
void JitMapClear(const void* code_begin, const void* code_end);

}  // namespace Dynarmic::Common
