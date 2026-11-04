// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

/// SPIR-V modules of the graphics stages, null for unused stages. Shared by the pipelines whose
/// keys differ only in state the shaders do not see (ShaderReuseCache).
using GraphicsShaderModules =
    std::array<vk::ShaderModule, Tegra::Engines::Maxwell3D::Regs::MaxShaderStage>;

class Device;
class GraphicsPipelineLibraryCache;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        GraphicsPipelineLibraryCache* library_cache, Common::ThreadWorker* optimize_worker,
        const GraphicsPipelineCacheKey& key,
        std::shared_ptr<const GraphicsShaderModules> modules,
        const std::array<const Shader::Info*, NUM_STAGES>& infos);

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }

    /// Switch 2 task/mesh pipeline: drawn with vkCmdDrawMeshTasksEXT.
    [[nodiscard]] bool IsMeshPipeline() const noexcept {
        return stage_infos[0].is_task || stage_infos[0].is_mesh;
    }

    /// A constant buffer slot of one stage (Shader::Info::native_pointer_slots).
    struct NativePointerSlot {
        u32 stage;
        Shader::NativePointerSlot slot;
    };

    /// Native programs: the slots, of every stage, whose storage buffer pointer classification
    /// the shaders were translated with. The pipeline only fits draws where each still has it.
    [[nodiscard]] std::span<const NativePointerSlot> NativePointerSlots() const noexcept {
        return native_pointer_slots;
    }

    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    bool Configure(bool is_indexed) {
        return configure_func(this, is_indexed);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    /// When the pipeline cache last handed this pipeline out, in its use clock's seconds (evicts
    /// long unused ones).
    void MarkUsed(u64 seconds) noexcept {
        last_use_frame = seconds;
    }

    [[nodiscard]] u64 LastUse() const noexcept {
        return last_use_frame;
    }

    /// Forgets the transitions, which may point to pipelines the cache evicted.
    void ClearTransitions() noexcept {
        transition_keys.clear();
        transitions.clear();
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    /// Whether a worker job still uses the pipeline (its build, or its optimized compile, which
    /// starts after IsBuilt): it must not be destroyed until none does.
    [[nodiscard]] bool HasPendingJobs() const noexcept {
        return pending_jobs.load(std::memory_order_acquire) != 0;
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { return pl->ConfigureImpl<Spec>(is_indexed); };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    struct DriverPipeline {
        vk::Pipeline pipeline;
        bool fast_linked{}; ///< Linked from VK_EXT_graphics_pipeline_library parts.
    };

    enum class LinkMode {
        None,            ///< Compile the pipeline whole.
        Link,            ///< Link it from library_cache's parts, compiling those missing; compile
                         ///< it whole when the parts cannot express its state.
        CachedPartsOnly, ///< Link it if every part is compiled already, else create nothing.
    };

    DriverPipeline MakePipeline(VkRenderPass render_pass, LinkMode link_mode);

    /// Builds the pipeline on the calling thread if it can be linked from parts compiled already.
    bool LinkFromCachedParts(RenderPassCache& render_pass_cache,
                             VideoCore::ShaderNotify* shader_notify);

    /// Compiles the pipeline whole on optimize_worker, then draws switch to it.
    void QueueOptimizedPipeline(VkRenderPass render_pass);

    /// The pipeline to bind: the optimized one once it is ready, else `pipeline`.
    VkPipeline BoundPipeline() const noexcept {
        return optimized_ready.load(std::memory_order_acquire) ? *optimized_pipeline : *pipeline;
    }

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;
    u64 last_use_frame{};

    std::shared_ptr<const GraphicsShaderModules> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::vector<NativePointerSlot> native_pointer_slots;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    u32 num_textures{};
    bool fragment_has_color0_output{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    /// Null when pipelines are compiled whole (no VK_EXT_graphics_pipeline_library, disk cache
    /// replay at boot).
    GraphicsPipelineLibraryCache* library_cache{};
    /// Null when fast-linked pipelines are kept as they are.
    Common::ThreadWorker* optimize_worker{};
    /// Written once by optimize_worker, then published by optimized_ready. The fast-linked
    /// `pipeline` stays alive with it: command buffers still in flight may use it.
    vk::Pipeline optimized_pipeline;
    std::atomic_bool optimized_ready{false};
    /// Worker jobs queued with this pipeline (HasPendingJobs). Each job's last access to it is
    /// its decrement; the build's comes after it queued the optimized compile.
    std::atomic<u32> pending_jobs{0};

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
};

} // namespace Vulkan
