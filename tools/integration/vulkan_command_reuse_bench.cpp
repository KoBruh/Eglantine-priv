// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <vulkan/vulkan.h>

void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        std::cerr << "Vulkan failure: " << result << '\n';
        std::exit(1);
    }
}

int main() {
    VkInstance instance;
    const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    Check(vkCreateInstance(&instance_info, nullptr, &instance));
    uint32_t count{};
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (!count)
        return 2;
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    const auto physical = devices.front();
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::cout << "Device: " << properties.deviceName << '\n';
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family{};
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        ++family;
    if (family == count)
        return 3;
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
    const VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1,
                                         &queue};
    VkDevice device;
    Check(vkCreateDevice(physical, &device_info, nullptr, &device));
    const VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
                                         1024 * 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    Check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    uint32_t memory_type{};
    while (!(requirements.memoryTypeBits & (1u << memory_type)))
        ++memory_type;
    const VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                           requirements.size, memory_type};
    VkDeviceMemory memory;
    Check(vkAllocateMemory(device, &memory_info, nullptr, &memory));
    Check(vkBindBufferMemory(device, buffer, memory, 0));
    constexpr unsigned rounds = 512;
    constexpr unsigned commands = 2048;
    for (const unsigned interval : {1, 8, 16, 0}) {
        VkCommandPool pool;
        const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                                family};
        Check(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
        std::array<VkCommandBuffer, 4> buffers;
        const VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                   nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                   4};
        Check(vkAllocateCommandBuffers(device, &allocate, buffers.data()));
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < rounds; ++i) {
            const auto command_buffer = buffers[i % buffers.size()];
            const unsigned uses = i / buffers.size();
            if (uses && interval && uses % interval == 0) {
                Check(vkResetCommandBuffer(command_buffer,
                                           VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
                if (i % buffers.size() == 0)
                    vkTrimCommandPool(device, pool, 0);
            }
            const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                 nullptr,
                                                 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            Check(vkBeginCommandBuffer(command_buffer, &begin));
            for (unsigned j = 0; j < commands; ++j) {
                vkCmdFillBuffer(command_buffer, buffer, (j % 4096) * 256, 256, j);
            }
            Check(vkEndCommandBuffer(command_buffer));
        }
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        std::cout << "release_interval=" << interval << " recording_ms=" << ms
                  << " us_per_buffer=" << ms * 1000 / rounds << '\n';
        vkDestroyCommandPool(device, pool, nullptr);
    }
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}
