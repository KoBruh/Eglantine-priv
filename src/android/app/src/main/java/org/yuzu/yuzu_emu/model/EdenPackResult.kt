// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.model

// Mirrors FileSys::EdenPack::PackResult.
enum class EdenPackResult {
    Success,
    Cancelled,
    InvalidSource,
    IoError;

    companion object {
        fun from(int: Int): EdenPackResult = entries.getOrElse(int) { IoError }
    }
}
