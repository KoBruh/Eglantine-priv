// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <cstdint>
#include <iostream>
#include "video_core/renderer_vulkan/command_reuse_policy.h"

int main() {
    std::uint32_t uses{};
    unsigned releases{};
    for (unsigned i = 0; i < 8000; ++i) {
        releases += Vulkan::ShouldReleaseCommandMemory(uses, 8, false);
    }
    assert(releases == 1000 && uses == 0);
    assert(!Vulkan::ShouldReleaseCommandMemory(uses, 8, false));
    assert(Vulkan::ShouldReleaseCommandMemory(uses, 8, true));
    assert(uses == 0);
    for (unsigned i = 0; i < 1000; ++i) {
        assert(Vulkan::ShouldReleaseCommandMemory(uses, 1, false));
        assert(!Vulkan::ShouldReleaseCommandMemory(uses, 0, true));
    }
    std::cout << "Command recording reuse: periodic, pressure and compatibility "
                 "modes passed\n";
}
