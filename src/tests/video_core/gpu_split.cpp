// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"

namespace {

using Tegra::Engines::KeplerCompute;
using Tegra::Engines::Maxwell3D;

// A Turing macro instruction that writes the next parameter to `method` (the encoding of
// native_macro_fifo.cpp). After an instruction with `end`, one more runs.
std::array<u32, 3> EmitParameter(u32 method, bool end) {
    std::array<u32, 3> bits{};
    const auto field = [&](u32 offset, u32 count, u32 value) {
        for (u32 bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
        }
    };
    field(0, 1, end);
    for (u32 lane = 0; lane < 2; ++lane) {
        field(15 + lane * 36, 5, 24);
        field(20 + lane * 36, 5, 24);
        field(25 + lane * 36, 5, 24);
    }
    field(30, 16, method);
    field(82, 3, 5);
    field(85, 4, 3);
    return {bits[2], bits[1], bits[0]};
}

// The words of a macro that writes its two parameters to `first` and `first + 1`.
std::array<u32, 6> TwoWrites(u32 first) {
    const auto a = EmitParameter(first, true);
    const auto b = EmitParameter(first + 1, false);
    return {a[0], a[1], a[2], b[0], b[1], b[2]};
}

constexpr u32 MacroCall = 0xe00;
constexpr u32 Scratch = static_cast<u32>(MAXWELL3D_REG_INDEX(shadow_scratch));
constexpr u32 CodePointer = static_cast<u32>(MAXWELL3D_REG_INDEX(load_mme.instruction_ptr));
constexpr u32 Code = static_cast<u32>(MAXWELL3D_REG_INDEX(load_mme.instruction));
constexpr u32 BindPointer = static_cast<u32>(MAXWELL3D_REG_INDEX(load_mme.start_address_ptr));
constexpr u32 Bind = static_cast<u32>(MAXWELL3D_REG_INDEX(load_mme.start_address));
// The compute class's macro upload and bind methods (KeplerCompute::ProcessNativeMethod).
constexpr u32 ComputeCodePointer = 0x45;
constexpr u32 ComputeCode = 0x46;
constexpr u32 ComputeBindPointer = 0x47;
constexpr u32 ComputeBind = 0x48;

} // Anonymous namespace

// The split's front runs the 3D and compute macros and loads and binds their code
// (EDEN_NX2_SPLIT=2). When it stops mid-game (DMA accuracy set to Safe, or an EDEN_NX2_AB=split B
// window), the engines run on one thread again: a macro call cut by the stop keeps its first
// parameters, macro uploads and binds take effect, and the next start copies the registers afresh.
TEST_CASE("GPU thread split hands the engines' macro state back when it stops",
          "[video_core][gpu_split]") {
    Core::System system;
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager device_memory_manager{device_memory};
    Tegra::MemoryManager memory_manager{system, device_memory_manager, 32, u64{1} << 32};
    const auto macros = std::make_shared<Tegra::NativeMacroContext>();
    macros->engine.native_turing = true;
    macros->compute_engine.native_turing = true;
    Maxwell3D maxwell3d{memory_manager};
    maxwell3d.SetNativeMacroContext(macros);
    maxwell3d.SetNativeTuringMacroMode(true);
    KeplerCompute compute{memory_manager};
    compute.SetNativeMacroMode(&macros->engine, macros->positions, macros.get());
    compute.SetNativeDataRam(&maxwell3d.GetNativeDataRam());
    // DmaPusher sets it before each call; no command list here.
    maxwell3d.current_dma_segment = 0;
    compute.current_dma_segment = 0;

    auto& split = VideoCommon::GpuSplit::Get();
    const bool was_switch2 = Tegra::g_nx2_ampere.exchange(true);
    const auto dma_accuracy = Settings::values.dma_accuracy.GetValue();
    const auto backend = Settings::values.renderer_backend.GetValue();
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    SCOPE_EXIT {
        split.Stop();
        Settings::values.dma_accuracy.SetValue(dma_accuracy);
        Settings::values.renderer_backend.SetValue(backend);
        Tegra::g_nx2_ampere.store(was_switch2);
    };
    const auto run_split = [&](bool on) {
        Settings::values.dma_accuracy.SetValue(on ? Settings::DmaAccuracy::Default
                                                  : Settings::DmaAccuracy::Safe);
        split.MaybeStart(system);
        REQUIRE(split.Active() == on);
    };

    Settings::values.dma_accuracy.SetValue(Settings::DmaAccuracy::Default);
    split.MaybeStart(system);
    if (!split.FrontMacros()) {
        SKIP("The split runs macros on its front only with the x86-64 macro JIT and "
             "EDEN_NX2_SPLIT=2");
    }
    // Macro 0 of each engine, loaded and bound by the front: 3D writes scratch 1 and 2, compute
    // its registers 0x200 and 0x201. Then a state write, and one call of each macro whose first
    // parameter comes before the stop and its last after.
    maxwell3d.FrontCallMethod(system, split, CodePointer, 0, true);
    for (const u32 word : TwoWrites(Scratch + 1)) {
        maxwell3d.FrontCallMethod(system, split, Code, word, true);
    }
    maxwell3d.FrontCallMethod(system, split, BindPointer, 0, true);
    maxwell3d.FrontCallMethod(system, split, Bind, 0, true);
    maxwell3d.FrontCallMethod(system, split, Scratch + 5, 5, true);
    maxwell3d.FrontCallMethod(system, split, MacroCall, 0x1111, false);
    compute.FrontCallMethod(system, split, ComputeCodePointer, 0, true, nullptr);
    for (const u32 word : TwoWrites(0x200)) {
        compute.FrontCallMethod(system, split, ComputeCode, word, true, nullptr);
    }
    compute.FrontCallMethod(system, split, ComputeBindPointer, 0, true, nullptr);
    compute.FrontCallMethod(system, split, ComputeBind, 0, true, nullptr);
    compute.FrontCallMethod(system, split, MacroCall, 0x3333, false, nullptr);

    run_split(false);
    // The cut calls finish on this thread with both parameters.
    maxwell3d.CallMethod(system, MacroCall + 1, 0x2222, true);
    CHECK(maxwell3d.GetRegisterValue(Scratch + 1) == 0x1111);
    CHECK(maxwell3d.GetRegisterValue(Scratch + 2) == 0x2222);
    compute.CallMethod(system, MacroCall + 1, 0x4444, true);
    CHECK(compute.regs.reg_array[0x200] == 0x3333);
    CHECK(compute.regs.reg_array[0x201] == 0x4444);

    // Macro 1 of each engine, loaded at instruction 0x10 and bound while the split is off.
    maxwell3d.CallMethod(system, CodePointer, 0x10, true);
    for (const u32 word : TwoWrites(Scratch + 3)) {
        maxwell3d.CallMethod(system, Code, word, true);
    }
    maxwell3d.CallMethod(system, BindPointer, 1, true);
    maxwell3d.CallMethod(system, Bind, 0x10, true);
    maxwell3d.CallMethod(system, MacroCall + 2, 0x5555, false);
    maxwell3d.CallMethod(system, MacroCall + 3, 0x6666, true);
    CHECK(maxwell3d.GetRegisterValue(Scratch + 3) == 0x5555);
    CHECK(maxwell3d.GetRegisterValue(Scratch + 4) == 0x6666);
    compute.CallMethod(system, ComputeCodePointer, 0x10, true);
    for (const u32 word : TwoWrites(0x202)) {
        compute.CallMethod(system, ComputeCode, word, true);
    }
    compute.CallMethod(system, ComputeBindPointer, 1, true);
    compute.CallMethod(system, ComputeBind, 0x10, true);
    compute.CallMethod(system, MacroCall + 2, 0x7777, false);
    compute.CallMethod(system, MacroCall + 3, 0x8888, true);
    CHECK(compute.regs.reg_array[0x202] == 0x7777);
    CHECK(compute.regs.reg_array[0x203] == 0x8888);

    // Scratch 5 changes while the split is off. After a restart the front must not still hold
    // the 5 it sent before the stop, or it drops the write of 5 below as unchanged.
    maxwell3d.ConsumeSink(system);
    maxwell3d.CallMethod(system, Scratch + 5, 7, true);
    run_split(true);
    maxwell3d.FrontCallMethod(system, split, Scratch + 5, 5, true);
    run_split(false);
    maxwell3d.ConsumeSink(system);
    CHECK(maxwell3d.GetRegisterValue(Scratch + 5) == 5);
}
