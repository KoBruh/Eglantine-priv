// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <vector>

#include "common/logging.h"
#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

#include "video_core/host_shaders/fsr3_frame_interpolation_comp_spv.h"
#include "video_core/host_shaders/fsr3_optical_flow_comp_spv.h"

namespace Vulkan {

namespace {

constexpr VkFormat MOTION_FORMAT = VK_FORMAT_R16G16_SFLOAT;

// Binding of the generated frame in the interpolation set, the only one Generate writes.
constexpr u32 INTERP_DESTINATION_BINDING = 3;

constexpr VkPipelineStageFlags COPY_AND_COMPUTE =
    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

// --- Barriers ------------------------------------------------------------------------------
// Every image this pass touches stays in GENERAL, which the copies, the storage writes and the
// sampled reads all accept: the layout the descriptors declare is then always the real one, and
// only a first use (or a discarded destination) transitions, from UNDEFINED.

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                                 VkImageLayout old_layout = VK_IMAGE_LAYOUT_GENERAL) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

void CopyWholeImage(vk::CommandBuffer cmdbuf, VkImage source, VkImage destination,
                    VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL, region);
}

// --- Resources -----------------------------------------------------------------------------

vk::Image CreateImage(MemoryAllocator& allocator, VkExtent2D extent, VkFormat format,
                      VkImageUsageFlags usage) {
    return allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
}

VkWriteDescriptorSet MakeStorageImageWrite(const VkDescriptorImageInfo& info, VkDescriptorSet set,
                                           u32 binding) {
    return VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
}

} // anonymous namespace

struct FlowPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float search_radius;
    float block_size;
};

struct InterpPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float inv_flow_w;
    float inv_flow_h;
    float blend_factor;
};

Fsr3FrameGen::Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                           VkExtent2D extent, VkFormat format)
    : m_allocator{memory_allocator}, m_extent{extent},
      m_flow_extent{std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)},
      m_format{format} {
    CreateResources(device);
    CreateShaders(device);
    CreatePipelines(device);
    WriteDescriptorSets(device);
    LOG_INFO(Render_Vulkan, "FSR 3 frame generation: {}x{} frames, {}x{} flow, format {}",
             m_extent.width, m_extent.height, m_flow_extent.width, m_flow_extent.height,
             static_cast<u32>(m_format));
}

Fsr3FrameGen::~Fsr3FrameGen() = default;

void Fsr3FrameGen::CreateResources(const Device& device) {
    m_linear_sampler = CreateBilinearSampler(device);

    // The history only takes copies of the current-frame image, which takes copies of the
    // presented frame; both are sampled by the two passes.
    m_history_image = CreateImage(m_allocator, m_extent, m_format,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    m_history_view = CreateWrappedImageView(device, m_history_image, m_format);

    m_current_image =
        CreateImage(m_allocator, m_extent, m_format,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT);
    m_current_view = CreateWrappedImageView(device, m_current_image, m_format);

    // Half-resolution motion vector image
    m_motion_image = CreateImage(m_allocator, m_flow_extent, MOTION_FORMAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    m_motion_view = CreateWrappedImageView(device, m_motion_image, MOTION_FORMAT);

    // One flow set (2 sampled images, 1 storage image) and one interpolation set per destination
    // (3 sampled images, 1 storage image).
    constexpr u32 interp_sets = static_cast<u32>(MAX_TARGETS);
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 + 3 * interp_sets},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 + interp_sets},
    };
    m_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 1 + interp_sets,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

void Fsr3FrameGen::CreateShaders(const Device& device) {
    m_flow_shader = BuildShader(device, FSR3_OPTICAL_FLOW_COMP_SPV);
    m_interp_shader = BuildShader(device, FSR3_FRAME_INTERPOLATION_COMP_SPV);
}

void Fsr3FrameGen::CreatePipelines(const Device& device) {
    // --- Optical Flow Pipeline -----------------------------------------------------------------
    const std::array flow_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_flow_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(flow_bindings.size()),
        .pBindings = flow_bindings.data(),
    });

    const VkPushConstantRange flow_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(FlowPushConstants),
    };

    m_flow_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_flow_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &flow_push_range,
    });

    m_flow_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_flow_shader,
            .pName = "main",
        },
        .layout = *m_flow_pipe_layout,
    });

    // --- Frame Interpolation Pipeline ----------------------------------------------------------
    const std::array interp_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{INTERP_DESTINATION_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_interp_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(interp_bindings.size()),
        .pBindings = interp_bindings.data(),
    });

    const VkPushConstantRange interp_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(InterpPushConstants),
    };

    m_interp_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_interp_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &interp_push_range,
    });

    m_interp_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_interp_shader,
            .pName = "main",
        },
        .layout = *m_interp_pipe_layout,
    });

    // Allocate descriptor sets
    m_flow_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, {*m_flow_desc_layout});
    const std::vector<VkDescriptorSetLayout> interp_layouts(MAX_TARGETS, *m_interp_desc_layout);
    m_interp_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, interp_layouts);
}

void Fsr3FrameGen::WriteDescriptorSets(const Device& device) {
    // Everything but the generated frame is one of the images above, so it is written once here,
    // before any command uses the sets. All of them are in GENERAL whenever a pass reads them.
    std::vector<VkDescriptorImageInfo> image_infos;
    image_infos.reserve(2 + 3 * MAX_TARGETS);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(3 + 3 * MAX_TARGETS);

    const VkSampler sampler = *m_linear_sampler;
    const VkDescriptorSet flow_set = m_flow_desc_sets[0];
    writes.push_back(CreateWriteDescriptorSet(image_infos, sampler, *m_current_view, flow_set, 0));
    writes.push_back(CreateWriteDescriptorSet(image_infos, sampler, *m_history_view, flow_set, 1));

    const VkDescriptorImageInfo motion_store{
        .sampler = VK_NULL_HANDLE,
        .imageView = *m_motion_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    writes.push_back(MakeStorageImageWrite(motion_store, flow_set, 2));

    for (size_t target = 0; target < MAX_TARGETS; ++target) {
        const VkDescriptorSet set = m_interp_desc_sets[target];
        writes.push_back(CreateWriteDescriptorSet(image_infos, sampler, *m_current_view, set, 0));
        writes.push_back(CreateWriteDescriptorSet(image_infos, sampler, *m_history_view, set, 1));
        writes.push_back(CreateWriteDescriptorSet(image_infos, sampler, *m_motion_view, set, 2));
    }

    device.GetLogical().UpdateDescriptorSets(writes, {});
}

void Fsr3FrameGen::Process(Scheduler& scheduler, VkImage current_frame, bool estimate_flow) {
    const bool first = !m_has_history;
    m_has_history = true;
    // The first frame has nothing to estimate the motion against.
    m_flow_valid = estimate_flow && !first;

    // Dispatch Optical Flow at half resolution
    const u32 flow_w = m_flow_extent.width;
    const u32 flow_h = m_flow_extent.height;
    const FlowPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(flow_w),
        .inv_render_h = 1.0f / static_cast<float>(flow_h),
        .render_w = static_cast<float>(flow_w),
        .render_h = static_cast<float>(flow_h),
        .search_radius = 2.0f,
        .block_size = 2.0f,
    };

    scheduler.Record([this, current_frame, first, run_flow = m_flow_valid, constants, flow_w,
                      flow_h](vk::CommandBuffer cmdbuf) {
        // The presented frame was just drawn by the window-adapt render pass, which leaves it in
        // GENERAL.
        if (first) {
            // Both copies start out as this frame. The motion image leaves UNDEFINED here too, so
            // every internal image is in GENERAL from now on.
            const std::array barriers{
                MakeBarrier(current_frame, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT),
                MakeBarrier(*m_history_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED),
                MakeBarrier(*m_current_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED),
                MakeBarrier(*m_motion_image, 0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, COPY_AND_COMPUTE,
                                   0, {}, {}, barriers);
            CopyWholeImage(cmdbuf, current_frame, *m_history_image, m_extent);
            CopyWholeImage(cmdbuf, current_frame, *m_current_image, m_extent);
        } else {
            // The previous frame becomes the history: the flow pass compares the new frame
            // against it, and Generate interpolates between the two. The copy waits for the
            // previous frame's passes, which sampled both images.
            const std::array barriers{
                MakeBarrier(current_frame, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT),
                MakeBarrier(*m_history_image, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT),
                MakeBarrier(*m_current_image, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | COPY_AND_COMPUTE,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, barriers);
            CopyWholeImage(cmdbuf, *m_current_image, *m_history_image, m_extent);

            // Only then does the current-frame image take the new frame.
            const auto history_copied =
                MakeBarrier(*m_current_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, history_copied);
            CopyWholeImage(cmdbuf, current_frame, *m_current_image, m_extent);
        }

        if (!run_flow) {
            // No frame is generated after this one: the next Process call picks up from the
            // copies alone.
            return;
        }

        // The flow samples both copies and overwrites the motion the previous Generate calls read.
        const std::array barriers{
            MakeBarrier(*m_history_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            MakeBarrier(*m_current_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            MakeBarrier(*m_motion_image, 0, VK_ACCESS_SHADER_WRITE_BIT),
        };
        cmdbuf.PipelineBarrier(COPY_AND_COMPUTE, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {},
                               barriers);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipe_layout, 0,
                                  m_flow_desc_sets[0], {});
        cmdbuf.PushConstants(*m_flow_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((flow_w + 15) / 16, (flow_h + 15) / 16, 1);

        // Generate samples the motion vectors.
        const auto motion_written =
            MakeBarrier(*m_motion_image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, motion_written);
    });
}

void Fsr3FrameGen::Generate(const Device& device, Scheduler& scheduler, u32 destination_index,
                            VkImage destination_image, VkImageView destination_view,
                            float blend_factor) {
    if (!m_flow_valid || destination_index >= MAX_TARGETS) {
        return;
    }

    // Each present frame has its own set, so the generations of one frame (x3, x4), which go to
    // different present frames, never share one. The caller has just taken this present frame
    // from the present manager, which waits until its previous presentation is done: every
    // command that bound this set before has been recorded and has finished on the GPU, so the
    // set can be written now. Writing it on every call also keeps it right after the frame was
    // recreated, even if the new view got the old view's handle value.
    const VkDescriptorSet set = m_interp_desc_sets[destination_index];
    const VkDescriptorImageInfo destination_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = destination_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    device.GetLogical().UpdateDescriptorSets(
        MakeStorageImageWrite(destination_info, set, INTERP_DESTINATION_BINDING), {});

    // Interpolation dispatches at full resolution
    const u32 out_w = m_extent.width;
    const u32 out_h = m_extent.height;
    const InterpPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(out_w),
        .inv_render_h = 1.0f / static_cast<float>(out_h),
        .render_w = static_cast<float>(out_w),
        .render_h = static_cast<float>(out_h),
        .inv_flow_w = 1.0f / static_cast<float>(m_flow_extent.width),
        .inv_flow_h = 1.0f / static_cast<float>(m_flow_extent.height),
        .blend_factor = blend_factor,
    };

    scheduler.Record([this, set, destination_image, constants, out_w,
                      out_h](vk::CommandBuffer cmdbuf) {
        // The shader writes every pixel, so whatever the frame held (it was just created, or its
        // last presentation is done) is discarded.
        const auto discard =
            MakeBarrier(destination_image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, discard);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipe_layout, 0, set,
                                  {});
        cmdbuf.PushConstants(*m_interp_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((out_w + 15) / 16, (out_h + 15) / 16, 1);

        // The present copy reads the frame in GENERAL, like a frame the window-adapt pass drew.
        const auto generated =
            MakeBarrier(destination_image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, generated);
    });
}

} // namespace Vulkan
