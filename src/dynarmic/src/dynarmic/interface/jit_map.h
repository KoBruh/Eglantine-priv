// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>

namespace Dynarmic {

/// DYNARMIC_JIT_MAP=1: the JIT keeps a map of the host code it emits to the guest block it came
/// from (the x64 backend's perf map, in memory), so a sampling profiler can name guest code.
/// Returns the block's name (its guest location) for an address inside emitted code, if mapped.
std::optional<std::string> JitMapLookup(const void* host_address);

}  // namespace Dynarmic
