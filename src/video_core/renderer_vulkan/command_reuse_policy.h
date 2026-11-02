// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace Vulkan {

// Called only after the resource pool confirms that the GPU has finished with this buffer.
inline bool ShouldReleaseCommandMemory(std::uint32_t& uses, unsigned interval, bool pressure) {
    if (interval == 0) {
        uses = 0;
        return false;
    }
    if (pressure || ++uses >= interval) {
        uses = 0;
        return true;
    }
    return false;
}

} // namespace Vulkan
