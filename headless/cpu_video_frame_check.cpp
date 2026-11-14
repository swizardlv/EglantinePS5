// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/cpu_framebuffer.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <future>
#include <limits>
#include <thread>

int main() {
    using Tegra::CpuFramebuffer;
    using Tegra::CpuVideoFrameStore;
    auto image = std::make_shared<CpuFramebuffer>(CpuFramebuffer{
        .width = 3, .height = 2, .pitch = 16,
        .pixels = {255,0,0,255, 0,255,0,255, 0,0,255,255, 99,99,99,99,
                   1,2,3,255, 4,5,6,255, 7,8,9,255, 99,99,99,99}});
    std::array<std::uint8_t, 24> packed{};
    assert(image->IsValid() && image->CopyPacked(packed));
    assert(packed[0] == 255 && packed[5] == 255 && packed[10] == 255);
    assert(packed[12] == 1 && packed[20] == 7 && packed[23] == 255);
    std::array<std::uint8_t, 23> short_out{};
    short_out.fill(77);
    assert(!image->CopyPacked(short_out));
    for (auto value : short_out) assert(value == 77);
    CpuFramebuffer invalid = *image;
    invalid.pitch = 11;
    assert(!invalid.IsValid());
    invalid.pitch = std::numeric_limits<std::size_t>::max();
    assert(!invalid.IsValid());
    invalid = *image;
    invalid.pixels.pop_back();
    assert(!invalid.IsValid());
    invalid = *image;
    invalid.width = 0;
    assert(!invalid.IsValid());

    CpuVideoFrameStore store;
    assert(!store.Snapshot());
    assert(!store.Publish(0, image));
    const auto first = store.Start();
    assert(store.Publish(first, image));
    auto queued = store.Snapshot();
    store.Stop(first);
    assert(!store.Snapshot() && queued->pixels[0] == 255);
    assert(!store.Publish(first, image));
    const auto second = store.Start();
    assert(second != first && store.Publish(second, image));
    store.Stop(first);
    assert(store.Snapshot());
    assert(store.Restart(first) == 0 && store.Snapshot());
    const auto third = store.Restart(second);
    assert(third != 0 && third != second);
    assert(!store.Snapshot() && !store.Publish(second, image));
    assert(store.Publish(third, image));
    assert(!store.Publish(third, {}));
    assert(!store.Publish(third, std::make_shared<CpuFramebuffer>(invalid)));
    assert(store.Snapshot() == image);

    // A conversion blocked behind Stop must not resurrect an old video plane.
    std::promise<void> converted, release;
    auto ready = converted.get_future();
    auto proceed = release.get_future();
    std::thread decoder([&] {
        converted.set_value();
        proceed.wait();
        assert(!store.Publish(third, image));
    });
    ready.wait();
    store.Stop(third);
    release.set_value();
    decoder.join();
    assert(!store.Snapshot() && queued->CopyPacked(packed));
    std::puts("CPU video frame PASS: RGBA/padding, bounds/overflow, ownership, stop/restart and late conversion");
}
