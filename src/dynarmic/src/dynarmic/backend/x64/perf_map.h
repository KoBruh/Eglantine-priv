// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <string_view>

#include <bit>

#include "dynarmic/common/jit_map.h"

namespace Dynarmic::Backend::X64 {

#if defined(__linux__) && !defined(__ANDROID__)
namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name);
}  // namespace detail
template<typename T>
void PerfMapRegister(T start, const void* end, std::string_view friendly_name) noexcept {
    detail::PerfMapRegister(std::bit_cast<const void*>(start), end, friendly_name);
}
void PerfMapClear();
#else
// No perf map file here; DYNARMIC_JIT_MAP=1 keeps the same map in memory (Dynarmic::JitMapLookup).
template<typename T>
inline void PerfMapRegister(T start, const void* end, std::string_view friendly_name) noexcept {
    if (::Dynarmic::Common::JitMapEnabled()) {
        ::Dynarmic::Common::JitMapRegister(std::bit_cast<const void*>(start), end, friendly_name);
    }
}
inline void PerfMapClear() noexcept {}
#endif

}  // namespace Dynarmic::Backend::X64
