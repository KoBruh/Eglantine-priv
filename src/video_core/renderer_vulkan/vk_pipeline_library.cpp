// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <span>
#include <type_traits>

#include <boost/container/static_vector.hpp>

#include "common/cityhash.h"
#include "video_core/renderer_vulkan/pipeline_stats.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

// Writes create info contents as 32-bit words. Pointers are followed, never stored: two equal
// states built in different places give equal words.
class KeyWriter {
public:
    explicit KeyWriter(std::vector<u32>& words_) : words{words_} {}

    void Add(u32 value) {
        words.push_back(value);
    }

    void AddFloat(f32 value) {
        Add(std::bit_cast<u32>(value));
    }

    template <typename Handle>
    void AddHandle(Handle handle) {
        static_assert(sizeof(Handle) == sizeof(u64));
        const u64 value = std::bit_cast<u64>(handle);
        Add(static_cast<u32>(value));
        Add(static_cast<u32>(value >> 32));
    }

    // Copies the bytes of an array of plain structures.
    template <typename T>
    void AddArray(const T* data, u32 count) {
        // Padding bytes or floats (+0/-0) could make equal states give different words.
        static_assert(std::has_unique_object_representations_v<T>);
        static_assert(sizeof(T) % sizeof(u32) == 0);
        Add(count);
        if (count == 0) {
            return;
        }
        const size_t offset = words.size();
        words.resize(offset + count * sizeof(T) / sizeof(u32));
        std::memcpy(words.data() + offset, data, count * sizeof(T));
    }

private:
    std::vector<u32>& words;
};

// Calls `visit` on every structure of a pNext chain. False as soon as `visit` refuses one.
template <typename Visit>
bool ForEachNext(const void* next, Visit&& visit) {
    for (auto* it = static_cast<const VkBaseInStructure*>(next); it != nullptr; it = it->pNext) {
        if (!visit(*it)) {
            return false;
        }
    }
    return true;
}

template <typename T>
const T& As(const VkBaseInStructure& base) {
    return *reinterpret_cast<const T*>(&base);
}

// Each Describe* function writes one state structure, or a marker when it is absent. They return
// false when they meet a structure (in a pNext chain) they do not know: a part described without
// it could be confused with a different one, so such pipelines are compiled whole.

bool DescribeStages(KeyWriter& key, std::span<const VkPipelineShaderStageCreateInfo> stages) {
    key.Add(static_cast<u32>(stages.size()));
    for (const VkPipelineShaderStageCreateInfo& stage : stages) {
        if (stage.pNext != nullptr || stage.flags != 0 || stage.pSpecializationInfo != nullptr ||
            std::strcmp(stage.pName, "main") != 0) {
            return false;
        }
        key.Add(stage.stage);
        // The handle stands for the SPIR-V: the cache keeps the module alive with its key, so the
        // handle cannot be given to another module.
        key.AddHandle(stage.module);
    }
    return true;
}

bool DescribeVertexInput(KeyWriter& key, const VkPipelineVertexInputStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.AddArray(ci->pVertexBindingDescriptions, ci->vertexBindingDescriptionCount);
    key.AddArray(ci->pVertexAttributeDescriptions, ci->vertexAttributeDescriptionCount);
    return ForEachNext(ci->pNext, [&](const VkBaseInStructure& next) {
        if (next.sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
            return false;
        }
        const auto& divisors = As<VkPipelineVertexInputDivisorStateCreateInfoEXT>(next);
        key.Add(next.sType);
        key.AddArray(divisors.pVertexBindingDivisors, divisors.vertexBindingDivisorCount);
        return true;
    });
}

bool DescribeInputAssembly(KeyWriter& key, const VkPipelineInputAssemblyStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.Add(ci->topology);
    key.Add(ci->primitiveRestartEnable);
    return ci->pNext == nullptr;
}

bool DescribeTessellation(KeyWriter& key, const VkPipelineTessellationStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.Add(ci->patchControlPoints);
    return ci->pNext == nullptr;
}

bool DescribeViewport(KeyWriter& key, const VkPipelineViewportStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.Add(ci->viewportCount);
    key.Add(ci->pViewports != nullptr);
    if (ci->pViewports != nullptr) {
        for (u32 index = 0; index < ci->viewportCount; ++index) {
            const VkViewport& viewport = ci->pViewports[index];
            for (const f32 value : {viewport.x, viewport.y, viewport.width, viewport.height,
                                    viewport.minDepth, viewport.maxDepth}) {
                key.AddFloat(value);
            }
        }
    }
    key.Add(ci->scissorCount);
    key.Add(ci->pScissors != nullptr);
    if (ci->pScissors != nullptr) {
        key.AddArray(ci->pScissors, ci->scissorCount);
    }
    return ForEachNext(ci->pNext, [&](const VkBaseInStructure& next) {
        key.Add(next.sType);
        switch (next.sType) {
        case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV: {
            const auto& swizzle = As<VkPipelineViewportSwizzleStateCreateInfoNV>(next);
            key.Add(swizzle.flags);
            key.AddArray(swizzle.pViewportSwizzles, swizzle.viewportCount);
            return true;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
            key.Add(As<VkPipelineViewportDepthClipControlCreateInfoEXT>(next).negativeOneToOne);
            return true;
        default:
            return false;
        }
    });
}

bool DescribeRasterization(KeyWriter& key, const VkPipelineRasterizationStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.Add(ci->depthClampEnable);
    key.Add(ci->rasterizerDiscardEnable);
    key.Add(ci->polygonMode);
    key.Add(ci->cullMode);
    key.Add(ci->frontFace);
    key.Add(ci->depthBiasEnable);
    key.AddFloat(ci->depthBiasConstantFactor);
    key.AddFloat(ci->depthBiasClamp);
    key.AddFloat(ci->depthBiasSlopeFactor);
    key.AddFloat(ci->lineWidth);
    return ForEachNext(ci->pNext, [&](const VkBaseInStructure& next) {
        key.Add(next.sType);
        switch (next.sType) {
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT: {
            const auto& line = As<VkPipelineRasterizationLineStateCreateInfoEXT>(next);
            key.Add(line.lineRasterizationMode);
            key.Add(line.stippledLineEnable);
            key.Add(line.lineStippleFactor);
            key.Add(line.lineStipplePattern);
            return true;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT: {
            const auto& conservative =
                As<VkPipelineRasterizationConservativeStateCreateInfoEXT>(next);
            key.Add(conservative.flags);
            key.Add(conservative.conservativeRasterizationMode);
            key.AddFloat(conservative.extraPrimitiveOverestimationSize);
            return true;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
            key.Add(As<VkPipelineRasterizationProvokingVertexStateCreateInfoEXT>(next)
                        .provokingVertexMode);
            return true;
        default:
            return false;
        }
    });
}

bool DescribeMultisample(KeyWriter& key, const VkPipelineMultisampleStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.Add(ci->rasterizationSamples);
    key.Add(ci->sampleShadingEnable);
    key.AddFloat(ci->minSampleShading);
    key.Add(ci->pSampleMask != nullptr);
    if (ci->pSampleMask != nullptr) {
        key.AddArray(ci->pSampleMask, (static_cast<u32>(ci->rasterizationSamples) + 31) / 32);
    }
    key.Add(ci->alphaToCoverageEnable);
    key.Add(ci->alphaToOneEnable);
    return ci->pNext == nullptr;
}

bool DescribeDepthStencil(KeyWriter& key, const VkPipelineDepthStencilStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.Add(ci->depthTestEnable);
    key.Add(ci->depthWriteEnable);
    key.Add(ci->depthCompareOp);
    key.Add(ci->depthBoundsTestEnable);
    key.Add(ci->stencilTestEnable);
    key.AddArray(&ci->front, 1);
    key.AddArray(&ci->back, 1);
    key.AddFloat(ci->minDepthBounds);
    key.AddFloat(ci->maxDepthBounds);
    return ci->pNext == nullptr;
}

bool DescribeColorBlend(KeyWriter& key, const VkPipelineColorBlendStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci == nullptr) {
        return true;
    }
    key.Add(ci->flags);
    key.Add(ci->logicOpEnable);
    key.Add(ci->logicOp);
    key.AddArray(ci->pAttachments, ci->attachmentCount);
    for (const f32 constant : ci->blendConstants) {
        key.AddFloat(constant);
    }
    return ci->pNext == nullptr;
}

void DescribeDynamicState(KeyWriter& key, const VkPipelineDynamicStateCreateInfo* ci) {
    key.Add(ci != nullptr);
    if (ci != nullptr) {
        key.Add(ci->flags);
        key.AddArray(ci->pDynamicStates, ci->dynamicStateCount);
    }
}

// Writes everything the driver reads from the create info of one part. The pipeline layout is
// written as the identity of the pipeline's whole shader module set, `shaders`:
// - the shader parts are linked without VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, so
//   their layouts must be identically defined (VUID-VkGraphicsPipelineCreateInfo-pLibraries-06613);
// - Eden's layout has one descriptor set with the bindings of all stages, built from the
//   Shader::Info of all of them, and those travel with the module set (ShaderReuseCache).
// So the vertex part of one pipeline can only be linked with the fragment part of a pipeline
// with the same module set, never with another fragment program.
bool DescribePart(KeyWriter& key, const VkGraphicsPipelineCreateInfo& part,
                  VkGraphicsPipelineLibraryFlagsEXT subset, const GraphicsShaderModules* shaders) {
    key.Add(subset);
    if (part.layout != VK_NULL_HANDLE) {
        key.AddHandle(shaders);
    }
    key.Add(part.flags);
    key.AddHandle(part.renderPass);
    key.Add(part.subpass);
    DescribeDynamicState(key, part.pDynamicState);
    return DescribeStages(key, {part.pStages, part.stageCount}) &&
           DescribeVertexInput(key, part.pVertexInputState) &&
           DescribeInputAssembly(key, part.pInputAssemblyState) &&
           DescribeTessellation(key, part.pTessellationState) &&
           DescribeViewport(key, part.pViewportState) &&
           DescribeRasterization(key, part.pRasterizationState) &&
           DescribeMultisample(key, part.pMultisampleState) &&
           DescribeDepthStencil(key, part.pDepthStencilState) &&
           DescribeColorBlend(key, part.pColorBlendState);
}

// The create info of one library: `ci` reduced to the state of `library_info.flags`.
VkGraphicsPipelineCreateInfo MakePartCreateInfo(
    const VkGraphicsPipelineCreateInfo& ci,
    const VkGraphicsPipelineLibraryCreateInfoEXT& library_info,
    std::span<const VkPipelineShaderStageCreateInfo> stages) {
    const VkGraphicsPipelineLibraryFlagsEXT subset = library_info.flags;
    const bool vertex_input = subset == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
    const bool pre_rasterization =
        subset == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
    const bool fragment_shader = subset == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    const bool fragment_output =
        subset == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
    return {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &library_info,
        .flags = ci.flags | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.empty() ? nullptr : stages.data(),
        .pVertexInputState = vertex_input ? ci.pVertexInputState : nullptr,
        .pInputAssemblyState = vertex_input ? ci.pInputAssemblyState : nullptr,
        .pTessellationState = pre_rasterization ? ci.pTessellationState : nullptr,
        .pViewportState = pre_rasterization ? ci.pViewportState : nullptr,
        .pRasterizationState = pre_rasterization ? ci.pRasterizationState : nullptr,
        // Both fragment parts read the multisample state and must get identical ones
        // (VUID-VkGraphicsPipelineCreateInfo-pLibraries-06635); the pre-rasterization part needs
        // it as well (VUID-VkGraphicsPipelineCreateInfo-pRasterizationState-09039).
        .pMultisampleState = vertex_input ? nullptr : ci.pMultisampleState,
        .pDepthStencilState = fragment_shader ? ci.pDepthStencilState : nullptr,
        .pColorBlendState = fragment_output ? ci.pColorBlendState : nullptr,
        // Every part gets the whole list: a part ignores the dynamic states of state it does not
        // hold, and the linked pipeline has those of all its parts.
        .pDynamicState = ci.pDynamicState,
        // Only the shader parts take a layout (VUID-VkGraphicsPipelineCreateInfo-flags-06642).
        .layout = pre_rasterization || fragment_shader ? ci.layout : VK_NULL_HANDLE,
        // Eden draws inside render passes, not with dynamic rendering: all parts but the vertex
        // input one must be created with compatible render passes
        // (VUID-VkGraphicsPipelineCreateInfo-pLibraries-06628), so the render pass is in each of
        // their keys.
        .renderPass = vertex_input ? VK_NULL_HANDLE : ci.renderPass,
        .subpass = vertex_input ? 0 : ci.subpass,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
}

bool HasDynamicState(const VkGraphicsPipelineCreateInfo& ci, VkDynamicState state) {
    if (ci.pDynamicState == nullptr) {
        return false;
    }
    const std::span states{ci.pDynamicState->pDynamicStates, ci.pDynamicState->dynamicStateCount};
    return std::ranges::find(states, state) != states.end();
}

} // Anonymous namespace

struct GraphicsPipelineLibraryCache::Library {
    /// Set once the part is compiled. A compile that throws leaves it unset: the next pipeline
    /// that needs the part compiles it again.
    std::once_flag compiled;
    /// Set after `pipeline`, for FindLibrary, which must not wait for a compile in progress.
    std::atomic_bool ready{false};
    vk::Pipeline pipeline;
    /// Keeps the shader modules named by the key alive, so neither their handles nor the
    /// address of the set can be reused while the key exists (DescribePart).
    std::shared_ptr<const GraphicsShaderModules> shaders;
};

size_t GraphicsPipelineLibraryCache::KeyHash::operator()(const Key& key) const noexcept {
    return static_cast<size_t>(Common::CityHash64(reinterpret_cast<const char*>(key.words.data()),
                                                  key.words.size() * sizeof(u32)));
}

GraphicsPipelineLibraryCache::GraphicsPipelineLibraryCache(const Device& device_)
    : device{device_} {}

GraphicsPipelineLibraryCache::~GraphicsPipelineLibraryCache() = default;

vk::Pipeline GraphicsPipelineLibraryCache::Link(
    const VkGraphicsPipelineCreateInfo& ci, VkPipelineCache pipeline_cache,
    const std::shared_ptr<const GraphicsShaderModules>& shaders, bool cached_parts_only) {
    // Descriptor buffers are the only creation flag the parts can share: every part of a
    // pipeline must have it when one has (VUID-VkPipelineLibraryCreateInfoKHR-pLibraries-08096).
    // Other flags (statistics capture with renderer_debug) are left to monolithic compiles.
    if (ci.pNext != nullptr || (ci.flags & ~VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0) {
        return {};
    }
    boost::container::static_vector<VkPipelineShaderStageCreateInfo, 4> pre_rasterization_stages;
    const VkPipelineShaderStageCreateInfo* fragment_stage{};
    for (u32 index = 0; index < ci.stageCount; ++index) {
        if (ci.pStages[index].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            fragment_stage = &ci.pStages[index];
        } else {
            pre_rasterization_stages.push_back(ci.pStages[index]);
        }
    }
    // Without a fragment program, the fragment shader part would have no stage; with a static
    // rasterizer discard, the pipeline would not take fragment parts at all
    // (VUID-VkGraphicsPipelineCreateInfo-flags-08909). Both are rare: compile those whole.
    if (ci.pRasterizationState == nullptr || pre_rasterization_stages.empty() ||
        fragment_stage == nullptr) {
        return {};
    }
    if (ci.pRasterizationState->rasterizerDiscardEnable == VK_TRUE &&
        !HasDynamicState(ci, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE)) {
        return {};
    }

    struct Part {
        VkGraphicsPipelineLibraryCreateInfoEXT library_info;
        std::span<const VkPipelineShaderStageCreateInfo> stages;
        VkGraphicsPipelineCreateInfo ci;
        Key key;
    };
    const auto library_info = [](VkGraphicsPipelineLibraryFlagsEXT subset) {
        return VkGraphicsPipelineLibraryCreateInfoEXT{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = subset,
        };
    };
    std::array<Part, 4> parts{{
        {library_info(VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT), {}, {}, {}},
        {library_info(VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT),
         {pre_rasterization_stages.data(), pre_rasterization_stages.size()}, {}, {}},
        {library_info(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT),
         {fragment_stage, 1}, {}, {}},
        {library_info(VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT), {}, {}, {}},
    }};
    // Every part is described before any is compiled, so a refused pipeline costs no compile.
    for (Part& part : parts) {
        part.ci = MakePartCreateInfo(ci, part.library_info, part.stages);
        KeyWriter writer{part.key.words};
        if (!DescribePart(writer, part.ci, part.library_info.flags, shaders.get())) {
            return {};
        }
    }
    std::array<VkPipeline, 4> handles{};
    for (size_t index = 0; index < parts.size(); ++index) {
        if (cached_parts_only) {
            handles[index] = FindLibrary(parts[index].key);
            if (handles[index] == VK_NULL_HANDLE) {
                return {};
            }
        } else {
            handles[index] =
                GetLibrary(std::move(parts[index].key), parts[index].ci, pipeline_cache, shaders);
        }
    }
    if (cached_parts_only) {
        // Counted once all are found: a probe that misses a part is not a use of the others.
        for (size_t index = 0; index < parts.size(); ++index) {
            PipelineStats::NoteLibrary(PipelineStats::Registry::LibraryEvent::Hit);
        }
    }

    const PipelineStats::ScopedTimer timer{PipelineStats::Kind::Graphics,
                                           PipelineStats::Stage::Link};
    const VkPipelineLibraryCreateInfoKHR libraries_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
        .pNext = nullptr,
        .libraryCount = static_cast<u32>(handles.size()),
        .pLibraries = handles.data(),
    };
    // Without a VkGraphicsPipelineLibraryCreateInfoEXT, a pipeline created from libraries holds
    // no state of its own: everything comes from the parts. Without
    // VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT, this is the fast link that
    // graphicsPipelineLibraryFastLinking promises; the optimized pipeline is a monolithic compile
    // made later (GraphicsPipeline::QueueOptimizedPipeline).
    return device.GetLogical().CreateGraphicsPipeline(
        {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &libraries_info,
            .flags = ci.flags,
            .stageCount = 0,
            .pStages = nullptr,
            .pVertexInputState = nullptr,
            .pInputAssemblyState = nullptr,
            .pTessellationState = nullptr,
            .pViewportState = nullptr,
            .pRasterizationState = nullptr,
            .pMultisampleState = nullptr,
            .pDepthStencilState = nullptr,
            .pColorBlendState = nullptr,
            .pDynamicState = nullptr,
            // Compatible with the parts' layouts (VUID-VkGraphicsPipelineCreateInfo-layout-07827).
            .layout = ci.layout,
            .renderPass = ci.renderPass,
            .subpass = ci.subpass,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        },
        pipeline_cache);
}

VkPipeline GraphicsPipelineLibraryCache::GetLibrary(
    Key key, const VkGraphicsPipelineCreateInfo& part_ci, VkPipelineCache pipeline_cache,
    const std::shared_ptr<const GraphicsShaderModules>& shaders) {
    std::shared_ptr<Library> library;
    {
        std::scoped_lock lock{mutex};
        auto [it, inserted] = libraries.try_emplace(std::move(key));
        if (inserted) {
            it->second = std::make_shared<Library>();
        }
        library = it->second;
    }
    // Compiled without the lock: other parts compile meanwhile, and threads needing this very
    // part wait in call_once until it is done.
    bool compiled_here = false;
    std::call_once(library->compiled, [&] {
        const PipelineStats::ScopedTimer timer{PipelineStats::Kind::Graphics,
                                               PipelineStats::Stage::Library};
        library->pipeline = device.GetLogical().CreateGraphicsPipeline(part_ci, pipeline_cache);
        if (part_ci.layout != VK_NULL_HANDLE) {
            library->shaders = shaders;
        }
        library->ready.store(true, std::memory_order_release);
        compiled_here = true;
    });
    if (!compiled_here) {
        PipelineStats::NoteLibrary(PipelineStats::Registry::LibraryEvent::Hit);
    }
    return *library->pipeline;
}

VkPipeline GraphicsPipelineLibraryCache::FindLibrary(const Key& key) {
    std::shared_ptr<Library> library;
    {
        std::scoped_lock lock{mutex};
        const auto it = libraries.find(key);
        if (it == libraries.end()) {
            return VK_NULL_HANDLE;
        }
        library = it->second;
    }
    if (!library->ready.load(std::memory_order_acquire)) {
        return VK_NULL_HANDLE;
    }
    return *library->pipeline;
}

} // namespace Vulkan
