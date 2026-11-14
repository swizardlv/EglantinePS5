// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "performance.h"
#include "common/range_sets.inc"
#include "core/gpu_dirty_memory_manager.h"
#include "video_core/rasterizer_download_area.h"

namespace Perf = Eden::Performance;
using VideoCore::DownloadLanding;

namespace {
constexpr DAddr Page = 0x137fff000;

Common::RangeSet<DAddr> DeferredRanges() {
    Common::RangeSet<DAddr> ranges;
    for (const auto& write : Perf::SnapshotDeferredWrites()) {
        ranges.Add(write.address, write.end - write.address);
    }
    return ranges;
}

void CheckReplayLifetime() {
    assert(Perf::SnapshotDeferredWrites().empty());
    Perf::DeferWrite(Page + 0x100, 16, 0);
    assert(Perf::ExtendDeferredWrite(Page + 0x110, 16, 0));
    auto first = Perf::TakeDeferredWrites(0);
    assert(first && first->writes.size() == 1);
    assert(first->writes[0].address == Page + 0x100 && first->writes[0].end == Page + 0x120);
    assert(!Perf::ExtendDeferredWrite(Page + 0x120, 16, 0));
    assert(DeferredRanges().Intersects(Page + 0x100, 32));

    Core::GPUDirtyMemoryManager dirty;
    unsigned pending = 0;
    dirty.ForEachPending([&](auto, auto) { ++pending; });
    assert(pending == 0); // Taking a batch has not handed its writes to the dirty manager yet.
    dirty.Collect(Page + 0x100, 32);
    Perf::CompleteDeferredWrites(0, first);
    assert(DeferredRanges().Empty());
    Common::RangeSet<DAddr> collected;
    dirty.ForEachPending([&](auto address, auto size) { collected.Add(address, size); });
    assert(collected.Intersects(Page + 0x100, 32));
    unsigned applied = 0;
    std::function<void(PAddr, size_t)> apply = [&](auto address, auto size) {
        assert(address == Page + 0x100 && size == 64);
        ++applied;
    };
    dirty.Gather(apply);
    assert(applied == 1);
    pending = 0;
    dirty.ForEachPending([&](auto, auto) { ++pending; });
    assert(pending == 0);

    // New queued writes and multiple replay batches remain independently visible.
    Perf::DeferWrite(Page + 0x200, 16, 0);
    auto failed = Perf::TakeDeferredWrites(0);
    Perf::DeferWrite(Page + 0x300, 16, 0);
    auto second = Perf::TakeDeferredWrites(0);
    Perf::DeferWrite(Page + 0x400, 16, 1);
    assert(Perf::SnapshotDeferredWrites().size() == 3);
    std::weak_ptr<const Perf::DeferredWriteBatch> retained = failed;
    failed.reset();
    assert(!retained.expired() && DeferredRanges().Intersects(Page + 0x200, 16));
    Perf::CompleteDeferredWrites(0, second);
    assert(Perf::SnapshotDeferredWrites().size() == 2);
    // A failed replay stays protected until its owner explicitly completes recovery.
    Perf::CompleteDeferredWrites(0, retained.lock());
    assert(retained.expired());
    auto queued = Perf::TakeDeferredWrites(1);
    Perf::CompleteDeferredWrites(1, queued);
    assert(Perf::SnapshotDeferredWrites().empty());
    assert(!Perf::TakeDeferredWrites(0));
}

void CheckLateStore() {
    using namespace std::chrono_literals;
    std::array<u8, 4096> bytes{};
    std::atomic<bool> stored{};
    std::thread guest;
    const u64 waits = DownloadLanding::Waits();
    {
        DownloadLanding landing{Page, Page + bytes.size()};
        assert(DeferredRanges().Empty());
        guest = std::thread([&] {
            Perf::DeferWrite(Page, bytes.size(), 0);
            DownloadLanding::Await(Page, bytes.size());
            bytes.fill(0xa5);
            stored.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (DownloadLanding::Waits() == waits && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        assert(DownloadLanding::Waits() > waits && !stored.load(std::memory_order_acquire));
        // The waiting writer has released the deferred-queue lock.
        assert(DeferredRanges().Intersects(Page, bytes.size()));
        DownloadLanding::Await(Page - 16, 16);
        DownloadLanding::Await(Page + bytes.size(), 16);
        bytes.fill(0x12);
    }
    guest.join();
    assert(stored.load() && std::all_of(bytes.begin(), bytes.end(), [](u8 b) { return b == 0xa5; }));
    auto late = Perf::TakeDeferredWrites(0);
    Perf::CompleteDeferredWrites(0, late);
    assert(Perf::SnapshotDeferredWrites().empty());
}
} // namespace

int main() {
    CheckReplayLifetime();
    CheckLateStore();
    std::cout << "Deferred CPU writes: replay handoff, extension, retained failures, concurrent batches, "
                 "range boundaries and late landing barrier PASS\n";
}
