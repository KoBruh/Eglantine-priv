// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/engines/maxwell_3d.h"

/// EDEN_NX2_DRAW_CENSUS=1: every 300 presents, log what the GPU thread's draws look like per
/// frame: how many there are, which render passes and shaders they go to, and how many repeat
/// the previous draw's state (pipeline, render targets, textures, descriptors, vertex and index
/// buffers). Draws that repeat all of it are the ones a merge, or a skip of the per-draw state
/// work, could take. GPU back thread only.
namespace Vulkan::Nx2DrawCensus {

[[nodiscard]] inline bool Enabled() noexcept {
    static constinit std::atomic<int> state{-1};
    const int known = state.load(std::memory_order_relaxed);
    if (known >= 0) [[likely]] {
        return known != 0;
    }
    const char* const value = std::getenv("EDEN_NX2_DRAW_CENSUS");
    const bool enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
    state.store(enabled ? 1 : 0, std::memory_order_relaxed);
    return enabled;
}

struct PassKey {
    u64 color{};
    u64 zeta{};
    u32 width{};
    u32 height{};
    u32 colors{};

    bool operator==(const PassKey&) const = default;
};

struct PassKeyHash {
    size_t operator()(const PassKey& key) const noexcept {
        u64 h = key.color * 0x9E3779B97F4A7C15ULL;
        h ^= key.zeta + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2);
        h ^= (u64{key.width} << 32 | key.height) + (h << 6) + (h >> 2);
        h ^= key.colors;
        return static_cast<size_t>(h);
    }
};

struct PairHash {
    size_t operator()(const std::pair<u64, u64>& key) const noexcept {
        return static_cast<size_t>(key.first * 0x9E3779B97F4A7C15ULL ^ key.second);
    }
};

/// A viewport rectangle in pixels (x, y, width, height) and the scissor's (min_x, min_y, max_x,
/// max_y; zero when the test is off).
struct RectKey {
    s32 x{};
    s32 y{};
    s32 width{};
    s32 height{};
    u32 scissor{};
    u32 scissor_max{};

    bool operator==(const RectKey&) const = default;
};

struct RectKeyHash {
    size_t operator()(const RectKey& key) const noexcept {
        u64 h = static_cast<u64>(static_cast<u32>(key.x)) << 32 | static_cast<u32>(key.y);
        h ^= (static_cast<u64>(static_cast<u32>(key.width)) << 32 | static_cast<u32>(key.height)) *
             0x9E3779B97F4A7C15ULL;
        h ^= (u64{key.scissor} << 32 | key.scissor_max) * 0xC2B2AE3D27D4EB4FULL;
        return static_cast<size_t>(h);
    }
};

struct PassStats {
    u64 draws{};
    u64 mergeable{};
    bool depth_only{};
};

struct State {
    // Notes on the draw being configured.
    const void* pipeline{};
    bool payload_same{};
    bool handles_same{};
    bool vertex_dirty{};

    // The previous draw.
    const void* last_pipeline{};
    PassKey last_pass{};
    u64 last_index_address{~u64{0}};
    u32 last_index_format{~u32{0}};
    const void* last_payload_pipeline{};
    std::vector<u8> last_payload;
    const void* last_handles_pipeline{};
    std::vector<u32> last_handles;

    // Totals since window_start.
    u64 window_start{~u64{0}};
    u64 draws{};
    u64 indirect{};
    u64 empty{};
    u64 instanced{};
    u64 instances{};
    u64 vertices{};
    u64 depth_only{};
    u64 same_pipeline{};
    u64 same_pass{};
    u64 same_handles{};
    u64 same_payload{};
    u64 vertex_changed{};
    u64 index_changed{};
    u64 mergeable{};
    u64 runs{};
    u64 current_run{};
    u64 longest_run{};
    std::unordered_map<PassKey, PassStats, PassKeyHash> passes;
    std::unordered_map<std::pair<u64, u64>, u64, PairHash> shaders;
    // Depth-only square passes of 1024 pixels or more (shadow maps): draws by viewport and
    // scissor, by shader pair, and their vertices and instances.
    u64 shadow_draws{};
    u64 shadow_vertices{};
    u64 shadow_instances{};
    std::unordered_map<RectKey, u64, RectKeyHash> shadow_rects;
    std::unordered_map<std::pair<u64, u64>, u64, PairHash> shadow_shaders;
};

[[nodiscard]] inline State& Get() {
    static State state;
    return state;
}

/// Before the pipeline configures: whether the vertex buffer bindings (addresses, strides,
/// limits) changed. The index buffer is compared in RecordDraw: its dirty flag also covers the
/// draw's first and count, which a merged draw can differ in.
inline void NoteVertexDirty(bool vertex_dirty) {
    Get().vertex_dirty = vertex_dirty;
}

/// The texture and sampler handles the draw's shaders read (before they are resolved to views).
inline void NoteHandles(const void* pipeline, const u32* handles, size_t count) {
    State& s = Get();
    s.handles_same = s.last_handles_pipeline == pipeline && s.last_handles.size() == count &&
                     (count == 0 ||
                      std::memcmp(s.last_handles.data(), handles, count * sizeof(u32)) == 0);
    s.last_handles_pipeline = pipeline;
    s.last_handles.assign(handles, handles + count);
}

/// The draw's descriptor payload (images, samplers, uniform and storage buffers it binds).
inline void NotePayload(const void* pipeline, const void* data, size_t bytes) {
    State& s = Get();
    s.pipeline = pipeline;
    const u8* const begin = static_cast<const u8*>(data);
    s.payload_same = s.last_payload_pipeline == pipeline && s.last_payload.size() == bytes &&
                     (bytes == 0 || std::memcmp(s.last_payload.data(), begin, bytes) == 0);
    s.last_payload_pipeline = pipeline;
    s.last_payload.assign(begin, begin + bytes);
}

inline void Report(State& s, u64 frames) {
    const double f = static_cast<double>(frames);
    const auto pct = [&](u64 part) {
        return s.draws ? 100.0 * static_cast<double>(part) / static_cast<double>(s.draws) : 0.0;
    };
    LOG_INFO(HW_GPU,
             "Draw census over {} frames: {:.0f} draws/frame ({:.0f} indirect, {:.0f} empty), "
             "{:.0f} render pass changes/frame; instanced {:.1f}% (avg {:.1f} instances), "
             "avg {:.0f} vertices; depth-only {:.1f}%",
             frames, static_cast<double>(s.draws) / f, static_cast<double>(s.indirect) / f,
             static_cast<double>(s.empty) / f,
             static_cast<double>(s.draws - s.same_pass) / f, pct(s.instanced),
             s.instanced ? static_cast<double>(s.instances) / static_cast<double>(s.instanced) : 0.0,
             s.draws ? static_cast<double>(s.vertices) / static_cast<double>(s.draws) : 0.0,
             pct(s.depth_only));
    LOG_INFO(HW_GPU,
             "Draw census, same as the previous draw: pipeline {:.1f}%, render pass {:.1f}%, "
             "texture handles {:.1f}%, descriptors {:.1f}%; vertex buffers changed {:.1f}%, "
             "index buffer changed {:.1f}%; all of it (mergeable) {:.1f}% in {:.0f} runs/frame, "
             "longest {}",
             pct(s.same_pipeline), pct(s.same_pass), pct(s.same_handles), pct(s.same_payload),
             pct(s.vertex_changed), pct(s.index_changed), pct(s.mergeable),
             static_cast<double>(s.runs) / f, s.longest_run);

    std::vector<std::pair<PassKey, PassStats>> passes(s.passes.begin(), s.passes.end());
    std::sort(passes.begin(), passes.end(),
              [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });
    for (size_t i = 0; i < passes.size() && i < 12; ++i) {
        const auto& [key, stats] = passes[i];
        LOG_INFO(HW_GPU,
                 "  pass color {:#x} zeta {:#x} {}x{} colors {}{}: {:.0f} draws/frame, "
                 "{:.1f}% mergeable",
                 key.color, key.zeta, key.width, key.height, key.colors,
                 stats.depth_only ? " (depth-only)" : "", static_cast<double>(stats.draws) / f,
                 stats.draws ? 100.0 * static_cast<double>(stats.mergeable) /
                                   static_cast<double>(stats.draws)
                             : 0.0);
    }
    std::vector<std::pair<std::pair<u64, u64>, u64>> shaders(s.shaders.begin(), s.shaders.end());
    std::sort(shaders.begin(), shaders.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < shaders.size() && i < 12; ++i) {
        LOG_INFO(HW_GPU, "  shaders vs {:#x} ps {:#x}: {:.0f} draws/frame", shaders[i].first.first,
                 shaders[i].first.second, static_cast<double>(shaders[i].second) / f);
    }
    LOG_INFO(HW_GPU, "  {} render passes, {} shader pairs seen", passes.size(), shaders.size());
    if (s.shadow_draws == 0) {
        return;
    }
    LOG_INFO(HW_GPU,
             "Draw census, shadow maps: {:.0f} draws/frame, avg {:.0f} vertices, avg {:.2f} "
             "instances; {} viewport/scissor rectangles, {} shader pairs",
             static_cast<double>(s.shadow_draws) / f,
             static_cast<double>(s.shadow_vertices) / static_cast<double>(s.shadow_draws),
             static_cast<double>(s.shadow_instances) / static_cast<double>(s.shadow_draws),
             s.shadow_rects.size(), s.shadow_shaders.size());
    std::vector<std::pair<RectKey, u64>> rects(s.shadow_rects.begin(), s.shadow_rects.end());
    std::sort(rects.begin(), rects.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < rects.size() && i < 12; ++i) {
        const RectKey& r = rects[i].first;
        LOG_INFO(HW_GPU,
                 "  shadow viewport {},{} {}x{} scissor {:#010x}..{:#010x}: {:.0f} draws/frame",
                 r.x, r.y, r.width, r.height, r.scissor, r.scissor_max,
                 static_cast<double>(rects[i].second) / f);
    }
    std::vector<std::pair<std::pair<u64, u64>, u64>> shadow_shaders(s.shadow_shaders.begin(),
                                                                    s.shadow_shaders.end());
    std::sort(shadow_shaders.begin(), shadow_shaders.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < shadow_shaders.size() && i < 10; ++i) {
        LOG_INFO(HW_GPU, "  shadow shaders vs {:#x} ps {:#x}: {:.0f} draws/frame",
                 shadow_shaders[i].first.first, shadow_shaders[i].first.second,
                 static_cast<double>(shadow_shaders[i].second) / f);
    }
}

/// After the pipeline configured, as the draw is recorded.
inline void RecordDraw(const Tegra::Engines::Maxwell3D::Regs& regs, u32 num_vertices,
                       u32 num_instances, bool indirect, u64 present) {
    State& s = Get();
    if (s.window_start == ~u64{0}) {
        s.window_start = present;
    }

    const u32 colors = regs.rt_control.count.Value();
    bool color_written = false;
    for (u32 i = 0; i < colors && i < 8; ++i) {
        color_written |= regs.color_mask[i].raw != 0;
    }
    const bool depth_only = colors == 0 || !color_written;
    PassKey pass{};
    pass.colors = colors;
    pass.zeta = regs.zeta_enable != 0 ? regs.zeta.Address() : 0;
    if (colors != 0) {
        const auto& rt = regs.rt[regs.rt_control.target0.Value()];
        pass.color = rt.Address();
        pass.width = rt.width;
        pass.height = rt.height;
    } else {
        pass.width = regs.zeta_size.width;
        pass.height = regs.zeta_size.height;
    }

    const u64 index_address = regs.index_buffer.StartAddress();
    const u32 index_format = static_cast<u32>(regs.index_buffer.format);
    const bool index_changed =
        index_address != s.last_index_address || index_format != s.last_index_format;
    s.last_index_address = index_address;
    s.last_index_format = index_format;
    const bool same_pipeline = s.pipeline == s.last_pipeline;
    const bool same_pass = pass == s.last_pass;
    const bool mergeable = !indirect && same_pipeline && same_pass && s.payload_same &&
                           s.handles_same && !s.vertex_dirty && !index_changed;
    ++s.draws;
    s.indirect += indirect ? 1 : 0;
    s.empty += !indirect && (num_vertices == 0 || num_instances == 0) ? 1 : 0;
    if (num_instances > 1) {
        ++s.instanced;
        s.instances += num_instances;
    }
    s.vertices += num_vertices;
    s.depth_only += depth_only ? 1 : 0;
    s.same_pipeline += same_pipeline ? 1 : 0;
    s.same_pass += same_pass ? 1 : 0;
    s.same_handles += s.handles_same ? 1 : 0;
    s.same_payload += s.payload_same ? 1 : 0;
    s.vertex_changed += s.vertex_dirty ? 1 : 0;
    s.index_changed += index_changed ? 1 : 0;
    if (mergeable) {
        ++s.mergeable;
        if (s.current_run++ == 0) {
            ++s.runs;
        }
        s.longest_run = (std::max)(s.longest_run, s.current_run + 1);
    } else {
        s.current_run = 0;
    }
    PassStats& stats = s.passes[pass];
    ++stats.draws;
    stats.mergeable += mergeable ? 1 : 0;
    stats.depth_only = depth_only;
    const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
    const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
    ++s.shaders[{vs, ps}];
    if (depth_only && pass.width == pass.height && pass.width >= 1024) {
        const auto& vt = regs.viewport_transform[0];
        const float half_w = vt.scale_x < 0 ? -vt.scale_x : vt.scale_x;
        const float half_h = vt.scale_y < 0 ? -vt.scale_y : vt.scale_y;
        const auto& sc = regs.scissor_test[0];
        RectKey rect{
            .x = static_cast<s32>(vt.translate_x - half_w),
            .y = static_cast<s32>(vt.translate_y - half_h),
            .width = static_cast<s32>(half_w * 2.0f),
            .height = static_cast<s32>(half_h * 2.0f),
            .scissor = sc.enable != 0 ? (u32{sc.min_x.Value()} | u32{sc.min_y.Value()} << 16) : 0,
            .scissor_max =
                sc.enable != 0 ? (u32{sc.max_x.Value()} | u32{sc.max_y.Value()} << 16) : 0,
        };
        ++s.shadow_draws;
        s.shadow_vertices += num_vertices;
        s.shadow_instances += num_instances;
        ++s.shadow_rects[rect];
        ++s.shadow_shaders[{vs, ps}];
    }

    s.last_pipeline = s.pipeline;
    s.last_pass = pass;
    s.pipeline = nullptr;
    s.payload_same = false;
    s.handles_same = false;

    const u64 frames = present - s.window_start;
    if (frames >= 300) {
        Report(s, frames);
        const State keep_last = [&] {
            State next;
            next.last_pipeline = s.last_pipeline;
            next.last_pass = s.last_pass;
            next.last_index_address = s.last_index_address;
            next.last_index_format = s.last_index_format;
            next.last_payload_pipeline = s.last_payload_pipeline;
            next.last_payload = std::move(s.last_payload);
            next.last_handles_pipeline = s.last_handles_pipeline;
            next.last_handles = std::move(s.last_handles);
            next.window_start = present;
            return next;
        }();
        s = keep_last;
    }
}

} // namespace Vulkan::Nx2DrawCensus
