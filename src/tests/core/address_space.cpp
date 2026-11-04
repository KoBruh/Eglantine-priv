// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/hle/kernel/board/nintendo/nx/k_system_control.h"
#include "core/hle/kernel/k_address_space_info.h"
#include "core/hle/kernel/k_memory_layout.h"

namespace {

using SystemControl = Kernel::Board::Nintendo::Nx::KSystemControl;

constexpr std::array memory_layouts{
    Settings::MemoryLayout::Memory_4Gb,  Settings::MemoryLayout::Memory_6Gb,
    Settings::MemoryLayout::Memory_8Gb,  Settings::MemoryLayout::Memory_10Gb,
    Settings::MemoryLayout::Memory_12Gb,
};

struct MemoryLayoutRestore {
    Settings::MemoryLayout configured = Settings::values.memory_layout_mode.GetValue();
    ~MemoryLayoutRestore() {
        Settings::values.memory_layout_mode.SetValue(configured);
    }
};

} // Anonymous namespace

TEST_CASE("The 39-bit heap region holds the application pool of every memory layout",
          "[kernel][heap]") {
    MemoryLayoutRestore restore;
    const std::size_t heap_region =
        Kernel::KAddressSpaceInfo::GetAddressSpaceSize(39, Kernel::KAddressSpaceInfo::Type::Heap);

    // A process may grow its heap to its whole pool (its resource limit), so the region must not
    // be what stops it: the 12 GiB layout's pool is larger than Switch 1's 8 GiB region.
    for (const auto layout : memory_layouts) {
        Settings::values.memory_layout_mode.SetValue(layout);
        INFO("memory layout " << static_cast<int>(layout));
        CHECK(SystemControl::Init::GetApplicationPoolSize() <= heap_region);
    }

    // The heap Pokopia, sized from the 12 GiB layout, asks for at boot.
    constexpr std::size_t pokopia_heap_size = 0x261600000;
    Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_12Gb);
    CHECK(pokopia_heap_size <= SystemControl::Init::GetApplicationPoolSize());
    CHECK(pokopia_heap_size <= heap_region);
}

TEST_CASE("The kernel page table heap covers the DRAM of every memory layout", "[kernel][heap]") {
    MemoryLayoutRestore restore;
    // The 12 GiB layout has the Switch 2 graphics server's memory on top (14 GiB of DRAM).
    for (const auto layout : memory_layouts) {
        Settings::values.memory_layout_mode.SetValue(layout);
        INFO("memory layout " << static_cast<int>(layout));
        CHECK(Kernel::GetMaximumOverheadSize(SystemControl::Init::GetIntendedMemorySize()) <=
              Kernel::KernelPageTableHeapSize);
    }
}
