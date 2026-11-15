// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop uses libc's heap and the scheduler's standard notify-all path.
// The PS5 frontend otherwise provides these observations/scheduler selection hooks.
#include <cstddef>
#include <cstdio>
#include <malloc.h>

#include "headless/hud.h"
#include "src/fastmem.h"

extern "C" std::size_t eden_heap_committed() {
    const auto usage = mallinfo2();
    return static_cast<std::size_t>(usage.arena) + static_cast<std::size_t>(usage.hblkhd);
}

extern "C" bool eden_vulkan_handoff_enabled() {
    // ordered_read_trace.inc defaults this optional notify-one experiment to false.
    return false;
}

namespace Eden::Fastmem {
std::uint64_t Window64Base() noexcept {
    // The desktop uses HostMemory's POSIX arena, without the PS5 window's
    // 16 KiB alignment restrictions or stack relocation workaround.
    return 0;
}
} // namespace Eden::Fastmem

namespace Eden::Performance {
void ReportHeap(const char* why) {
    const auto usage = mallinfo2();
    std::fprintf(stderr, "LINUX_LIBC_HEAP arena=%zu mmap=%zu in_use=%zu reason=%s\n",
                 static_cast<std::size_t>(usage.arena),
                 static_cast<std::size_t>(usage.hblkhd),
                 static_cast<std::size_t>(usage.uordblks) +
                     static_cast<std::size_t>(usage.hblkhd),
                 why != nullptr ? why : "");
}
} // namespace Eden::Performance

namespace Eden {
HudSnapshot GetVulkanHud() {
    // Desktop frontends use their own UI or MangoHud rather than the PS5 overlay.
    return {};
}

void CountPresentedFrame() {
    // Only feeds the PS5 overlay's output-rate counter; desktop metrics are separate.
}
} // namespace Eden
