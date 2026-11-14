// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <limits>

#include <vulkan/vulkan_core.h>

namespace Vulkan {

// The console's guest-memory service is absent on other platforms. Retry only after it
// returned memory, and only for device OOM; a lost device must reach the caller unchanged.
template <typename Create, typename Reclaim>
VkResult RetryAfterMemoryReclaim(VkResult result, VkDeviceSize bytes, Reclaim* reclaim,
                                Create&& create) {
    if (reclaim == nullptr) {
        return result;
    }
    constexpr std::size_t margin = std::size_t{64} << 20;
    constexpr auto max = (std::numeric_limits<std::size_t>::max)();
    const std::size_t request = bytes > max - margin ? max : static_cast<std::size_t>(bytes) + margin;
    for (int attempt = 0; result == VK_ERROR_OUT_OF_DEVICE_MEMORY && attempt < 3; ++attempt) {
        if ((*reclaim)(request) == 0) {
            break;
        }
        result = create();
    }
    return result;
}

} // namespace Vulkan
