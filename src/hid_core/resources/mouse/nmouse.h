// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <mutex>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/typed_address.h"
#include "hid_core/resources/controller_base.h"
#include "hid_core/resources/ring_lifo.h"

namespace Kernel {
class KProcess;
}

namespace Core::HID {
class EmulatedDevices;
}

namespace Service::HID {

// The Switch 2 mouse (nn::hid::InitializeNmouse, hid 3139): the application hands hid a transfer
// memory and reads the mice from it, Joy-Con 2 mouse sensors included. Fed here from the host
// mouse, as player 1's Joy-Con mice (and the merged mouse).
class Nmouse final : public ControllerBase {
public:
    explicit Nmouse(Core::HID::HIDCore& hid_core_);
    ~Nmouse() override;

    void OnInit() override;
    void OnRelease() override;
    void OnUpdate(const Core::Timing::CoreTiming& core_timing) override;

    void Initialize(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* owner, s32 width,
                    s32 height);
    void Finalize();

    // NmouseType bits as the application asks for them (GetNmouseHandles, GetMergedNmouseHandle).
    static constexpr u32 TypeMerged = 1u << 0;
    static constexpr u32 TypeJoyLeftJcms = 1u << 1;
    static constexpr u32 TypeJoyRightJcms = 1u << 2;

private:
    // The layout the SDK's NmouseTransferMemoryAccessor reads.
    static constexpr std::size_t MaxHandles = 17;
    static constexpr std::size_t LifoOffset = 0x220;
    static constexpr std::size_t LifoStride = 0x818;
    static constexpr std::size_t InfoOffset = 0x8c40;
    static constexpr std::size_t InfoStride = 0x1c;
    static constexpr std::size_t ShowCursorOffset = 0x8e68;
    static constexpr std::size_t MinimumSize = ShowCursorOffset + 1;

    struct HandleEntry {
        u64 handle;
        u8 is_valid;
        INSERT_PADDING_BYTES(7);
    };
    static_assert(sizeof(HandleEntry) == 0x10);

    struct NmouseInfo {
        u32 type_set;
        u8 is_type_set_valid;
        INSERT_PADDING_BYTES(3);
        u32 npad_id;
        u8 is_npad_id_valid;
        INSERT_PADDING_BYTES(3);
        u8 is_pointer_enabled;
        INSERT_PADDING_BYTES(0xb);
    };
    static_assert(sizeof(NmouseInfo) == InfoStride);

    // Only the fields applications are seen to read are known. The SDK copies this into
    // nn::hid::NmouseState (0x30 bytes): +0x00 to +0x24 as they are, then +0x30 to +0x24, +0x28 and
    // +0x2c as they are.
    struct NmouseDevState {
        u64 unknown_00;
        s64 sampling_number; // NmouseState +0x08
        s32 x; // NmouseState +0x10, the pointer, in the range given to InitializeNmouse
        s32 y; // NmouseState +0x14
        INSERT_PADDING_BYTES(0x8);
        s32 delta_x; // NmouseState +0x20
        INSERT_PADDING_BYTES(0x4);
        u32 unknown_28;
        u32 attribute; // NmouseState +0x2c, bit 1 must be set for applications to take the mouse as
                       // in use
        s32 delta_y;   // NmouseState +0x24
        INSERT_PADDING_BYTES(0x3c);
    };
    static_assert(sizeof(NmouseDevState) == 0x70);
    static_assert(offsetof(NmouseDevState, sampling_number) == 0x08);
    static_assert(offsetof(NmouseDevState, x) == 0x10);
    static_assert(offsetof(NmouseDevState, y) == 0x14);
    static_assert(offsetof(NmouseDevState, delta_x) == 0x20);
    static_assert(offsetof(NmouseDevState, attribute) == 0x2c);
    static_assert(offsetof(NmouseDevState, delta_y) == 0x30);

    using NmouseLifo = Lifo<NmouseDevState, MaxHandles>;
    static_assert(sizeof(NmouseLifo) == LifoStride);

    static constexpr u32 AttributeConnected = 1u << 0;
    static constexpr u32 AttributeActive = 1u << 1;

    struct Slot {
        u32 type_set;
        bool has_npad;
        bool moves;
    };
    // 0: player 1's left Joy-Con mouse, 1: its right one, 2: the merged mouse. The host mouse is
    // the right Joy-Con on the table: an application that took the left one as a mouse too would
    // stop reading its buttons (L, ZL).
    static constexpr std::array<Slot, 3> Slots{{
        {TypeJoyLeftJcms, true, false},
        {TypeJoyRightJcms, true, true},
        {TypeMerged, false, true},
    }};

    void WriteLayout();

    std::mutex mutex;
    Common::ProcessAddress transfer_memory{};
    Kernel::KProcess* owner{};
    s32 range_width{1920};
    s32 range_height{1080};
    bool has_last_position{};
    // A Joy-Con mouse is on the table while it is moved, lifted after a while still (so games leave
    // their mouse mode, as with the console's Joy-Con: Pokopia only lets moves be changed that
    // way).
    static constexpr s64 LiftAfterNs = 2'500'000'000;
    s64 last_moved_ns{};
    bool has_moved{};
    s32 last_x{};
    s32 last_y{};
    std::array<NmouseLifo, Slots.size()> lifos{};
    Core::HID::EmulatedDevices* emulated_devices{};
};

} // namespace Service::HID
