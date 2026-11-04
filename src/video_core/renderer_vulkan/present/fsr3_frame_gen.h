// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// AMD FidelityFX Super Resolution 3 (FSR 3) Frame Generation pipeline.
/// Uses compute-based optical flow and bilateral frame interpolation
/// to synthesize intermediate frames without requiring engine motion vectors.
///
/// Optimizations:
///  - Half-resolution optical flow
///  - Early-out for static pixels (skip diamond search)
///  - The flow only runs on frames that are followed by generated frames
///  - Every internal image stays in GENERAL, so the descriptors always match the real layout
///  - Descriptor sets are never written while a submitted or queued command still uses them
class Fsr3FrameGen {
public:
    /// Present frames that can be in flight at once (PresentManager caps them at 7), so the most
    /// destinations a generated frame can be written into.
    static constexpr size_t MAX_TARGETS = 7;

    explicit Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                          VkExtent2D extent, VkFormat format);
    ~Fsr3FrameGen();

    /// Keeps the previous and the new presented frame, and when `estimate_flow` is set, estimates
    /// the motion between them for the Generate calls that follow.
    void Process(Scheduler& scheduler, VkImage current_frame, bool estimate_flow);

    /// Writes the frame at `blend_factor` (0 = previous frame, 1 = new frame) into the present
    /// frame `destination_index`, whose image and storage view are given.
    void Generate(const Device& device, Scheduler& scheduler, u32 destination_index,
                  VkImage destination_image, VkImageView destination_view, float blend_factor);

private:
    void CreateResources(const Device& device);
    void CreateShaders(const Device& device);
    void CreatePipelines(const Device& device);
    void WriteDescriptorSets(const Device& device);

    MemoryAllocator& m_allocator;
    VkExtent2D m_extent;
    VkExtent2D m_flow_extent; // half-res for optical flow
    VkFormat m_format;

    vk::Sampler m_linear_sampler;
    vk::ShaderModule m_flow_shader;
    vk::ShaderModule m_interp_shader;

    vk::DescriptorSetLayout m_flow_desc_layout;
    vk::PipelineLayout m_flow_pipe_layout;
    vk::Pipeline m_flow_pipeline;

    vk::DescriptorSetLayout m_interp_desc_layout;
    vk::PipelineLayout m_interp_pipe_layout;
    vk::Pipeline m_interp_pipeline;

    vk::DescriptorPool m_desc_pool;
    // The flow only reads and writes the images below, so its set is written once.
    vk::DescriptorSets m_flow_desc_sets;
    // One interpolation set per destination present frame, see Generate.
    vk::DescriptorSets m_interp_desc_sets;

    // The previous presented frame
    vk::Image m_history_image;
    vk::ImageView m_history_view;

    // Half-resolution motion from the previous frame to the current one, in flow pixels
    vk::Image m_motion_image;
    vk::ImageView m_motion_view;

    // The latest presented frame
    vk::Image m_current_image;
    vk::ImageView m_current_view;

    bool m_has_history{};
    // The flow was estimated for the frames the history and current images hold now
    bool m_flow_valid{};
};

} // namespace Vulkan
