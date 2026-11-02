// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>

#include "common/memory_detect.h"
#include "video_core/renderer_vulkan/command_reuse_policy.h"

#include "video_core/nx2.h"

#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

constexpr size_t COMMAND_BUFFER_POOL_SIZE = 4;

struct CommandPool::Pool {
    vk::CommandPool handle;
    vk::CommandBuffers cmdbufs;
    std::array<u32, COMMAND_BUFFER_POOL_SIZE> uses{};
};

CommandPool::CommandPool(MasterSemaphore& master_semaphore_, const Device& device_)
    : ResourcePool(master_semaphore_, COMMAND_BUFFER_POOL_SIZE), device{device_} {}

CommandPool::~CommandPool() = default;

void CommandPool::Allocate(size_t begin, size_t end) {
    // Command buffers are going to be committed, recorded, executed every single usage cycle.
    // They are also going to be reset when committed.
    Pool& pool = pools.emplace_back();
    pool.handle = device.GetLogical().CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    pool.cmdbufs = pool.handle.Allocate(COMMAND_BUFFER_POOL_SIZE);
}

VkCommandBuffer CommandPool::Commit() {
    const size_t index = CommitResource();
    const auto pool_index = index / COMMAND_BUFFER_POOL_SIZE;
    const auto sub_index = index % COMMAND_BUFFER_POOL_SIZE;
    Pool& pool = pools[pool_index];
    const VkCommandBuffer cmdbuf = pool.cmdbufs[sub_index];
#ifdef _WIN32
    // Reuse driver recording memory between periodic releases. Releasing on every Begin made
    // the driver allocate it again each time; never releasing retained large recordings.
    static const unsigned release_interval = [] {
        const char* value = std::getenv("EDEN_NX2_COMMAND_POOL_RELEASE");
        if (value != nullptr && value[0] == '0') {
            return 0u;
        }
        const char* interval = std::getenv("EDEN_NX2_COMMAND_POOL_RELEASE_INTERVAL");
        return interval ? static_cast<unsigned>(std::clamp(std::strtoul(interval, nullptr, 10),
                                                           1UL, 64UL)) : 8u;
    }();
    if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        if (pressure_check++ % 32 == 0) {
            const u64 free_commit = Common::GetAvailableCommitMemory();
            commit_pressure = free_commit != 0 && free_commit < (u64{4} << 30);
        }
    }
    if (Tegra::g_nx2_ampere.load(std::memory_order_relaxed) &&
        ShouldReleaseCommandMemory(pool.uses[sub_index], release_interval, commit_pressure)) {
        const auto& dld = device.GetDispatchLoader();
        vk::Check(dld.vkResetCommandBuffer(cmdbuf, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
        if (sub_index == 0 && dld.vkTrimCommandPool) {
            dld.vkTrimCommandPool(*device.GetLogical(), *pool.handle, 0);
        }
    }
#elif defined(__APPLE__)
    // MoltenVK records commands into host memory and keeps a reset buffer's command objects in
    // per-type pools for the next recording. Releasing them and trimming the pool emptied those
    // pools, so the Vulkan worker allocated every command again: the reset, trim and allocations
    // took ~10% of its time in Pokemon Pokopia's world. The implicit reset at Begin is enough.
#else
    // The implicit reset at Begin keeps a command buffer's memory, so each one held on to its
    // largest recording: title 1 grew ~200 of them, ~6 GB of mostly empty write-combined NVIDIA
    // pushbuffer memory charged to the commit limit. Release it into the pool on reuse (the GPU is
    // done with this buffer) and trim the pool once per round of its buffers.
    const auto& dld = device.GetDispatchLoader();
    dld.vkResetCommandBuffer(cmdbuf, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    if (sub_index == 0 && dld.vkTrimCommandPool) {
        dld.vkTrimCommandPool(*device.GetLogical(), *pool.handle, 0);
    }
#endif
    return cmdbuf;
}

} // namespace Vulkan
