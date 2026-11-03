# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/event.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/host_memory_fallback.cpp
    common/page_table.cpp
    common/param_package.cpp
    common/path_defaults.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    common/game_presets.cpp
    core/core_timing.cpp
    core/deconstructed_rom_directory.cpp
    core/dynarmic_code_memory.cpp
    core/nx2_gpu_memory.cpp
    core/nrr.cpp
    core/page_heap.cpp
    core/read_traps.cpp
    core/nx2_readback_cache.cpp
    core/nx2_core_readback_capacity.cpp
    core/nso.cpp
    core/ncz_decompressor.cpp
    core/romfs_build_memory.cpp
    core/vfs_real_cache.cpp
    core/nup_writer.cpp
    core/internal_network/network.cpp
    shader_recompiler/compilation_memory.cpp
    video_core/memory_tracker.cpp
    video_core/ampere_conversions.cpp
    video_core/ampere_half.cpp
    video_core/cache_entry.cpp
    video_core/staging_buffer_policy.cpp
    video_core/transcode_budget_policy.cpp
    video_core/memory_budget.cpp
    video_core/astc_recompression_override.cpp
    video_core/nx2_video_configuration.cpp
    video_core/nx2_decoder_image.cpp
    video_core/nx2_present.cpp
    video_core/nx2_gpu_mappings.cpp
    video_core/nx2_sparse_mappings.cpp
    video_core/nx2_sparse_region.cpp
    video_core/nx2_command_sync.cpp
    video_core/nx2_command_ranges.cpp
    video_core/physical_alias.cpp
    video_core/vertex_repack.cpp
    video_core/ampere_returns.cpp
    video_core/guardband.cpp
    video_core/ampere_gather.cpp
    video_core/ampere_sample.cpp
    video_core/ampere_geometry.cpp
    video_core/native_clip.cpp
    video_core/native_qmd.cpp
    video_core/ampere_tensor.cpp
    video_core/native_video.cpp
    video_core/ampere_f2i.cpp
    video_core/compute_storage_buffers.cpp
    video_core/cooperative_matrix.cpp
    video_core/ampere_kirby.cpp
    video_core/ampere_geometry_lowering.cpp
    video_core/ampere_geometry_exit.cpp
    video_core/ampere_uniform_lea.cpp
    video_core/ampere_integer_multiply.cpp
    video_core/ampere_program_override.cpp
    video_core/x8d24_conversion.cpp
    video_core/ampere_bindless_tex.cpp
    video_core/ampere_sured.cpp
    video_core/ampere_release_guards.cpp
    video_core/ampere_release_f2i.cpp
    video_core/usage_tracker.cpp
    video_core/sparse_tiles.cpp
    video_core/descriptor_table.cpp
    video_core/shader_invalidation.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

if (WIN32)
    target_sources(tests PRIVATE common/host_memory_commit.cpp)
endif()

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)
if (TARGET dynarmic)
    target_link_libraries(tests PRIVATE dynarmic)
endif()

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
