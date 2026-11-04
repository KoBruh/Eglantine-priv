// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>

#include "common/logging.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_devices.h"
#include "hid_core/hid_core.h"
#include "hid_core/resources/mouse/nmouse.h"

namespace Service::HID {

Nmouse::Nmouse(Core::HID::HIDCore& hid_core_) : ControllerBase{hid_core_} {
    emulated_devices = hid_core.GetEmulatedDevices();
}

Nmouse::~Nmouse() {
    hid_core.SetNmouseInUse(false);
}

void Nmouse::OnInit() {}
void Nmouse::OnRelease() {}

void Nmouse::Initialize(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* process,
                        s32 width, s32 height) {
    std::scoped_lock lock{mutex};
    if (size < MinimumSize) {
        LOG_ERROR(Service_HID, "Nmouse transfer memory too small: {:#x}", size);
        return;
    }
    transfer_memory = t_mem;
    owner = process;
    range_width = width > 0 ? width : 1920;
    range_height = height > 0 ? height : 1080;
    has_last_position = false;
    has_moved = false;
    lifos = {};
    WriteLayout();
    hid_core.SetNmouseInUse(true);
}

void Nmouse::Finalize() {
    std::scoped_lock lock{mutex};
    transfer_memory = {};
    owner = nullptr;
    hid_core.SetNmouseInUse(false);
}

void Nmouse::WriteLayout() {
    auto& memory = owner->GetMemory();
    for (std::size_t i = 0; i < Slots.size(); ++i) {
        const HandleEntry entry{.handle = 0x4e4d0000 + i, .is_valid = 1};
        memory.WriteBlock(transfer_memory + i * sizeof(HandleEntry), &entry, sizeof(entry));

        NmouseInfo info{};
        info.type_set = Slots[i].type_set;
        info.is_type_set_valid = 1;
        info.npad_id = 0; // Player 1
        info.is_npad_id_valid = Slots[i].has_npad ? 1 : 0;
        info.is_pointer_enabled = 1;
        memory.WriteBlock(transfer_memory + InfoOffset + i * InfoStride, &info, sizeof(info));

        memory.WriteBlock(transfer_memory + LifoOffset + i * LifoStride, &lifos[i],
                          sizeof(lifos[i]));
    }
    memory.Write8(transfer_memory + ShowCursorOffset, 1);
}

void Nmouse::OnUpdate(const Core::Timing::CoreTiming& core_timing) {
    std::scoped_lock lock{mutex};
    if (owner == nullptr) {
        return;
    }

    // The host mouse over the game's window, in the pointer range the application asked for.
    const auto position = emulated_devices->GetMousePosition();
    const s32 x = static_cast<s32>(std::lround(position.x * static_cast<f32>(range_width)));
    const s32 y = static_cast<s32>(std::lround(position.y * static_cast<f32>(range_height)));
    const s32 delta_x = has_last_position ? x - last_x : 0;
    const s32 delta_y = has_last_position ? y - last_y : 0;
    last_x = x;
    last_y = y;
    has_last_position = true;

    const s64 now_ns = core_timing.GetGlobalTimeNs().count();
    if (delta_x != 0 || delta_y != 0) {
        last_moved_ns = now_ns;
        has_moved = true;
    }
    const bool on_table = has_moved && now_ns - last_moved_ns < LiftAfterNs;

    auto& memory = owner->GetMemory();
    for (std::size_t i = 0; i < Slots.size(); ++i) {
        NmouseDevState state{};
        state.sampling_number = lifos[i].ReadCurrentEntry().state.sampling_number + 1;
        if (Slots[i].moves) {
            state.x = x;
            state.y = y;
            state.delta_x = delta_x;
            state.delta_y = delta_y;
            state.attribute = AttributeConnected | (on_table ? AttributeActive : 0);
        }
        lifos[i].timestamp = now_ns;
        lifos[i].WriteNextEntry(state);
        memory.WriteBlock(transfer_memory + LifoOffset + i * LifoStride, &lifos[i],
                          sizeof(lifos[i]));
    }
}

} // namespace Service::HID
