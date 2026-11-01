// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Shader {

// Caps shader compilation workers by both CPU and RAM: at most half the logical
// processors, and no more than one worker per GiB of RAM left after reserving 8 GiB
// for the running title and the host.
[[nodiscard]] constexpr std::size_t CompilationWorkerCount(std::size_t logical_processors,
                                                            std::uint64_t physical_memory) {
    const std::size_t cpu_workers = std::max<std::size_t>(logical_processors / 2, 1);
    if (physical_memory == 0) {
        return cpu_workers;
    }

    constexpr std::uint64_t ReservedMemory = 8ULL << 30;
    constexpr std::uint64_t MemoryPerWorker = 1ULL << 30;
    const std::uint64_t compiler_memory =
        physical_memory > ReservedMemory ? physical_memory - ReservedMemory : 0;
    const std::uint64_t memory_workers =
        std::max<std::uint64_t>(compiler_memory / MemoryPerWorker, 1);
    return static_cast<std::size_t>(std::min<std::uint64_t>(cpu_workers, memory_workers));
}

} // namespace Shader
