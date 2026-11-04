// SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Common::X64 {

#if defined(__x86_64__) || defined(_M_X64)
/// Exits with a readable message when the CPU lacks the instructions this build was compiled for
/// (YUZU_BUILD_PRESET v3, zen2 or zen4 need AVX2, BMI2 and FMA), instead of crashing on the first
/// one. The check runs by itself before every other initializer of the program; the frontends
/// call this at the top of main so that the object holding it gets linked in.
void CheckCpuRequirements();
#else
inline void CheckCpuRequirements() {}
#endif

} // namespace Common::X64
