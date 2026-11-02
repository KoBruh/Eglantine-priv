// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/common/jit_map.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>

#include "dynarmic/interface/jit_map.h"

namespace Dynarmic {

namespace {
struct Block {
    std::uintptr_t end;
    std::string name;
};

std::mutex mutex;
std::map<std::uintptr_t, Block> blocks;  // by start address
}  // namespace

namespace Common {

bool JitMapEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DYNARMIC_JIT_MAP");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void JitMapRegister(const void* start, const void* end, std::string_view name) {
    const auto begin = reinterpret_cast<std::uintptr_t>(start);
    const auto finish = reinterpret_cast<std::uintptr_t>(end);
    if (begin >= finish) {
        return;
    }
    std::scoped_lock lock{mutex};
    // Code emitted over older blocks (after a cache clear) replaces them.
    auto it = blocks.lower_bound(begin);
    if (it != blocks.begin() && std::prev(it)->second.end > begin) {
        --it;
    }
    while (it != blocks.end() && it->first < finish) {
        it = blocks.erase(it);
    }
    blocks.emplace(begin, Block{finish, std::string{name}});
}

void JitMapClear(const void* code_begin, const void* code_end) {
    std::scoped_lock lock{mutex};
    blocks.erase(blocks.lower_bound(reinterpret_cast<std::uintptr_t>(code_begin)),
                 blocks.lower_bound(reinterpret_cast<std::uintptr_t>(code_end)));
}

}  // namespace Common

std::optional<std::string> JitMapLookup(const void* host_address) {
    const auto address = reinterpret_cast<std::uintptr_t>(host_address);
    std::scoped_lock lock{mutex};
    auto it = blocks.upper_bound(address);
    if (it == blocks.begin()) {
        return std::nullopt;
    }
    --it;
    if (address >= it->second.end) {
        return std::nullopt;
    }
    return it->second.name;
}

}  // namespace Dynarmic
