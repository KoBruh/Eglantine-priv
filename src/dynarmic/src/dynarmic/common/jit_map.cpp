// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/common/jit_map.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
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

struct Counted {
    unsigned long long pc, end;
    unsigned terminal;
    unsigned long long count;  // incremented by the block's code (one JIT per host thread)
    const void* host;
    std::size_t host_size;
};
std::mutex counts_mutex;
std::deque<Counted> counted;  // stable addresses
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

bool BlockCountsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DYNARMIC_BLOCK_COUNTS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

unsigned long long* BlockCountsRegister(unsigned long long pc, unsigned long long end, unsigned terminal) {
    std::scoped_lock lock{counts_mutex};
    return &counted.emplace_back(Counted{pc, end, terminal, 0, nullptr, 0}).count;
}

void BlockCountsSetHost(unsigned long long* counter, const void* host, std::size_t size) {
    std::scoped_lock lock{counts_mutex};
    auto* const entry = reinterpret_cast<Counted*>(reinterpret_cast<char*>(counter) - offsetof(Counted, count));
    entry->host = host;
    entry->host_size = size;
}

}  // namespace Common

std::vector<BlockCount> BlockCountsSnapshot() {
    std::scoped_lock lock{counts_mutex};
    std::vector<BlockCount> result;
    result.reserve(counted.size());
    for (const Counted& entry : counted) {
        const unsigned long long count = *static_cast<volatile const unsigned long long*>(&entry.count);
        if (count != 0) {
            result.push_back({entry.pc, entry.end, entry.terminal, count, entry.host, entry.host_size});
        }
    }
    return result;
}

void ResetBlockCounts() {
    std::scoped_lock lock{counts_mutex};
    for (Counted& entry : counted) {
        *static_cast<volatile unsigned long long*>(&entry.count) = 0;
    }
}

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
