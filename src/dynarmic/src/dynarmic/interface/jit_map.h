// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Dynarmic {

/// DYNARMIC_JIT_MAP=1: the JIT keeps a map of the host code it emits to the guest block it came
/// from (the x64 backend's perf map, in memory), so a sampling profiler can name guest code.
/// Returns the block's name (its guest location) for an address inside emitted code, if mapped.
std::optional<std::string> JitMapLookup(const void* host_address);

/// DYNARMIC_BLOCK_COUNTS=1: every compiled A64 block counts its executions (each host thread's
/// JIT has its own blocks, so a guest block may appear once per core and per recompilation).
struct BlockCount {
    std::uint64_t pc;        ///< guest address of the block's first instruction
    std::uint64_t end;       ///< guest address after its last instruction
    unsigned terminal;       ///< how the block ends (IR::Term::Terminal's index)
    std::uint64_t count;     ///< executions since the last reset
};
std::vector<BlockCount> BlockCountsSnapshot();
void ResetBlockCounts();

}  // namespace Dynarmic
