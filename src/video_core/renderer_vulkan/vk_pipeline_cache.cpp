// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>
#include <bit>
#include <numeric>
#include "common/cityhash.h"
#include "common/memory_detect.h"
#include "common/scope_exit.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/thread_worker.h"
#include "core/core.h"
#include "shader_recompiler/compilation_budget.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/program_header.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/pipeline_stats.h"
#include "video_core/renderer_vulkan/vertex_repack.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_reuse.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"

#ifdef __ANDROID__
#include "../../android/app/src/main/jni/android_settings.h"
#endif

namespace Vulkan {

namespace {
using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;

// 27: both forks' shader translation changes merged (19 and 26 before).
// 30: above both forks' values (29 here, 19 upstream for the FSEL lowering fix).
// 31: the second upstream merge's lowering changes (upstream is at 21).
constexpr u32 CACHE_VERSION = 31;
constexpr size_t VULKAN_CACHE_FLUSH_PIPELINES = 128;
constexpr size_t VULKAN_CACHE_FLUSH_MIN_SECONDS = 30;
// Not yuzu's and Eden's "yuzuvkch" (see MAGIC_NUMBER in shader_environment.cpp).
constexpr std::array<char, 8> VULKAN_CACHE_MAGIC_NUMBER{'e', 'g', 'l', 'n', 'v', 'k', 'c', 'h'};

constexpr bool IsAmdProprietaryWindows(VkDriverId driver_id) {
#ifdef _WIN32
    return driver_id == VK_DRIVER_ID_AMD_PROPRIETARY;
#else
    return false;
#endif
}

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    UNIMPLEMENTED_MSG("Unimplemented comparison op={}", comparison);
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        ASSERT_MSG(false, "Invalid vertex attribute type!");
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

Shader::InputTopology MaxwellToInputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::InputTopology::Points;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::InputTopology::Lines;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        return Shader::InputTopology::Triangles;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        return Shader::InputTopology::LinesAdjacency;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        return Shader::InputTopology::TrianglesAdjacency;
    }
    return {};
}

Shader::TessPrimitive MaxwellToTessPrimitive(u32 raw) {
    switch (static_cast<Maxwell::Tessellation::DomainType>(raw)) {
    case Maxwell::Tessellation::DomainType::Isolines:
        return Shader::TessPrimitive::Isolines;
    case Maxwell::Tessellation::DomainType::Triangles:
        return Shader::TessPrimitive::Triangles;
    case Maxwell::Tessellation::DomainType::Quads:
        return Shader::TessPrimitive::Quads;
    }
    ASSERT(false);
    return Shader::TessPrimitive::Triangles;
}

Shader::TessSpacing MaxwellToTessSpacing(u32 raw) {
    switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
    case Maxwell::Tessellation::Spacing::Integer:
        return Shader::TessSpacing::Equal;
    case Maxwell::Tessellation::Spacing::FractionalOdd:
        return Shader::TessSpacing::FractionalOdd;
    case Maxwell::Tessellation::Spacing::FractionalEven:
        return Shader::TessSpacing::FractionalEven;
    }
    ASSERT(false);
    return Shader::TessSpacing::Equal;
}

Shader::AttributeType ColorOutputType(u8 render_target_format) {
    const auto format = static_cast<Tegra::RenderTargetFormat>(render_target_format);
    if (format == Tegra::RenderTargetFormat::NONE) {
        // Unused render target: no format to look up.
        return Shader::AttributeType::Float;
    }
    const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
    if (!VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
        return Shader::AttributeType::Float;
    }
    return VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)
               ? Shader::AttributeType::SignedInt
               : Shader::AttributeType::UnsignedInt;
}

// What translation and SPIR-V emission read of a pipeline key, plus the settings they read
// (see vk_shader_reuse.h). MakeRuntimeInfo and the layer passthrough only get this key, so
// nothing they use can be left out of it. A field is only filled when a stage of the pipeline
// reads it: pipelines differing in state their shaders ignore then share the shaders.
ShaderTranslationKey MakeShaderTranslationKey(const GraphicsPipelineCacheKey& pipeline_key,
                                              const Device& device) {
    const FixedPipelineState& state{pipeline_key.state};
    const bool has_tessellation_eval{pipeline_key.unique_hashes[3] != 0};
    const bool has_geometry{pipeline_key.unique_hashes[4] != 0};
    const bool has_fragment{pipeline_key.unique_hashes[5] != 0};

    ShaderTranslationKey key{};
    key.unique_hashes = pipeline_key.unique_hashes;
    key.input_topology = MaxwellToInputTopology(state.topology);
    key.passthrough_topology = MaxwellToOutputTopology(state.topology);
    key.convert_depth_mode = state.ndc_minus_one_to_one != 0;
    key.force_early_z = state.early_z != 0;
    key.y_negate = state.y_negate != 0;
    key.geometry_guardband_xy = state.geometry_guardband_xy;
    key.geometry_guardband_z = state.geometry_guardband_z;

    if (state.dynamic_vertex_input) {
        for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
            key.generic_input_types[index] = AttributeType(state, index);
        }
    } else {
        std::ranges::transform(state.attributes, key.generic_input_types.begin(),
                               &CastAttributeType);
    }
    // A fixed point size is written by the last vertex stage when points are drawn, and by a
    // geometry stage that outputs points (a generated passthrough one outputs what is drawn).
    if (key.input_topology == Shader::InputTopology::Points || has_geometry) {
        key.point_size = state.point_size;
    }
    if (state.xfb_enabled) {
        if (device.IsExtTransformFeedbackSupported()) {
            key.xfb_enabled = true;
            key.xfb_state = state.xfb_state;
        } else {
            LOG_WARNING(Render_Vulkan, "XFB requested in pipeline key but device lacks VK_EXT_transform_feedback; ignoring XFB decorations");
        }
    }
    if (has_tessellation_eval) {
        key.tess_clockwise = state.tessellation_clockwise != 0;
        key.tess_primitive = MaxwellToTessPrimitive(state.tessellation_primitive.Value());
        key.tess_spacing = MaxwellToTessSpacing(state.tessellation_spacing);
    }
    if (has_fragment) {
        key.alpha_test_func = MaxwellToCompareFunction(
            state.UnpackComparisonOp(state.alpha_test_func.Value()));
        key.alpha_test_reference = state.alpha_test_ref;
        key.dual_source_blend = state.attachment0_dual_source_blend != 0;
        if (device.IsMoltenVK()) {
            std::ranges::transform(state.color_formats, key.color_output_types.begin(),
                                   &ColorOutputType);
        }
    }

    const auto& resolution{Settings::values.resolution_info};
    key.resolution_up_scale = resolution.up_scale;
    key.resolution_down_shift = resolution.down_shift;
    key.rescaling_active = resolution.active;
    key.renderer_debug = Settings::values.renderer_debug.GetValue();
    key.disable_shader_loop_safety_checks =
        Settings::values.disable_shader_loop_safety_checks.GetValue();
    key.fix_bloom_effects = Settings::values.fix_bloom_effects.GetValue();
    return key;
}

void SetTransformFeedback(Shader::RuntimeInfo& info, const ShaderTranslationKey& key) {
    if (key.xfb_enabled) {
        auto [varyings, count] = VideoCommon::MakeTransformFeedbackVaryings(key.xfb_state);
        info.xfb_varyings = varyings;
        info.xfb_count = count;
    }
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const ShaderTranslationKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program) {
    Shader::RuntimeInfo info;
    info.geometry_guardband_xy = key.geometry_guardband_xy;
    info.geometry_guardband_z = key.geometry_guardband_z;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    if (previous_program && previous_program->stage == Shader::Stage::Mesh) {
        for (const u32 line : previous_program->mesh.primitive_lines) {
            const auto attribute{static_cast<Shader::IR::Attribute>(line)};
            if (Shader::IR::IsGeneric(attribute)) {
                info.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
        }
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const float point_size{std::bit_cast<float>(key.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.input_topology == Shader::InputTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            SetTransformFeedback(info, key);
            info.convert_depth_mode = key.convert_depth_mode;
        }
        info.generic_input_types = key.generic_input_types;
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.tess_clockwise;
        info.tess_primitive = key.tess_primitive;
        info.tess_spacing = key.tess_spacing;
        break;
    case Shader::Stage::Mesh:
        info.convert_depth_mode = key.convert_depth_mode;
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        SetTransformFeedback(info, key);
        info.convert_depth_mode = key.convert_depth_mode;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = key.alpha_test_func;
        info.alpha_test_reference = std::bit_cast<float>(key.alpha_test_reference);
        info.dual_source_blend = key.dual_source_blend;
        info.color_output_types = key.color_output_types;
        break;
    default:
        break;
    }
    info.input_topology = key.input_topology;
    info.force_early_z = key.force_early_z;
    info.y_negate = key.y_negate;
    return info;
}

// EDEN_NX2_SHADER_REUSE=0: translate every graphics pipeline's shaders, as before (for A/B
// tests). Otherwise pipelines reuse the shaders of an earlier one when they fit (vk_shader_reuse.h).
bool IsShaderReuseAllowed() {
    const char* const value = std::getenv("EDEN_NX2_SHADER_REUSE");
    return value == nullptr || value[0] != '0';
}

// EDEN_NX2_GPL_OPTIMIZE=0: keep fast-linked pipelines as they are. Otherwise each one is also
// compiled whole in the background and replaced when that is done.
bool IsPipelineOptimizationAllowed() {
    const char* const value = std::getenv("EDEN_NX2_GPL_OPTIMIZE");
    return value == nullptr || value[0] != '0';
}

size_t GetTotalPipelineWorkers() {
    const size_t max_core_threads = Shader::CompilationWorkerCount(
        static_cast<size_t>(std::thread::hardware_concurrency()),
        Common::GetMemInfo().TotalPhysicalMemory);
#ifdef __ANDROID__
    const int configured = AndroidSettings::values.pipeline_worker_count.GetValue();
    const int clamped = std::clamp(configured, 2, 8);
    const size_t desired = static_cast<size_t>(clamped);
    if (desired == 0) {
        return 1ULL;
    }
    return std::min(max_core_threads, desired);
#else
    return max_core_threads;
#endif
}

// EDEN_NX2_ASYNC_TRANSLATION=0: translate every graphics pipeline on the GPU thread, as before
// (for A/B tests). Otherwise, with asynchronous shaders on, native graphics pipelines are
// translated on worker threads.
bool IsAsyncTranslationAllowed() {
    const char* const value = std::getenv("EDEN_NX2_ASYNC_TRANSLATION");
    return value == nullptr || value[0] != '0';
}

// Translation threads get half of the pipeline workers' count; driver compilation is queued
// on the pipeline workers.
size_t GetTotalTranslationWorkers() {
    return std::max<size_t>(1, GetTotalPipelineWorkers() / 2);
}

// Optimized builds are not waited for: a quarter of the pipeline workers' count, so they leave
// the CPU to the builds draws wait for.
size_t GetTotalOptimizeWorkers() {
    return std::max<size_t>(1, GetTotalPipelineWorkers() / 4);
}

} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this);
    return static_cast<size_t>(hash);
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    if (unique_hashes != rhs.unique_hashes) {
        return false;
    }
    return std::memcmp(reinterpret_cast<const char*>(this) + sizeof(unique_hashes),
                       reinterpret_cast<const char*>(&rhs) + sizeof(unique_hashes),
                       state.Size()) == 0;
}

namespace {
// NVIDIA's and AMD's drivers keep a disk cache of built pipelines of their own. Eden's copy on top
// only kept every pipeline the title ever built in host memory (title 1: a 245 MB file, ~0.35 GB
// of RAM for the session) and copied it whole at each save, which stalled frames; pipelines built
// no slower without it.
bool DriverCachesPipelines(const Device& device) {
    switch (device.GetDriverID()) {
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
    case VK_DRIVER_ID_AMD_PROPRIETARY:
        return true;
    default:
        return false;
    }
}
} // Anonymous namespace

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             GuestDescriptorQueue& guest_descriptor_queue_,
                             DescriptorBufferRing& descriptor_buffer_ring_,
                             RenderPassCache& render_pass_cache_, BufferCache& buffer_cache_,
                             TextureCache& texture_cache_, VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_},
      device{device_}, scheduler{scheduler_},
      descriptor_pool{descriptor_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      descriptor_buffer_ring{descriptor_buffer_ring_},
      render_pass_cache{render_pass_cache_}, buffer_cache{buffer_cache_},
      texture_cache{texture_cache_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{Settings::values.use_asynchronous_shaders.GetValue()},
      use_vulkan_pipeline_cache{Settings::values.use_vulkan_driver_pipeline_cache.GetValue() &&
                                !DriverCachesPipelines(device_)},
      // Translation threads compile in parallel with the pipeline workers: not on drivers that
      // break with parallel compiles.
      use_async_translation{use_asynchronous_shaders && IsAsyncTranslationAllowed() &&
                            !device.HasBrokenParallelShaderCompiling()},
      // The known-good AMD path predates native shader reuse. Keep it disabled for this driver
      // until its geometry-interface interaction has been validated.
      use_shader_reuse{IsShaderReuseAllowed() &&
                       !IsAmdProprietaryWindows(device_.GetDriverID())},
      // Shader parts are cached per shader module set: without reuse, every pipeline has its own
      // set and would compile its parts and then its optimized build, twice the work for nothing.
      use_pipeline_libraries{device.IsExtGraphicsPipelineLibrarySupported() && use_shader_reuse},
      use_optimized_pipelines{use_pipeline_libraries && IsPipelineOptimizationAllowed()},
      num_translation_threads{use_async_translation ? GetTotalTranslationWorkers() : 0},
      pipeline_libraries{device},
      optimize_workers(use_optimized_pipelines ? GetTotalOptimizeWorkers() : 0,
                       "VkPipelineOptimizer", {}, Common::ThreadPlacement::Background),
      workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers(),
              "VkPipelineBuilder", {}, Common::ThreadPlacement::Background),
      serialization_thread(1, "VkPipelineSerialization", {},
                           Common::ThreadPlacement::Background),
      translation_workers(num_translation_threads, "VkPipelineTranslator", {},
                          Common::ThreadPlacement::Background) {
    const char* tensor_mode = std::getenv("EDEN_NX2_TENSOR");
    const bool tensor_enabled = tensor_mode && std::string_view{tensor_mode} == "fast" &&
                                device.IsExtCooperativeMatrixSupported();
    SetCooperativeMatrixSupported(tensor_enabled);
    if (use_async_translation) {
        LOG_INFO(Render_Vulkan, "Native graphics pipelines are translated on {} worker threads",
                 num_translation_threads);
    }
    LOG_INFO(Render_Vulkan, "Graphics pipelines {} the shaders of earlier pipelines that fit them",
             use_shader_reuse ? "reuse" : "do not reuse (EDEN_NX2_SHADER_REUSE=0)");
    if (device.IsExtGraphicsPipelineLibrarySupported() && !use_pipeline_libraries) {
        LOG_INFO(Render_Vulkan, "Pipeline libraries are not used without shader reuse");
    }
    if (use_pipeline_libraries) {
        LOG_INFO(Render_Vulkan,
                 "Graphics pipelines built for draws are fast-linked from pipeline libraries, {}",
                 use_optimized_pipelines
                     ? fmt::format("then compiled whole on {} background threads",
                                   GetTotalOptimizeWorkers())
                     : std::string{"and kept so (EDEN_NX2_GPL_OPTIMIZE=0)"});
    }
    LOG_INFO(Render_Vulkan, "Shader pipeline workers={} host_threads={} async_shaders={}",
             device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers(),
             std::thread::hardware_concurrency(), use_asynchronous_shaders);
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    if (IsAmdProprietaryWindows(driver_id)) {
        LOG_WARNING(Render_Vulkan,
                    "AMD proprietary driver: disabling the Vulkan driver pipeline cache");
    }
    const VkShaderStageFlags subgroup_stages{device.GetSubgroupSupportedStages()};
    const auto subgroup_stage_bit{[subgroup_stages](VkShaderStageFlags flag, Shader::Stage stage) {
        return (subgroup_stages & flag) != 0 ? (1u << static_cast<u32>(stage)) : 0u;
    }};
    u32 supported_subgroup_stages{
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexA) |
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexB) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                           Shader::Stage::TessellationControl) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                           Shader::Stage::TessellationEval) |
        subgroup_stage_bit(VK_SHADER_STAGE_GEOMETRY_BIT, Shader::Stage::Geometry) |
        subgroup_stage_bit(VK_SHADER_STAGE_FRAGMENT_BIT, Shader::Stage::Fragment) |
        subgroup_stage_bit(VK_SHADER_STAGE_COMPUTE_BIT, Shader::Stage::Compute) |
        subgroup_stage_bit(VK_SHADER_STAGE_TASK_BIT_EXT, Shader::Stage::Task) |
        subgroup_stage_bit(VK_SHADER_STAGE_MESH_BIT_EXT, Shader::Stage::Mesh)};
    if (driver_id == VK_DRIVER_ID_MESA_TURNIP) {
        supported_subgroup_stages &= ~(1u << static_cast<u32>(Shader::Stage::Geometry));
    }
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_int8 = device.IsInt8Supported(),
        .support_uniform_and_storage_buffer_8bit =
            device.IsUniformAndStorageBuffer8BitAccessSupported(),
        .support_storage_buffer_8bit = device.IsStorageBuffer8BitAccessSupported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_uniform_and_storage_buffer_16bit =
            device.IsUniformAndStorageBuffer16BitAccessSupported(),
        .support_storage_buffer_16bit = device.IsStorageBuffer16BitAccessSupported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            float_control.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            float_control.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = float_control.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = float_control.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = float_control.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = float_control.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_workgroup_layout_8bit_access =
            device.IsWorkgroupMemoryExplicitLayout8BitAccessSupported(),
        .support_workgroup_layout_16bit_access =
            device.IsWorkgroupMemoryExplicitLayout16BitAccessSupported(),
        .support_shader_quad_control = device.IsKhrShaderQuadControlSupported(),
        .support_quad_shuffles = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .support_shuffle_relative =
            device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT),
        .supported_subgroup_stages = supported_subgroup_stages,
        .support_viewport_index_layer_non_geometry =
            device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_storage_image_extended_formats = device.IsStorageImageExtendedFormatsSupported(),
        .support_demote_to_helper_invocation =
            device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        .support_shared_int64_atomics = device.IsSharedInt64AtomicsSupported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
        .support_storage_image_array_nonuniform_indexing =
            device.IsStorageImageArrayNonUniformIndexingSupported(),
        .support_uniform_texel_buffer_array_nonuniform_indexing =
            device.IsUniformTexelBufferArrayNonUniformIndexingSupported(),
        .support_storage_texel_buffer_array_nonuniform_indexing =
            device.IsStorageTexelBufferArrayNonUniformIndexingSupported(),

        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),

        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                       driver_id == VK_DRIVER_ID_MESA_RADV ||
                                       driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                       driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA ||
                                       driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_MESA_TURNIP,

        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        .has_broken_spirv_position_input =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
            IsAmdProprietaryWindows(driver_id),
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .has_broken_fp32_denorm_flush = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
            device.GetDriverVersion() < VK_MAKE_VERSION(512, 672, 0),
        // Expanded into one broadcast per lane, which is slow on 64/128-wide Adreno warps; only
        // needed where the driver does not report shuffle support.
        .has_broken_spirv_subgroup_shuffle =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
            !device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_BIT),
        .max_subgroup_size = device.GetMaxSubgroupSize(),
        .support_cooperative_matrix = tensor_enabled,
        .max_compute_shared_memory_size = device.GetMaxComputeSharedMemorySize(),
        .max_tensor_workgroup_size = device.GetMaxTensorWorkgroupSize(),
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances()
    };
    LOG_INFO(Render_Vulkan, "Subgroups: shuffle {}, relative {}, max size {}, warp 32 in compute {}, emulated shuffles {}",
             device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_BIT),
             profile.support_shuffle_relative, profile.max_subgroup_size,
             device.IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT),
             profile.has_broken_spirv_subgroup_shuffle);

    host_info = Shader::HostTranslateInfo{
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_per_stage_descriptor_sampled_images = device.GetMaxPerStageDescriptorSampledImages(),
        .max_per_stage_resources = device.GetMaxPerStageResources(),
        .max_descriptor_set_samplers = device.GetMaxDescriptorSetSamplers(),
        .max_descriptor_set_uniform_buffers = device.GetMaxDescriptorSetUniformBuffers(),
        .max_descriptor_set_uniform_buffers_dynamic = device.GetMaxDescriptorSetUniformBuffersDynamic(),
        .max_descriptor_set_storage_buffers = device.GetMaxDescriptorSetStorageBuffers(),
        .max_descriptor_set_storage_buffers_dynamic = device.GetMaxDescriptorSetStorageBuffersDynamic(),
        .max_descriptor_set_sampled_images = device.GetMaxDescriptorSetSampledImages(),
        .max_descriptor_set_storage_images = device.GetMaxDescriptorSetStorageImages(),
        .max_descriptor_set_input_attachements = device.GetMaxDescriptorSetInputAttachments(),
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .needs_demote_reorder = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
        .single_lane_geometry_subgroups = !profile.SupportsSubgroupStage(Shader::Stage::Geometry),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
    };
    host_info.ApplyDescriptorLimitPolicy();

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputAttributes is too low: {} < {}",
                    device.GetMaxVertexInputAttributes(), Maxwell::NumVertexAttributes);
    }
    if (device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputBindings is too low: {} < {}",
                    device.GetMaxVertexInputBindings(), Maxwell::NumVertexArrays);
    }

    LOG_INFO(Render_Vulkan, "DynamicState setting value: {}", u32(Settings::values.dyna_state.GetValue()));

    dynamic_features = {};
    dynamic_features.driver_id = device.GetDriverID();
    dynamic_features.driver_version = device.GetDriverVersion();

    // User granularity enforced in vulkan_device.cpp switch statement:
    //   Level 0: Core Dynamic States only
    //   Level 1: Core + EDS1
    //   Level 2: Core + EDS1 + EDS2 (accumulative)
    //   Level 3: Core + EDS1 + EDS2 + EDS3 (accumulative)
    // Here we only verify if extensions were successfully loaded by the device

    dynamic_features.has_extended_dynamic_state =
        device.IsExtExtendedDynamicStateSupported();

    dynamic_features.has_extended_dynamic_state_2 =
        device.IsExtExtendedDynamicState2Supported();
    dynamic_features.has_extended_dynamic_state_2_logic_op =
        device.IsExtExtendedDynamicState2ExtrasSupported();
    dynamic_features.has_extended_dynamic_state_2_patch_control_points = false;

    dynamic_features.has_extended_dynamic_state_3_blend =
        device.IsExtExtendedDynamicState3BlendingSupported();
    dynamic_features.has_extended_dynamic_state_3_enables =
        device.IsExtExtendedDynamicState3EnablesSupported();
    dynamic_features.has_color_write_enable =
        device.IsExtColorWriteEnableSupported();
    dynamic_features.has_dynamic_state3_depth_clamp_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3DepthClampEnable();
    dynamic_features.has_dynamic_state3_logic_op_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LogicOpEnable();
    dynamic_features.has_dynamic_state3_line_stipple_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LineStippleEnable();

    // VIDS: Independent toggle (not affected by dyna_state levels)
    dynamic_features.has_dynamic_vertex_input =
        device.IsExtVertexInputDynamicStateSupported() &&
        Settings::values.vertex_input_dynamic_state.GetValue();
    dynamic_features.has_static_vertex_repack =
        StaticVertexRepackEnabled(device.GetMinVertexInputBindingStrideAlignment());

    dynamic_features.has_provoking_vertex = device.IsExtProvokingVertexSupported();
    dynamic_features.has_provoking_vertex_first_mode =
        device.SupportsProvokingVertexFirstMode();
    dynamic_features.has_provoking_vertex_last_mode =
        device.SupportsProvokingVertexLastMode();
    dynamic_features.has_provoking_vertex_tf_preserve =
        device.SupportsTransformFeedbackProvokingVertexPreservation();
}

namespace {
// <title>/vulkan_used.bin: the graphics pipelines recent sessions used, as key hash + sessions
// since the last use.
constexpr u32 UsedPipelinesMagic = 0x31505545; // "EUP1"
// Pipelines the last two sessions used are built at boot. Each holds ~0.45 MB of driver memory:
// title 1 built 4,800 from eight sessions, 1.5 GB more than building them on first use (but that
// stalled frames 4x as often), while one session of its title screen uses ~1,500.
// MoltenVK keeps the previous eight sessions: a pipeline built while playing goes through
// SPIR-V -> MSL -> Metal, far more than the 2-3 ms the shorter window assumes, so each one
// built at boot is a stutter avoided.
// Ocarina still built pipelines for 1-2 s of every 10 s early in play with two sessions kept:
// keep sixteen (#161).
constexpr u8 UsedPipelinesMaxAge = 16;
constexpr u8 UsedPipelinesMaxAgeMoltenVK = 8;

u8 MaxUsedPipelineAge(const Device& device) {
    return device.IsMoltenVK() ? UsedPipelinesMaxAgeMoltenVK : UsedPipelinesMaxAge;
}

std::unordered_map<u64, u8> LoadUsedPipelines(const std::filesystem::path& path, u8 max_age) {
    std::unordered_map<u64, u8> used;
    std::ifstream file{path, std::ios::binary};
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        return used;
    }
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash{};
        u8 age{};
        if (!file.read(reinterpret_cast<char*>(&hash), sizeof(hash)) ||
            !file.read(reinterpret_cast<char*>(&age), sizeof(age))) {
            break;
        }
        if (age < max_age) { // lists saved with a longer window
            used.emplace(hash, age);
        }
    }
    return used;
}

// Without a list of the pipelines recent sessions used (the first boot with this cache, or a session
// that ended before saving it), this many cached graphics pipelines at most are built at boot and the
// rest when first needed. A cache that grew over many builds held 48k pipelines, ~21 GB of driver
// memory built before the game started; the list is only saved at exit or after two minutes, so a
// boot that ran out of memory never saved one and the next boot did the same.
constexpr size_t MaxPipelinesWithoutUsedList = 3000;

// EDEN_NX2_PRELOAD_ALL_PIPELINES=1 builds every cached pipeline at boot, as before.
bool PreloadAllPipelines() {
    static const bool all = [] {
        const char* value = std::getenv("EDEN_NX2_PRELOAD_ALL_PIPELINES");
        return value != nullptr && value[0] == '1';
    }();
    return all;
}

/// A pipeline of the disk cache file, as LoadDiskResources read it.
struct DiskPipeline {
    enum class Kind : u8 {
        Unusable, ///< Task/mesh: never built from the file.
        Foreign,  ///< For other dynamic state support (another GPU or setting): not built here.
        Skipped,  ///< Not used lately: built when first needed.
        Queued,   ///< Built at boot.
    };
    u64 end;      ///< Where it ends in the file.
    u64 hash;     ///< Its disk cache identity (DiskPipelineHash): repeats have the same.
    u64 key_hash; ///< Of its key, as the used list has it.
    Kind kind;
    bool compute;
    bool failed; ///< Its build at boot failed.
};

/// A compute pipeline's disk cache identity: its key and the storage buffer pointer answers its
/// environment gave translation, as DiskPipelineHash for graphics ones.
u64 DiskComputePipelineHash(const ComputePipelineCacheKey& key, const FileEnvironment& env) {
    const u64 hash = key.Hash();
    const u64 answers = env.StoragePointerTypesHash();
    if (answers == 0) {
        return hash;
    }
    return hash ^ (answers + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2));
}

// The graphics pipelines left out at boot were never pruned: a cache that grew over many builds
// held 48k, every one parsed at every boot. A compaction drops the pipelines no later boot builds:
// those the recent sessions did not use (one used again is built when first needed, as it is with
// the entry, and written again then), task/mesh ones, ones for other dynamic state support that no
// recent session used, ones whose build failed, and repeats. It runs once they are half the file
// and this much (so the file stays under twice what the boots read), on the serialization thread.
constexpr u64 MinCompactedBytes = u64{32} << 20;

/// Which pipelines of the file a compaction keeps, or nothing when the file stays as it is.
/// `prune_unused`: the used list says what the recent sessions used.
std::vector<bool> PlanCompaction(std::span<const DiskPipeline> pipelines,
                                 const std::unordered_map<u64, u8>& used_pipelines,
                                 bool prune_unused) {
    std::vector<bool> keep(pipelines.size());
    std::unordered_set<u64> kept_graphics;
    std::unordered_set<u64> kept_compute;
    size_t dropped = 0;
    u64 kept_bytes = 0;
    u64 dropped_bytes = 0;
    u64 begin = 0; // The header counts with the first pipeline.
    for (size_t index = 0; index < pipelines.size(); ++index) {
        const DiskPipeline& pipeline = pipelines[index];
        bool usable = false;
        switch (pipeline.kind) {
        case DiskPipeline::Kind::Unusable:
            break;
        case DiskPipeline::Kind::Foreign:
            usable = !prune_unused || used_pipelines.contains(pipeline.key_hash);
            break;
        case DiskPipeline::Kind::Skipped:
            usable = !prune_unused;
            break;
        case DiskPipeline::Kind::Queued:
            usable = !pipeline.failed;
            break;
        }
        auto& kept = pipeline.compute ? kept_compute : kept_graphics;
        const bool kept_this = usable && kept.insert(pipeline.hash).second;
        keep[index] = kept_this;
        if (kept_this) {
            kept_bytes += pipeline.end - begin;
        } else {
            ++dropped;
            dropped_bytes += pipeline.end - begin;
        }
        begin = pipeline.end;
    }
    if (dropped_bytes < kept_bytes || dropped_bytes < MinCompactedBytes) {
        return {};
    }
    LOG_INFO(Render_Vulkan,
             "Compacting the pipeline cache: {} of {} pipelines ({} of {} MiB) are not needed",
             dropped, pipelines.size(), dropped_bytes >> 20, (kept_bytes + dropped_bytes) >> 20);
    return keep;
}
} // Anonymous namespace

PipelineCache::~PipelineCache() {
    // A queued driver cache flush writes the same temporary file as the one below.
    serialization_thread.WaitForRequests();
    SaveUsedPipelines();
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }
}

void PipelineCache::SaveUsedPipelines() {
    used_pipelines_saved = std::chrono::steady_clock::now();
    // A session too short to say what the game uses (a boot to the menu) keeps the old list.
    constexpr size_t MinimumUsed = 64;
    if (used_pipelines_filename.empty() ||
        used_this_session.size() + used_and_evicted.size() < MinimumUsed) {
        return;
    }
    std::unordered_map<u64, u8> next;
    next.reserve(used_pipelines.size() + used_this_session.size());
    for (const auto& [key, pipeline] : graphics_cache) {
        if (used_this_session.contains(pipeline.get())) {
            next.emplace(key.Hash(), u8{0});
        }
    }
    for (const u64 hash : used_and_evicted) {
        next.emplace(hash, u8{0});
    }
    for (const auto& [hash, age] : used_pipelines) {
        if (age + 1 < MaxUsedPipelineAge(device)) {
            next.try_emplace(hash, static_cast<u8>(age + 1));
        }
    }
    std::ofstream file{used_pipelines_filename, std::ios::binary | std::ios::trunc};
    const u32 count = static_cast<u32>(next.size());
    file.write(reinterpret_cast<const char*>(&UsedPipelinesMagic), sizeof(UsedPipelinesMagic));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [hash, age] : next) {
        file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        file.write(reinterpret_cast<const char*>(&age), sizeof(age));
    }
}

namespace {
/// What GraphicsEnvironment::IsCbufStorageBufferPointer would answer for the slot now.
bool IsGraphicsNativePointer(const Tegra::Engines::Maxwell3D& maxwell3d,
                             const Tegra::MemoryManager& gpu_memory, u32 stage, u32 cbuf_index,
                             u32 cbuf_offset) {
    const auto& cbufs{maxwell3d.state.shader_stages[stage].const_buffers};
    if (cbuf_index >= cbufs.size()) {
        return false;
    }
    const auto& cbuf{cbufs[cbuf_index]};
    return VideoCommon::IsCbufArgumentStorageBufferPointer(gpu_memory, cbuf.address, cbuf.size,
                                                           cbuf_offset);
}

/// What ComputeEnvironment::IsCbufStorageBufferPointer would answer for the slot now.
bool IsComputeNativePointer(const Tegra::Engines::KeplerCompute& kepler_compute,
                            const Tegra::MemoryManager& gpu_memory, u32 cbuf_index,
                            u32 cbuf_offset) {
    const auto& cbufs{kepler_compute.launch_description.const_buffer_config};
    if (cbuf_index >= cbufs.size()) {
        return false;
    }
    const auto& cbuf{cbufs[cbuf_index]};
    return VideoCommon::IsCbufArgumentStorageBufferPointer(gpu_memory, cbuf.Address(),
                                                           cbuf.size.Value(), cbuf_offset);
}

/// One answer per slot, in slot order (NativeVariant::answers).
template <typename Slots, typename Answer>
std::vector<bool> SlotAnswers(const Slots& slots, Answer&& answer) {
    std::vector<bool> answers;
    answers.reserve(slots.size());
    for (const auto& slot : slots) {
        answers.push_back(answer(slot));
    }
    return answers;
}

void ReportNativeVariant(const char* kind, u64 key_hash, size_t count) {
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_INFO(Render_Vulkan,
                 "{} pipeline {:016x}: variant {} for other native storage buffer pointers", kind,
                 key_hash, count);
    }
}

/// The disk cache's identity of a graphics pipeline: its key and, for native programs, the
/// storage buffer pointer answers its environments gave translation. Pipelines of one key built
/// for other answers are separate entries (PipelineCache::NativePointersMatch); for every other
/// pipeline this is the key's hash.
template <typename Environments>
u64 DiskPipelineHash(const GraphicsPipelineCacheKey& key, const Environments& envs) {
    u64 hash{key.Hash()};
    for (const auto& env : envs) {
        if (const u64 answers{env.StoragePointerTypesHash()}; answers != 0) {
            hash ^= answers + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        }
    }
    return hash;
}
} // Anonymous namespace

namespace {
/// EDEN_NX2_PIPELINE_SKIP: unset or 1 skips lookups nothing changed for, 0 never does, verify looks
/// up anyway and reports what skipping would have done.
int PipelineSkipMode() noexcept {
    static constinit std::atomic<int> mode{-1};
    const int known = mode.load(std::memory_order_relaxed);
    if (known >= 0) [[likely]] {
        return known;
    }
    const char* const value = std::getenv("EDEN_NX2_PIPELINE_SKIP");
    int parsed = 1;
    if (value != nullptr && value[0] == '0') {
        parsed = 0;
    } else if (value != nullptr && (value[0] == 'v' || value[0] == 'V')) {
        parsed = 2;
    }
    mode.store(parsed, std::memory_order_relaxed);
    if (parsed != 1) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_PIPELINE_SKIP: {}", parsed == 0 ? "off" : "verify");
    }
    return parsed;
}
} // Anonymous namespace

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        current_pipeline_is_variant = false;
        pipeline_skip.valid = false;
        return nullptr;
    }
    // Most draws keep the previous draw's pipeline state (~90% of Pokopia's). While no register
    // the key reads has changed, nor the stages, topology, engine hint or channel, the key is the
    // one current_pipeline was found for, and re-reading the state and comparing keys finds it
    // again: return it as Next() would.
    const int skip_mode = PipelineSkipMode();
    Tegra::Engines::Maxwell3D& maxwell = *maxwell3d;
    const auto topology = maxwell.draw_manager.draw_state.topology;
    const bool same_inputs = skip_mode != 0 && pipeline_skip.valid && current_pipeline != nullptr &&
                             pipeline_skip.pipeline == current_pipeline &&
                             pipeline_skip.maxwell3d == maxwell3d &&
                             pipeline_skip.topology == topology &&
                             pipeline_skip.engine_state == maxwell.engine_state &&
                             pipeline_skip.unique_hashes == graphics_key.unique_hashes;
    const bool unchanged = same_inputs && !maxwell.pipeline_state_dirty;
    if (unchanged && skip_mode == 1 && NativePointersMatch(*current_pipeline)) {
        current_pipeline->MarkUsed(use_seconds);
        return BuiltPipeline(current_pipeline);
    }
    std::optional<GraphicsPipelineCacheKey> before;
    if (skip_mode == 2) [[unlikely]] {
        before.emplace(graphics_key);
    }
    const bool dirty_only = same_inputs && maxwell.pipeline_state_dirty;
    maxwell.pipeline_state_dirty = false;
    graphics_key.state.Refresh(maxwell, dynamic_features);
    if (before) [[unlikely]] {
        VerifyPipelineSkip(unchanged, dirty_only, maxwell.pipeline_dirty_method, *before);
    }

    GraphicsPipeline* const pipeline = [&]() -> GraphicsPipeline* {
        if (current_pipeline) {
            GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
            // A native pipeline translated for other storage buffer pointers binds the wrong
            // buffers.
            if (next && NativePointersMatch(*next)) {
                // A transition always leads to a pipeline in graphics_cache.
                current_pipeline_is_variant =
                    current_pipeline_is_variant && next == current_pipeline;
                current_pipeline = next;
                current_pipeline->MarkUsed(use_seconds);
                return BuiltPipeline(current_pipeline);
            }
        }
        return CurrentGraphicsPipelineSlowPath();
    }();
    pipeline_skip = PipelineSkip{
        .maxwell3d = maxwell3d,
        .pipeline = current_pipeline,
        .unique_hashes = graphics_key.unique_hashes,
        .topology = topology,
        .engine_state = maxwell.engine_state,
        .valid = current_pipeline != nullptr && current_pipeline->Key() == graphics_key,
    };
    return pipeline;
}

void PipelineCache::VerifyPipelineSkip(bool would_skip, bool dirty_only, u32 dirty_method,
                                       const GraphicsPipelineCacheKey& before) {
    auto& stats = pipeline_skip_stats;
    ++stats.lookups;
    const bool same = before == graphics_key;
    if (would_skip) {
        ++stats.would_skip;
        if (!same) {
            ++stats.mismatches;
            if (stats.mismatches <= 16) {
                const auto* const old_bytes = reinterpret_cast<const u8*>(&before.state);
                const auto* const new_bytes = reinterpret_cast<const u8*>(&graphics_key.state);
                size_t first = 0;
                while (first < sizeof(FixedPipelineState) && old_bytes[first] == new_bytes[first]) {
                    ++first;
                }
                LOG_ERROR(Render_Vulkan,
                          "Pipeline skip would have kept a stale key: pipeline state differs "
                          "from byte {:#x} (no pipeline register written since the last lookup)",
                          first);
            }
        }
    } else if (dirty_only && same) {
        ++stats.false_dirty;
        ++stats.false_dirty_methods[dirty_method];
    }
    if ((stats.lookups & 0x3ffff) != 0) {
        return;
    }
    const auto pct = [&](u64 part) {
        return 100.0 * static_cast<double>(part) / static_cast<double>(stats.lookups);
    };
    LOG_INFO(Render_Vulkan,
             "Pipeline skip (verify): {} lookups, {:.1f}% would skip, {} mismatches, {:.1f}% "
             "looked up only for a register that changed nothing",
             stats.lookups, pct(stats.would_skip), stats.mismatches, pct(stats.false_dirty));
    std::vector<std::pair<u32, u64>> methods(stats.false_dirty_methods.begin(),
                                             stats.false_dirty_methods.end());
    std::sort(methods.begin(), methods.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < methods.size() && i < 16; ++i) {
        LOG_INFO(Render_Vulkan, "  register {:#06x} (method {:#x}): {:.1f}% of lookups",
                 methods[i].first * 4, methods[i].first, pct(methods[i].second));
    }
}

bool PipelineCache::NativePointersMatch(const GraphicsPipeline& pipeline) const {
    for (const auto& [stage, slot] : pipeline.NativePointerSlots()) {
        if (IsGraphicsNativePointer(*maxwell3d, *gpu_memory, stage, slot.cbuf_index,
                                    slot.cbuf_offset) != slot.is_pointer) {
            return false;
        }
    }
    return true;
}

bool PipelineCache::NativePointersMatch(const ComputePipeline& pipeline) const {
    return Shader::NativePointerSlotsMatch(
        pipeline.NativePointerSlots(), [this](u32 cbuf_index, u32 cbuf_offset) {
            return IsComputeNativePointer(*kepler_compute, *gpu_memory, cbuf_index, cbuf_offset);
        });
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineVariant(const GraphicsPipeline& base) {
    std::vector<bool> answers{SlotAnswers(base.NativePointerSlots(), [this](const auto& slot) {
        return IsGraphicsNativePointer(*maxwell3d, *gpu_memory, slot.stage, slot.slot.cbuf_index,
                                       slot.slot.cbuf_offset);
    })};
    auto& variants{graphics_variants[graphics_key]};
    auto it{std::ranges::find(variants, answers, &NativeVariant<GraphicsPipeline>::answers)};
    if (it == variants.end()) {
        // Each pointer that is sometimes null doubles the possible answers: past a few variants
        // of a key, the one used longest ago makes room (destroyed once the GPU is done with it).
        constexpr size_t MaxVariantsPerKey = 16;
        if (variants.size() >= MaxVariantsPerKey) {
            const auto oldest{std::ranges::min_element(variants, {}, [](const auto& variant) {
                return variant.pipeline ? variant.pipeline->LastUse() : u64{0};
            })};
            GraphicsPipeline* const retired{oldest->pipeline.get()};
            if (retired == nullptr || (retired != current_pipeline && !retired->HasPendingJobs())) {
                if (oldest->pipeline) {
                    evicted_pipelines.emplace_back(scheduler.CurrentTick(),
                                                   std::move(oldest->pipeline));
                }
                variants.erase(oldest);
            }
        }
        // Translated from the current state, so for these answers. Kept when the build fails
        // too: draws with these answers then skip without translating again each time.
        ReportNativeVariant("Graphics", graphics_key.Hash(), variants.size() + 1);
        std::unique_ptr<GraphicsPipeline> pipeline{BuildNewGraphicsPipeline()};
        variants.push_back({.answers = std::move(answers), .pipeline = std::move(pipeline)});
        it = std::prev(variants.end());
    }
    GraphicsPipeline* const pipeline{it->pipeline.get()};
    // Its own slots decide; they are the base's unless translation took another path.
    return pipeline && NativePointersMatch(*pipeline) ? pipeline : nullptr;
}

ComputePipeline* PipelineCache::CurrentComputePipelineVariant(const ComputePipelineCacheKey& key,
                                                              const ComputePipeline& base,
                                                              const ShaderInfo* shader) {
    std::vector<bool> answers{SlotAnswers(
        base.NativePointerSlots(), [this](const Shader::NativePointerSlot& slot) {
            return IsComputeNativePointer(*kepler_compute, *gpu_memory, slot.cbuf_index,
                                          slot.cbuf_offset);
        })};
    auto& variants{compute_variants[key]};
    auto it{std::ranges::find(variants, answers, &NativeVariant<ComputePipeline>::answers)};
    if (it == variants.end()) {
        ReportNativeVariant("Compute", key.Hash(), variants.size() + 1);
        NativeVariant<ComputePipeline> variant{.answers = std::move(answers)};
        try {
            variant.pipeline = CreateComputePipeline(key, shader);
        } catch (const Shader::TensorRejected&) {
            variant.rejection = std::current_exception();
        }
        variants.push_back(std::move(variant));
        it = std::prev(variants.end());
    }
    if (it->rejection) {
        std::rethrow_exception(it->rejection);
    }
    ComputePipeline* const pipeline{it->pipeline.get()};
    return pipeline && NativePointersMatch(*pipeline) ? pipeline : nullptr;
}

void PipelineCache::AddCachedGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                              std::unique_ptr<GraphicsPipeline> pipeline) {
    auto [it, is_new]{graphics_cache.try_emplace(key)};
    if (is_new || !it->second) {
        it->second = std::move(pipeline);
        return;
    }
    // Replayed in any order: whichever came first stands for the key, the others are variants
    // named by their answers for its slots. One without all of its slots can't be named and is
    // dropped; it is translated again if a draw needs it.
    const GraphicsPipeline& base{*it->second};
    const auto slots{pipeline->NativePointerSlots()};
    std::vector<bool> answers;
    for (const auto& base_slot : base.NativePointerSlots()) {
        const auto found{std::ranges::find_if(slots, [&base_slot](const auto& other) {
            return other.stage == base_slot.stage &&
                   other.slot.cbuf_index == base_slot.slot.cbuf_index &&
                   other.slot.cbuf_offset == base_slot.slot.cbuf_offset;
        })};
        if (found == slots.end()) {
            return;
        }
        answers.push_back(found->slot.is_pointer);
    }
    const std::vector<bool> base_answers{SlotAnswers(
        base.NativePointerSlots(), [](const auto& base_slot) { return base_slot.slot.is_pointer; })};
    auto& variants{graphics_variants[key]};
    if (answers == base_answers ||
        std::ranges::find(variants, answers, &NativeVariant<GraphicsPipeline>::answers) !=
            variants.end()) {
        return;
    }
    variants.push_back({.answers = std::move(answers), .pipeline = std::move(pipeline)});
}

void PipelineCache::AddCachedComputePipeline(const ComputePipelineCacheKey& key,
                                             std::unique_ptr<ComputePipeline> pipeline) {
    auto [it, is_new]{compute_cache.try_emplace(key)};
    if (is_new || !it->second) {
        it->second = std::move(pipeline);
        return;
    }
    // As AddCachedGraphicsPipeline.
    const ComputePipeline& base{*it->second};
    const auto slots{pipeline->NativePointerSlots()};
    std::vector<bool> answers;
    for (const Shader::NativePointerSlot& slot : base.NativePointerSlots()) {
        const auto found{std::ranges::find_if(slots, [&](const Shader::NativePointerSlot& other) {
            return other.cbuf_index == slot.cbuf_index && other.cbuf_offset == slot.cbuf_offset;
        })};
        if (found == slots.end()) {
            return;
        }
        answers.push_back(found->is_pointer);
    }
    const std::vector<bool> base_answers{SlotAnswers(
        base.NativePointerSlots(), [](const Shader::NativePointerSlot& slot) {
            return slot.is_pointer;
        })};
    auto& variants{compute_variants[key]};
    if (answers == base_answers ||
        std::ranges::find(variants, answers, &NativeVariant<ComputePipeline>::answers) !=
            variants.end()) {
        return;
    }
    variants.push_back({.answers = std::move(answers), .pipeline = std::move(pipeline)});
}

ComputePipeline* PipelineCache::CurrentComputePipeline() {

    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    if (const auto rejected = rejected_tensor_pipelines.find(key);
        rejected != rejected_tensor_pipelines.end()) {
        std::rethrow_exception(rejected->second);
    }
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (!is_new) {
        // A native shader translated for other storage buffer pointers binds the wrong buffers.
        if (pipeline && !NativePointersMatch(*pipeline)) {
            return CurrentComputePipelineVariant(key, *pipeline, shader);
        }
        return pipeline.get();
    }
    try {
        pipeline = CreateComputePipeline(key, shader);
    } catch (const Shader::TensorRejected&) {
        compute_cache.erase(pair);
        rejected_tensor_pipelines.emplace(key, std::current_exception());
        throw;
    }
    return pipeline.get();
}

namespace {
// Eglantine's caches used to sit in shader/<title id>, next to Eden's, before they got their own
// folder (ShaderDir) and file magic. Its files there are left over: they are told from Eden's by
// their cache versions, 26 to 31, which Eden's caches never had (Eden is at 21).
void RemoveOldSharedShaderCache(u64 title_id) {
    const auto shared_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir).parent_path() /
                          fmt::format("{:016x}", title_id)};
    const auto is_eglantines = [](const std::filesystem::path& path,
                                  const std::array<char, 8>& magic) {
        std::ifstream file(path, std::ios::binary);
        std::array<char, 8> found{};
        u32 version{};
        file.read(found.data(), found.size()).read(reinterpret_cast<char*>(&version), sizeof(version));
        return file.good() && found == magic && version >= 26 && version <= 31;
    };
    constexpr std::array<char, 8> OldPipelineMagic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
    constexpr std::array<char, 8> OldDriverCacheMagic{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};
    size_t removed = 0;
    if (is_eglantines(shared_dir / "vulkan.bin", OldPipelineMagic)) {
        removed += Common::FS::RemoveFile(shared_dir / "vulkan.bin") ? 1 : 0;
        // The used-pipeline list is Eglantine's own file, kept next to its pipeline cache.
        removed += Common::FS::RemoveFile(shared_dir / "vulkan_used.bin") ? 1 : 0;
    }
    if (is_eglantines(shared_dir / "vulkan_pipelines.bin", OldDriverCacheMagic)) {
        removed += Common::FS::RemoveFile(shared_dir / "vulkan_pipelines.bin") ? 1 : 0;
    }
    if (removed != 0) {
        LOG_INFO(Render_Vulkan, "Removed {} of Eglantine's old shader cache files from {}",
                 removed, Common::FS::PathToUTF8String(shared_dir));
    }
}
} // Anonymous namespace

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    RemoveOldSharedShaderCache(title_id);
    if (!pipeline_cache_filename.empty()) {
        serialization_thread.WaitForRequests();
        if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                         CACHE_VERSION);
        }
    }

    const auto shader_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    // CreateDirs: Eglantine's shader directory is inside the shared one, which may not exist yet.
    if (!Common::FS::CreateDirs(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "vulkan.bin";
    used_pipelines_filename = base_dir / "vulkan_used.bin";
    used_pipelines = LoadUsedPipelines(used_pipelines_filename, MaxUsedPipelineAge(device));
    used_pipelines_saved = std::chrono::steady_clock::now();
    size_t skipped_pipelines = 0;
    size_t queued_graphics = 0;

    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
    }

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
        std::unique_ptr<PipelineStatistics> statistics;
        std::vector<size_t> failed; ///< Indices in disk_entries of the builds that failed
    } state;

    // Every pipeline of the file, in file order (filled on this thread while the file is read).
    std::vector<DiskPipeline> disk_entries;
    const auto add_entry{[&disk_entries](std::ifstream& file, u64 hash, u64 key_hash,
                                         DiskPipeline::Kind kind, bool compute) {
        disk_entries.push_back({
            .end = static_cast<u64>(static_cast<std::streamoff>(file.tellg())),
            .hash = hash,
            .key_hash = key_hash,
            .kind = kind,
            .compute = compute,
            .failed = false,
        });
        return disk_entries.size() - 1;
    }};

    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }
    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        const size_t entry = add_entry(file, DiskComputePipelineHash(key, env), key.Hash(),
                                       DiskPipeline::Kind::Queued, true);

        workers.QueueWork([this, key, env_ = std::move(env), &state, &callback, entry]() mutable {
            const PipelineStats::ScopedRole stats_role{PipelineStats::Role::Boot};
            ShaderPools pools;
            auto pipeline{CreateComputePipeline(pools, key, env_, state.statistics.get(), false)};
            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                AddCachedComputePipeline(key, std::move(pipeline));
            } else {
                state.failed.push_back(entry);
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        const u64 hash = key.Hash();
        const u64 disk_hash = DiskPipelineHash(key, envs);

        // Task/mesh pipelines are not stored (their mesh registers are not in the format); skip
        // any that were.
        if (std::ranges::any_of(envs, [](const FileEnvironment& env) {
                return env.ShaderStage() == Shader::Stage::Task ||
                       env.ShaderStage() == Shader::Stage::Mesh;
            })) {
            add_entry(file, disk_hash, hash, DiskPipeline::Kind::Unusable, false);
            return;
        }

        if ((key.state.extended_dynamic_state != 0) !=
                dynamic_features.has_extended_dynamic_state ||
            (key.state.extended_dynamic_state_2 != 0) !=
                dynamic_features.has_extended_dynamic_state_2 ||
            (key.state.extended_dynamic_state_2_logic_op != 0) !=
                dynamic_features.has_extended_dynamic_state_2_logic_op ||
            (key.state.extended_dynamic_state_3_blend != 0) !=
                dynamic_features.has_extended_dynamic_state_3_blend ||
            (key.state.extended_dynamic_state_3_enables != 0) !=
                dynamic_features.has_extended_dynamic_state_3_enables ||
            (key.state.color_write_enable_dynamic != 0) !=
                dynamic_features.has_color_write_enable ||
            (key.state.dynamic_vertex_input != 0) !=
                dynamic_features.has_dynamic_vertex_input) {
            add_entry(file, disk_hash, hash, DiskPipeline::Kind::Foreign, false);
            return;
        }

        const bool key_requests_provoking_last = key.state.provoking_vertex_last != 0;
        if (key_requests_provoking_last && !dynamic_features.has_provoking_vertex_last_mode) {
            add_entry(file, disk_hash, hash, DiskPipeline::Kind::Foreign, false);
            return;
        }

        const bool key_uses_transform_feedback = key.state.xfb_enabled != 0;
        if (key_uses_transform_feedback && key_requests_provoking_last &&
            !dynamic_features.has_provoking_vertex_tf_preserve) {
            add_entry(file, disk_hash, hash, DiskPipeline::Kind::Foreign, false);
            return;
        }

        if (!PreloadAllPipelines() &&
            (used_pipelines.empty() ? queued_graphics >= MaxPipelinesWithoutUsedList
                                    : !used_pipelines.contains(hash))) {
            ++skipped_pipelines;
            add_entry(file, disk_hash, hash, DiskPipeline::Kind::Skipped, false);
            return;
        }
        ++queued_graphics;
        const size_t entry = add_entry(file, disk_hash, hash, DiskPipeline::Kind::Queued, false);

        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback, entry]() mutable {
            const PipelineStats::ScopedRole stats_role{PipelineStats::Role::Boot};
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram>
                env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs),
                                                 state.statistics.get(), false, false)};

            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                AddCachedGraphicsPipeline(key, std::move(pipeline));
            } else {
                state.failed.push_back(entry);
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics);
    // A damaged file was deleted: the pipelines read before the damage are no longer on disk,
    // so let SerializeGraphicsPipeline write them again.
    if (!Common::FS::Exists(pipeline_cache_filename)) {
        disk_entries.clear();
    }

    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);
    if (skipped_pipelines != 0) {
        LOG_INFO(Render_Vulkan,
                 "{} cached graphics pipelines are built when first needed",
                 skipped_pipelines);
    }

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    workers.WaitForRequests(stop_loading);

    // The pipelines the file holds for SerializeGraphicsPipeline, once their builds are done. One
    // whose build failed is not among them, so it is written again when a draw builds it: marked
    // when read, a failing entry kept that pipeline from being stored again at every boot. A
    // stopped load leaves builds unfinished: their pipelines count as stored, and the file stays.
    const bool complete = !stop_loading.stop_requested();
    {
        std::scoped_lock failed_lock{state.mutex};
        for (const size_t entry : state.failed) {
            disk_entries[entry].failed = true;
        }
    }
    const std::vector<bool> keep =
        complete ? PlanCompaction(disk_entries, used_pipelines,
                                  !used_pipelines.empty() && !PreloadAllPipelines())
                 : std::vector<bool>{};
    disk_pipelines.clear();
    for (size_t index = 0; index < disk_entries.size(); ++index) {
        const DiskPipeline& pipeline = disk_entries[index];
        const bool stored = keep.empty() || keep[index];
        if (stored && !pipeline.compute && !pipeline.failed &&
            (pipeline.kind == DiskPipeline::Kind::Skipped ||
             pipeline.kind == DiskPipeline::Kind::Queued)) {
            disk_pipelines.insert(pipeline.hash);
        }
    }
    if (!keep.empty()) {
        // Queued before any pipeline a draw stores, which then goes to the compacted file.
        std::vector<VideoCommon::PipelineCacheEntry> entries(disk_entries.size());
        for (size_t index = 0; index < disk_entries.size(); ++index) {
            entries[index] = {.end = disk_entries[index].end, .keep = keep[index]};
        }
        const auto kept = std::count(keep.begin(), keep.end(), true);
        serialization_thread.QueueWork(
            [filename = pipeline_cache_filename, entries = std::move(entries), kept] {
                if (VideoCommon::CompactPipelineCache(filename, entries)) {
                    LOG_INFO(Render_Vulkan, "Pipeline cache compacted to {} pipelines", kept);
                }
            });
    }

    if (use_vulkan_pipeline_cache) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        last_flush = std::chrono::steady_clock::now();
    }

    if (state.statistics) {
        state.statistics->Report();
    }
}

void PipelineCache::QueueVulkanPipelineCacheFlush() {
    if (!use_vulkan_pipeline_cache || vulkan_pipeline_cache_filename.empty()) {
        return;
    }
    if (++pipelines_since_flush < VULKAN_CACHE_FLUSH_PIPELINES) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto megabytes = last_cache_size.load(std::memory_order_relaxed) / (1024 * 1024);
    const std::chrono::seconds interval{
        std::max<size_t>(VULKAN_CACHE_FLUSH_MIN_SECONDS, megabytes)};
    if (last_flush.time_since_epoch().count() != 0 && now - last_flush < interval) {
        return;
    }
    if (flush_in_flight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    pipelines_since_flush = 0;
    last_flush = now;
    serialization_thread.QueueWork([this] {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        flush_in_flight.store(false, std::memory_order_release);
    });
}

namespace {
// EDEN_NX2_PIPELINE_DIFF=1: for a new graphics pipeline whose shaders an earlier one uses too,
// log which parts of the fixed state differ from that one (finds state that splits the same
// draws into many pipelines, each translated on the GPU thread).
void ReportNewPipelineKey(const GraphicsPipelineCacheKey& key) {
    static const bool enabled = std::getenv("EDEN_NX2_PIPELINE_DIFF") != nullptr;
    if (!enabled) {
        return;
    }
    static std::unordered_map<u64, GraphicsPipelineCacheKey> last_by_shaders;
    static u32 logged{};
    const u64 shaders = Common::CityHash64(reinterpret_cast<const char*>(key.unique_hashes.data()),
                                           sizeof(key.unique_hashes));
    const auto [it, first] = last_by_shaders.try_emplace(shaders, key);
    if (first) {
        return;
    }
    const FixedPipelineState& now = key.state;
    const FixedPipelineState& before = it->second.state;
    std::string diffs;
    if (now.raw1 != before.raw1) diffs += fmt::format(" raw1^{:#x}", now.raw1 ^ before.raw1);
    if (now.raw2 != before.raw2) diffs += fmt::format(" raw2^{:#x}", now.raw2 ^ before.raw2);
    if (now.color_formats != before.color_formats) {
        diffs += fmt::format(" color_formats {:02x} -> {:02x}", fmt::join(before.color_formats, ""),
                             fmt::join(now.color_formats, ""));
    }
    if (now.alpha_test_ref != before.alpha_test_ref) {
        diffs += fmt::format(" alpha_test_ref {:#x} -> {:#x} (func {})", before.alpha_test_ref,
                             now.alpha_test_ref, now.alpha_test_func.Value());
    }
    if (now.point_size != before.point_size) {
        diffs += fmt::format(" point_size {:#x} -> {:#x}", before.point_size, now.point_size);
    }
    if (now.viewport_swizzles != before.viewport_swizzles) diffs += " viewport_swizzles";
    if (now.attribute_types != before.attribute_types) {
        diffs += fmt::format(" attribute_types^{:#x}", now.attribute_types ^ before.attribute_types);
    }
    if (std::memcmp(&now.dynamic_state, &before.dynamic_state, sizeof(now.dynamic_state)) != 0) {
        diffs += " dynamic_state";
    }
    if (now.driver_id != before.driver_id || now.driver_version != before.driver_version) {
        diffs += " driver";
    }
    const size_t size = now.Size();
    if (diffs.empty() && size > offsetof(FixedPipelineState, attachments)) {
        diffs = fmt::format(" past attachments (key size {})", size);
    }
    it->second = key;
    if (logged++ < 400) {
        LOG_INFO(Render_Vulkan, "New pipeline for known shaders {:016x} differs in:{}", shaders,
                 diffs.empty() ? std::string{" nothing in the key prefix"} : diffs);
    }
}
} // Anonymous namespace

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    auto it{graphics_cache.find(graphics_key)};
    if (it == graphics_cache.end()) {
        ReportNewPipelineKey(graphics_key);
        it = graphics_cache.emplace(graphics_key, BuildNewGraphicsPipeline()).first;
    }
    auto& pipeline{it->second};
    if (!pipeline) {
        return nullptr;
    }
    if (used_this_session.insert(pipeline.get()).second &&
        std::chrono::steady_clock::now() - used_pipelines_saved > std::chrono::seconds{15}) {
        SaveUsedPipelines();
    }
    if (!NativePointersMatch(*pipeline)) {
        // Translated for other storage buffer pointers. The pipeline in the cache still stands
        // for the key (the used list, eviction); the variant gets no transitions, which only
        // link pipelines in graphics_cache.
        pipeline->MarkUsed(use_seconds);
        GraphicsPipeline* const variant{CurrentGraphicsPipelineVariant(*pipeline)};
        if (!variant) {
            return nullptr;
        }
        current_pipeline = variant;
        current_pipeline_is_variant = true;
        current_pipeline->MarkUsed(use_seconds);
        return BuiltPipeline(current_pipeline);
    }
    if (current_pipeline && !current_pipeline_is_variant) {
        current_pipeline->AddTransition(pipeline.get());
    }
    current_pipeline = pipeline.get();
    current_pipeline_is_variant = false;
    current_pipeline->MarkUsed(use_seconds);
    return BuiltPipeline(current_pipeline);
}

void PipelineCache::TickFrame() {
    ++frame_count;
    use_seconds = static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::steady_clock::now() - use_clock_start)
                                       .count());
    while (!evicted_pipelines.empty() && scheduler.IsFree(evicted_pipelines.front().first)) {
        evicted_pipelines.pop_front();
    }
    constexpr u64 EvictCheckFrames = 600;
    if (frame_count % EvictCheckFrames == 0) {
        EvictUnusedPipelines();
    }
}

void PipelineCache::EvictUnusedPipelines() {
    // Building a pipeline again on MoltenVK means a Metal compile mid-game, far dearer than the
    // 2-3 ms below, so none are evicted there.
    if (device.IsMoltenVK()) {
        return;
    }
    // Each built pipeline holds ~0.45 MB of driver memory and none was ever destroyed, so it grew
    // with every place a session went (title 1: 1,500 on its title screen alone). One unused for
    // this long is destroyed; building it again takes 3-6 ms of the GPU thread (translation) plus
    // the driver's build, a hitch where it reappears. Counted in seconds, not frames: 9000 frames
    // were 75 s at 120 fps, and Pokemon Pokopia rebuilt ~150 evicted pipelines in a 4-minute
    // session. A cache this small isn't worth any rebuild (~0.45 MB each).
    constexpr u64 EvictAfterSeconds = 600;
    constexpr size_t MinPipelinesToEvict = 1500;
    if (use_seconds <= EvictAfterSeconds || graphics_cache.size() < MinPipelinesToEvict) {
        return;
    }
    const u64 cutoff = use_seconds - EvictAfterSeconds;
    const u64 tick = scheduler.CurrentTick();
    size_t evicted = 0;
    size_t busy = 0;
    for (auto it = graphics_cache.begin(); it != graphics_cache.end();) {
        GraphicsPipeline* const pipeline = it->second.get();
        if (pipeline == nullptr || pipeline == current_pipeline || !pipeline->IsBuilt() ||
            pipeline->LastUse() >= cutoff) {
            ++it;
            continue;
        }
        // A fast-linked pipeline is built before its optimized compile runs, and that queue can
        // be minutes behind: destroyed now, the queued job would compile from freed memory.
        // Evicted at a later check instead.
        if (pipeline->HasPendingJobs()) {
            ++busy;
            ++it;
            continue;
        }
        // The key's variants (built for other native storage buffer pointers) go with it. Using
        // one marks the pipeline in the cache used as well, so none was used since the cutoff.
        const auto variants{graphics_variants.find(it->first)};
        if (variants != graphics_variants.end() &&
            std::ranges::any_of(variants->second, [this](const auto& variant) {
                const GraphicsPipeline* const other{variant.pipeline.get()};
                return other != nullptr && (other == current_pipeline || other->HasPendingJobs());
            })) {
            ++busy;
            ++it;
            continue;
        }
        if (used_this_session.erase(pipeline) != 0) {
            used_and_evicted.insert(it->first.Hash());
        }
        if (variants != graphics_variants.end()) {
            for (auto& variant : variants->second) {
                if (variant.pipeline) {
                    evicted_pipelines.emplace_back(tick, std::move(variant.pipeline));
                    ++evicted;
                }
            }
            graphics_variants.erase(variants);
        }
        evicted_pipelines.emplace_back(tick, std::move(it->second));
        it = graphics_cache.erase(it);
        ++evicted;
    }
    if (evicted == 0) {
        return;
    }
    for (auto& [key, pipeline] : graphics_cache) {
        if (pipeline) {
            pipeline->ClearTransitions();
        }
    }
    LOG_INFO(Render_Vulkan,
             "Evicted {} graphics pipelines unused for {} s, {} left ({} kept for queued work)",
             evicted, EvictAfterSeconds, graphics_cache.size(), busy);
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) const noexcept {
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    if (maxwell3d->GetNativeMacroEngine().native_turing || !use_asynchronous_shaders) {
        return pipeline;
    }
    return CanSkipDraw() ? nullptr : pipeline;
}

bool PipelineCache::CanSkipDraw() const noexcept {
    // If something is using depth, we can assume that games are not rendering anything which
    // will be used one time.
    if (maxwell3d->regs.zeta_enable) {
        return true;
    }
    // If games are using a small index count, we can assume these are full screen quads.
    // Usually these shaders are only used once for building textures so we can assume they
    // can't be built async
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    if (draw_state.draw_element_count <= 6) {
        return false;
    }
    return true;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::BuildNewGraphicsPipeline() {
    if (CanTranslateAsynchronously()) {
        // With an earlier pipeline's shaders only the driver compile is left, and it goes to the
        // pipeline workers: no translation worker is needed.
        if (std::unique_ptr<GraphicsPipeline> pipeline{CreateGraphicsPipelineFromReusedShaders()}) {
            return pipeline;
        }
        if (std::optional<std::future<TranslatedGraphicsPipeline>> translated{
                QueueGraphicsTranslation()}) {
            return AdoptTranslatedGraphicsPipeline(*translated);
        }
    }
    return CreateGraphicsPipeline();
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel, bool link_from_libraries, bool allow_translation) try {
    auto hash = key.Hash();
    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);
    const ShaderTranslationKey translation_key{MakeShaderTranslationKey(key, device)};
    std::shared_ptr<const ReusableShaders> shaders{FindReusableShaders(translation_key, envs, hash)};
    if (!shaders && !allow_translation) {
        return nullptr;
    }
    PipelineStats::NotePipeline(
        PipelineStats::Kind::Graphics,
        Common::CityHash64(reinterpret_cast<const char*>(key.unique_hashes.data()),
                           sizeof(key.unique_hashes)));
    if (!shaders) {
        shaders = BuildGraphicsShaders(pools, translation_key, envs, hash);
        if (!shaders) {
            return nullptr;
        }
        if (use_shader_reuse) {
            shader_reuse.Add(translation_key, shaders);
        }
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    const bool link{link_from_libraries && use_pipeline_libraries};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring, thread_worker, statistics,
        render_pass_cache, link ? &pipeline_libraries : nullptr,
        link && use_optimized_pipelines ? &optimize_workers : nullptr, key, shaders->modules,
        shaders->InfoPointers());

} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    auto hash = key.Hash();
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        // The CFG throws again for the stage that failed its flow analysis, and a throw out of this
        // handler would end the process; that stage is still the one worth dumping.
        try {
            const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        } catch (const Shader::Exception&) {
        }
        env.Dump(hash, key.unique_hashes[index]);
    }
    return nullptr;
}

std::shared_ptr<const ReusableShaders> PipelineCache::FindReusableShaders(
    const ShaderTranslationKey& key, std::span<Shader::Environment* const> envs,
    u64 pipeline_hash) const {
    if (!use_shader_reuse) {
        return nullptr;
    }
    ShaderReuseCache::Lookup found;
    {
        const PipelineStats::ScopedTimer reuse_timer{PipelineStats::Kind::Graphics,
                                                     PipelineStats::Stage::Reuse};
        found = shader_reuse.Find(key, envs);
    }
    if (found.shaders || found.rejected) {
        PipelineStats::NoteReuse(PipelineStats::Kind::Graphics, found.shaders != nullptr);
    }
    if (found.shaders && Settings::values.dump_guest_shaders) {
        // What BuildGraphicsShaders dumps for a pipeline it translates.
        size_t env_index{0};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                envs[env_index++]->Dump(pipeline_hash, key.unique_hashes[index]);
            }
        }
    }
    return found.shaders;
}

bool PipelineCache::HasReusableShaders(const GraphicsPipelineCacheKey& key) const {
    return use_shader_reuse && shader_reuse.Contains(MakeShaderTranslationKey(key, device));
}

// Translation (Maxwell code to IR) and SPIR-V emission of a graphics pipeline's programs. Reads
// nothing of the pipeline key but `key`, so the shaders fit every pipeline with that key and
// environments that answer the same (vk_shader_reuse.h). Null for programs with known issues.
std::shared_ptr<ReusableShaders> PipelineCache::BuildGraphicsShaders(
    ShaderPools& pools, const ShaderTranslationKey& key,
    std::span<Shader::Environment* const> pipeline_envs, u64 pipeline_hash) {
    // With reuse on, translation asks the environments through recorders: their answers tell
    // which later pipelines these shaders fit.
    boost::container::static_vector<RecordingEnvironment, Maxwell::MaxShaderProgram> recorders;
    boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> envs;
    for (Shader::Environment* const env : pipeline_envs) {
        envs.push_back(use_shader_reuse ? &recorders.emplace_back(*env) : env);
    }

    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};

    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program,
                                                          key.passthrough_topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        if (index == static_cast<size_t>(Maxwell::ShaderType::Geometry) &&
            !device.IsGeometryShaderSupported() && maxwell3d != nullptr &&
            maxwell3d->GetNativeMacroEngine().native_turing) {
            // A native (Switch 2) geometry program on a device without geometry shaders: the
            // pipeline cannot run it, so its draws are skipped.
            static std::atomic<bool> logged{};
            if (!logged.exchange(true)) {
                LOG_WARNING(Render_Vulkan, "Skipping native geometry shader pipelines: the device has no geometry shaders");
            }
            return nullptr;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        // Without VK_EXT_mesh_shader the driver cannot build a task/mesh pipeline; the draw is
        // skipped (see the pipeline check in the rasterizer).
        if ((env.ShaderStage() == Shader::Stage::Task ||
             env.ShaderStage() == Shader::Stage::Mesh) &&
            !device.IsExtMeshShaderSupported()) {
            return nullptr;
        }

        // Control flow analysis, translation and the dual vertex merge are charged together.
        std::optional<PipelineStats::ScopedTimer> translate_timer;
        translate_timer.emplace(PipelineStats::Kind::Graphics, PipelineStats::Stage::Translate);
        PipelineStats::NoteTranslation(PipelineStats::Kind::Graphics, static_cast<unsigned>(index),
                                       key.unique_hashes[index]);
        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            // Normal path
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env);
        }
        translate_timer.reset();

        if (device.GetDriverID() != VK_DRIVER_ID_NVIDIA_PROPRIETARY && Shader::Optimization::HasBrokenPattern(programs[index])) {
            LOG_WARNING(Render_Vulkan, "0x{:016x}: Skipping shader with known issues", key.unique_hashes[index]);
            return nullptr;
        }

        if (Settings::values.dump_guest_shaders) {
            env.Dump(pipeline_hash, key.unique_hashes[index]);
        }

        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }

#ifdef _WIN32
    // AMD's Windows driver can crash when a geometry shader exports all generic locations.
    if (device.GetDriverID() == VK_DRIVER_ID_AMD_PROPRIETARY && !key.xfb_enabled) {
        constexpr size_t geometry_index = static_cast<size_t>(Maxwell::ShaderType::Geometry);
        constexpr size_t fragment_index = static_cast<size_t>(Maxwell::ShaderType::Pixel);
        if (key.unique_hashes[geometry_index] != 0 &&
            key.unique_hashes[fragment_index] != 0) {
            auto& geometry = programs[geometry_index];
            const auto& fragment = programs[fragment_index];
            size_t generic_outputs{};
            for (size_t index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
                generic_outputs += geometry.info.stores.Generic(index) ? 1 : 0;
            }
            if (generic_outputs == Shader::IR::NUM_GENERICS &&
                !geometry.info.stores.Legacy() && !fragment.info.loads.Legacy()) {
                for (size_t index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
                    if (fragment.info.loads.Generic(index)) {
                        continue;
                    }
                    for (size_t component = 0; component < 4; ++component) {
                        geometry.info.stores.Set(Shader::IR::Attribute::Generic0X + index * 4 +
                                                     component,
                                                 false);
                    }
                }
            }
        }
    }
#endif

    // Task and mesh pipelines need VK_EXT_mesh_shader, and their draws are skipped without it.
    // Don't build one anyway: MoltenVK reads the vertex input state such a pipeline leaves null.
    if (!device.IsExtMeshShaderSupported() &&
        std::ranges::any_of(programs, [](const Shader::IR::Program& program) {
            return program.info.is_task || program.info.is_mesh;
        })) {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 4) {
            LOG_WARNING(Render_Vulkan, "Skipping a task/mesh pipeline: no VK_EXT_mesh_shader");
        }
        return nullptr;
    }
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    auto modules{std::make_shared<GraphicsShaderModules>()};

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        UNIMPLEMENTED_IF(index == 0);

        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        const auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage)};
        ConvertLegacyToGeneric(program, runtime_info);
        std::vector<u32> code;
        {
            const PipelineStats::ScopedTimer emit_timer{PipelineStats::Kind::Graphics,
                                                        PipelineStats::Stage::Emit};
            code = EmitSPIRV(profile, runtime_info, program, binding);
        }
        device.SaveShader(code);
        {
            const PipelineStats::ScopedTimer module_timer{PipelineStats::Kind::Graphics,
                                                          PipelineStats::Stage::Module};
            (*modules)[stage_index] = BuildShader(device, code);
        }

        // Text log + .spv dump. Text log is gated by gpu_log_level != Off; .spv dump
        // is independent and gated only by gpu_log_shader_dumps.
        const bool should_log = GPU::Logging::IsActive();
        const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
        if (should_log || should_dump) {
            static constexpr std::array stage_names{"vertex", "tess_control", "tess_eval", "geometry", "fragment"};
            const std::string shader_name = fmt::format("shader_{:016x}_{}", key.unique_hashes[index], stage_names[stage_index]);
            if (should_log) {
                const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                    code.size() * sizeof(u32), key.unique_hashes[index]);
                GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
            }
            if (should_dump) {
                GPU::Logging::DumpSpirvShader(key.unique_hashes[index],
                                              std::span<const u32>(code.data(), code.size()));
            }
        }

        if (device.HasDebuggingToolAttached()) {
            const std::string name{fmt::format("Shader {:016x}", key.unique_hashes[index])};
            (*modules)[stage_index].SetObjectNameEXT(name.c_str());
        }
        previous_stage = &program;
    }

    auto shaders{std::make_shared<ReusableShaders>()};
    shaders->modules = std::move(modules);
    for (size_t stage = 0; stage < infos.size(); ++stage) {
        if (infos[stage]) {
            shaders->infos[stage] = *infos[stage];
        }
    }
    for (RecordingEnvironment& recorder : recorders) {
        shaders->reads.push_back(recorder.TakeReads());
    }
    return shaders;
}

namespace {
// Pipelines built on the GPU thread at run time (shader translation; the driver compile is on
// workers): every one over 2 ms is logged, and the total every 10 s, to tell how much of the
// stutter on new content is translation.
void ReportRuntimePipeline(const char* kind, std::chrono::steady_clock::time_point begin) {
    static std::chrono::steady_clock::duration total{};
    static u32 count{}, logged{};
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - begin;
    total += elapsed;
    ++count;
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    if (elapsed > std::chrono::milliseconds{2} && logged < 400) {
        ++logged;
        LOG_INFO(Render_Vulkan, "{} pipeline built on the GPU thread in {:.1f} ms", kind, ms(elapsed));
    }
    if (now - window > std::chrono::seconds{10}) {
        LOG_INFO(Render_Vulkan, "Runtime pipelines in the last 10 s: {} built, {:.1f} ms on the GPU thread",
                 count, ms(total));
        total = {};
        count = 0;
        window = now;
    }
}

// Graphics pipelines translated on worker threads: what they still cost the GPU thread
// (environments and snapshots, waiting) and the workers, logged every 10 s.
// Used on the GPU thread only.
class TranslationReport {
public:
    using Duration = std::chrono::steady_clock::duration;

    void Queued(Duration preparation, size_t snapshot_bytes) {
        ++queued;
        preparation_time += preparation;
        copied_bytes += snapshot_bytes;
        LogEvery10Seconds();
    }

    void NotDetached() {
        ++not_detached;
        LogEvery10Seconds();
    }

    void Waited(Duration wait) {
        wait_time += wait;
        LogEvery10Seconds();
    }

    void Adopted(Duration build) {
        ++adopted;
        build_time += build;
        LogEvery10Seconds();
    }

private:
    void LogEvery10Seconds() {
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds{10}) {
            return;
        }
        const auto ms = [](Duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        LOG_INFO(Render_Vulkan,
                 "Graphics pipelines translated on workers in the last 10 s: {} queued ({} KiB "
                 "of state copied), {} adopted ({:.1f} ms on workers), {} built on the GPU "
                 "thread (texture pool not contiguous); GPU thread: {:.1f} ms preparing, {:.1f} "
                 "ms waiting",
                 queued, copied_bytes / 1024, adopted, ms(build_time),
                 not_detached, ms(preparation_time), ms(wait_time));
        // Start a new window.
        *this = TranslationReport{};
    }

    u32 queued{};
    u32 not_detached{};
    u32 adopted{};
    size_t copied_bytes{};
    Duration preparation_time{};
    Duration wait_time{};
    Duration build_time{};
    std::chrono::steady_clock::time_point window{std::chrono::steady_clock::now()};
};

TranslationReport& GetTranslationReport() {
    static TranslationReport report;
    return report;
}
} // Anonymous namespace

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        ReportRuntimePipeline("Graphics", begin);
    };
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);

    main_pools.ReleaseContents();
    auto pipeline{CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), nullptr,
                                         true, true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    SerializeGraphicsPipeline(*pipeline, graphics_key, std::move(environments.envs));
    QueueVulkanPipelineCacheFlush();
    return pipeline;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipelineFromReusedShaders() {
    if (!HasReusableShaders(graphics_key)) {
        return nullptr;
    }
    const auto begin = std::chrono::steady_clock::now();
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    auto pipeline{CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), nullptr,
                                         true, true, false)};
    if (!pipeline) {
        return nullptr;
    }
    ReportRuntimePipeline("Graphics (reused shaders)", begin);
    if (!pipeline_cache_filename.empty()) {
        SerializeGraphicsPipeline(*pipeline, graphics_key, std::move(environments.envs));
        QueueVulkanPipelineCacheFlush();
    }
    return pipeline;
}

void PipelineCache::SerializeGraphicsPipeline(const GraphicsPipeline& pipeline,
                                              const GraphicsPipelineCacheKey& key,
                                              GraphicsEnvironmentArray envs) {
    if (pipeline.IsMeshPipeline()) {
        // Not stored: task and mesh programs are translated with the mesh registers, which the
        // cache format does not carry. The few of them rebuild quickly from the driver cache.
        return;
    }
    // By the identity LoadDiskResources gave the file's pipelines: a native pipeline of a stored
    // key built for other storage buffer pointers is stored too.
    if (!disk_pipelines.insert(DiskPipelineHash(key, envs)).second) {
        return; // In the disk cache already, only not built at boot.
    }
    serialization_thread.QueueWork([this, key, envs = std::move(envs)] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION);
    });
}

// Asynchronous translation of native graphics pipelines (NX2_SHADER_STUTTER.md).
//
// The GPU thread used to translate a new pipeline (Maxwell code to IR to SPIR-V) before its
// draw, ~2-4 ms each. Now it builds the environments, detaches them from the 3D engine (copies
// what translation reads, see GraphicsEnvironment::Detach) and queues the translation on
// translation_workers, then waits for the result; driver compilation runs on pipeline workers.
// The draw gets its pipeline, and environments that cannot detach use the GPU thread instead.
//
// Only native (Switch 2) programs take this path: their code was lowered when the shader cache
// met them, so translation never reads guest memory for instructions. Compute pipelines stay
// synchronous.
bool PipelineCache::CanTranslateAsynchronously() const {
    if (!use_async_translation) {
        return false;
    }
    if (!maxwell3d->GetNativeMacroEngine().native_turing) {
        return false;
    }
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (graphics_key.unique_hashes[index] == 0) {
            continue;
        }
        if (!shader_infos[index] || !shader_infos[index]->lowered_native) {
            return false;
        }
    }
    return true;
}

std::optional<std::future<PipelineCache::TranslatedGraphicsPipeline>>
PipelineCache::QueueGraphicsTranslation() {
    const auto begin = std::chrono::steady_clock::now();
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);

    size_t snapshot_bytes{};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (graphics_key.unique_hashes[index] == 0) {
            continue;
        }
        if (!environments.envs[index].Detach()) {
            GetTranslationReport().NotDetached();
            return std::nullopt;
        }
        snapshot_bytes += environments.envs[index].SnapshotSizeBytes();
    }

    std::promise<TranslatedGraphicsPipeline> promise;
    std::future<TranslatedGraphicsPipeline> result{promise.get_future()};
    translation_workers.QueueWork([this, key = graphics_key, envs = std::move(environments.envs),
                                   promise = std::move(promise)]() mutable {
        promise.set_value(TranslateGraphicsPipeline(key, std::move(envs)));
    });
    GetTranslationReport().Queued(std::chrono::steady_clock::now() - begin, snapshot_bytes);
    return result;
}

PipelineCache::TranslatedGraphicsPipeline PipelineCache::TranslateGraphicsPipeline(
    const GraphicsPipelineCacheKey& key, GraphicsEnvironmentArray envs) {
    const PipelineStats::ScopedRole stats_role{PipelineStats::Role::Xlate};
    const auto begin = std::chrono::steady_clock::now();
    ShaderPools pools;
    boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> env_ptrs;
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] != 0) {
            env_ptrs.push_back(&envs[index]);
        }
    }
    std::unique_ptr<GraphicsPipeline> pipeline;
    try {
        // The scheduler waits for driver compilation at the first draw, while the GPU thread
        // waits only for translation.
        pipeline = CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), nullptr, true, true);
    } catch (const std::exception& exception) {
        // Shader errors are handled inside; nothing else may end the worker thread.
        LOG_ERROR(Render_Vulkan, "Graphics pipeline translation failed: {}", exception.what());
    }
    return {
        .key = key,
        .pipeline = std::move(pipeline),
        .envs = std::move(envs),
        .build_time = std::chrono::steady_clock::now() - begin,
    };
}

std::unique_ptr<GraphicsPipeline> PipelineCache::AdoptTranslatedGraphicsPipeline(
    std::future<TranslatedGraphicsPipeline>& translated) {
    const auto begin = std::chrono::steady_clock::now();
    TranslatedGraphicsPipeline result{translated.get()};
    GetTranslationReport().Waited(std::chrono::steady_clock::now() - begin);
    GetTranslationReport().Adopted(result.build_time);
    // What CreateGraphicsPipeline() does after building on the GPU thread.
    if (result.pipeline && !pipeline_cache_filename.empty()) {
        SerializeGraphicsPipeline(*result.pipeline, result.key, std::move(result.envs));
        QueueVulkanPipelineCacheFlush();
    }
    return std::move(result.pipeline);
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    const ComputePipelineCacheKey& key, const ShaderInfo* shader) {
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        ReportRuntimePipeline("Compute", begin);
    };
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start,
                           shader->lowered_native, profile.support_cooperative_matrix};
    env.SetCachedSize(shader->size_bytes);

    main_pools.ReleaseContents();
    auto pipeline{CreateComputePipeline(main_pools, key, env, nullptr, true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION);
    });
    QueueVulkanPipelineCacheFlush();
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel) try {
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping {:#016x}", hash);
        return nullptr;
    }

    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);
    PipelineStats::NotePipeline(PipelineStats::Kind::Compute, key.unique_hash);

    std::optional<PipelineStats::ScopedTimer> translate_timer;
    translate_timer.emplace(PipelineStats::Kind::Compute, PipelineStats::Stage::Translate);
    PipelineStats::NoteTranslation(PipelineStats::Kind::Compute, 5, key.unique_hash);
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_guest_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    translate_timer.reset();
    const VkDriverIdKHR driver_id = device.GetDriverID();
    const bool needs_shared_mem_clamp =
        driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
        driver_id == VK_DRIVER_ID_ARM_PROPRIETARY || driver_id == VK_DRIVER_ID_MOLTENVK;
    const u32 max_shared_memory = device.GetMaxComputeSharedMemorySize();
    if (needs_shared_mem_clamp && program.shared_memory_size > max_shared_memory) {
        LOG_WARNING(Render_Vulkan,
                    "Compute shader {:#016x} requests {}KB shared memory but device max is {}KB - clamping",
                    key.unique_hash,
                    program.shared_memory_size / 1024,
                    max_shared_memory / 1024);
        program.shared_memory_size = max_shared_memory;
    }
    std::vector<u32> code;
    {
        const PipelineStats::ScopedTimer emit_timer{PipelineStats::Kind::Compute,
                                                    PipelineStats::Stage::Emit};
        code = EmitSPIRV(profile, program);
    }

    // Text log + .spv dump. Done before pipeline build so a driver crash still leaves the module.
    {
        const bool should_log = GPU::Logging::IsActive();
        const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
        if (should_log || should_dump) {
            const std::string shader_name = fmt::format("shader_{:016x}_compute", key.unique_hash);
            if (should_log) {
                const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                    code.size() * sizeof(u32), key.unique_hash);
                GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
            }
            if (should_dump) {
                GPU::Logging::DumpSpirvShader(key.unique_hash,
                                              std::span<const u32>(code.data(), code.size()));
            }
        }
    }

    device.SaveShader(code);
    vk::ShaderModule spv_module;
    {
        const PipelineStats::ScopedTimer module_timer{PipelineStats::Kind::Compute,
                                                      PipelineStats::Stage::Module};
        spv_module = BuildShader(device, code);
    }

    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<ComputePipeline>(device, scheduler, vulkan_pipeline_cache, descriptor_pool,
                                             guest_descriptor_queue, descriptor_buffer_ring,
                                             thread_worker, statistics,
                                             &shader_notify, program.info, std::move(spv_module),
                                             key.unique_hash);

} catch (const Shader::TensorRejected& exception) {
    if (build_in_parallel) {
        throw;
    }
    LOG_WARNING(Render_Vulkan, "Skipping cached tensor shader: {}", exception.what());
    return nullptr;
} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    // Written beside the cache and renamed over it: the emulator stopped mid-write left the cache
    // empty, and every later boot failed to read it.
    auto temporary = filename;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    size_t cache_size = 0;
    std::vector<char> cache_data;
    if (pipeline_cache) {
        pipeline_cache.Read(&cache_size, nullptr);
        cache_data.resize(cache_size);
        pipeline_cache.Read(&cache_size, cache_data.data());
    }
    file.write(cache_data.data(), cache_size);
    file.close();
    std::error_code error;
    std::filesystem::rename(temporary, filename, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to replace Vulkan driver pipeline cache file {}: {}",
                  Common::FS::PathToUTF8String(filename), error.message());
        std::filesystem::remove(temporary, error);
        return;
    }

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));

} catch (const std::ios_base::failure& e) {
    // The cache itself is untouched; only the partly written copy goes.
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    auto temporary = filename;
    temporary += ".tmp";
    std::error_code error;
    std::filesystem::remove(temporary, error);
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         u32 expected_cache_version) {
    const auto create_pipeline_cache = [this](size_t data_size, const void* data) {
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        return device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
    };
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return create_pipeline_cache(0, nullptr);
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);

        std::array<char, 8> magic_number;
        u32 cache_version;
        // A file cut short (the emulator stopped while writing it) would throw from the reads
        // below, and macOS's libc++ throws an ios_base::failure the handler doesn't match.
        if (static_cast<size_t>(end) < magic_number.size() + sizeof(cache_version)) {
            file.close();
            LOG_ERROR(Common_Filesystem, "Truncated Vulkan driver pipeline cache file, deleting it");
            Common::FS::RemoveFile(filename);
            return create_pipeline_cache(0, nullptr);
        }
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != VULKAN_CACHE_MAGIC_NUMBER || cache_version != expected_cache_version) {
            file.close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != VULKAN_CACHE_MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem, "Invalid Vulkan driver pipeline cache file");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old Vulkan driver pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid Vulkan pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return create_pipeline_cache(0, nullptr);
        }

        static constexpr size_t header_size = magic_number.size() + sizeof(cache_version);
        const size_t cache_size = static_cast<size_t>(end) - header_size;
        std::vector<char> cache_data(cache_size);
        file.read(cache_data.data(), cache_size);

        LOG_INFO(Render_Vulkan,
                 "Loaded Vulkan driver pipeline cache: ", Common::FS::PathToUTF8String(filename));

        return create_pipeline_cache(cache_size, cache_data.data());

    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        if (!Common::FS::RemoveFile(filename)) {
            LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                      Common::FS::PathToUTF8String(filename));
        }

        return create_pipeline_cache(0, nullptr);
    }
}

} // namespace Vulkan
