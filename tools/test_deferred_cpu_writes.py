#!/usr/bin/env python3
"""Host regression using production deferred, pending-range and landing implementations."""
from pathlib import Path
import subprocess
import os
import tempfile
BASE=Path(__file__).resolve().parents[1]

def body(text, signature):
    start=text.index(signature)
    opening=text.index('{',start)
    level=1; end=opening+1
    while level:
        if text[end]=='{':level+=1
        if text[end]=='}':level-=1
        end+=1
    return text[start:end]

dirty=(BASE/'Eglantine/src/core/gpu_dirty_memory_manager.h').read_text()
dirty=dirty[dirty.index('class GPUDirtyMemoryManager'):dirty.rindex('} // namespace Core')]
gpu=(BASE/'Eglantine/src/video_core/gpu.cpp').read_text()
pending=body(gpu,'    void ForEachPendingCpuWrite(')
core=(BASE/'Eglantine/src/core/core.cpp').read_text()
gather=body(core,'void System::GatherGPUDirtyMemory(').replace('System::','')
cache=(BASE/'Eglantine/src/video_core/buffer_cache/buffer_cache.h').read_text()
pending_ranges=body(cache,'Common::RangeSet<DAddr> BufferCache<P>::PendingCpuWriteRanges()')
write_download=body(cache,'void BufferCache<P>::WriteDownloadedMemory(')
ranges=(BASE/'Eglantine/src/common/range_sets.inc').read_text()
range_impl=ranges[ranges.index('template <typename AddressType>\nstruct RangeSet'):ranges.index('template <typename AddressType>\nstruct OverlapRangeSet')]
range_functions=ranges[ranges.index('template <typename AddressType>\nRangeSet<AddressType>::RangeSet()'):ranges.index('template <typename AddressType>\nOverlapRangeSet<AddressType>::OverlapRangeSet()')]
landing=(BASE/'Eglantine/src/video_core/rasterizer_download_area.h').read_text()
landing=landing[landing.index('struct DownloadLandingState'):landing.index('/// EDEN_NX2_READ_CHECK:')]
memory=(BASE/'Eglantine/src/core/memory.cpp').read_text()
write_branch=memory[memory.index('            if (current_area.last_address != subaddress)'):]
write_branch=write_branch[:write_branch.index('        });')]
source=r'''
#include <cassert>
#include <bit>
#include <cstring>
#include <functional>
#include <iostream>
#include <chrono>
#include <future>
#include <span>
#include <thread>
#include "headless/performance.h"
#include "common/range_sets.h"
'''+"namespace Common {\n"+range_impl+range_functions+"}\n"+r'''
using DAddr = u64;
using PAddr = u64;
'''+"namespace VideoCore {\n"+landing+"}\n"+r'''
namespace Core {
constexpr size_t DEVICE_PAGEBITS = 12;
'''+dirty+r'''
}
struct System {
    struct Impl { std::array<Core::GPUDirtyMemoryManager,6> gpu_dirty_memory_managers; } storage;
    Impl* impl=&storage;
    auto& GetGPUDirtyMemoryManager() { return impl->gpu_dirty_memory_managers; }
'''+gather+r'''
};
struct GPU {
    System system;
    std::mutex applying_writes_mutex;
    std::vector<std::vector<std::pair<DAddr,u64>>*> applying_writes;
'''+pending+r'''
};
constexpr DAddr DevicePage=0x137fff000;

struct DeviceMemory {
    std::array<u8,4096> bytes{};
    void WriteBlockUnsafe(DAddr address,const u8* data,u64 size) {
        assert(address>=DevicePage && address+size<=DevicePage+bytes.size());
        std::memcpy(bytes.data()+address-DevicePage,data,size);
    }
};
template<class P> struct BufferCache {
    DeviceMemory device_memory;
    std::vector<int> early_landed;
    std::function<void(const std::function<void(DAddr,size_t)>&)> pending_cpu_writes;
    void ForgetEarlyLanded(DAddr,u64) {}
    Common::RangeSet<DAddr> PendingCpuWriteRanges();
    void WriteDownloadedMemory(const Common::RangeSet<DAddr>&,DAddr,const u8*,u64);
};
template<class P>
'''+pending_ranges+r'''
template<class P>
'''+write_download+r'''
// Actual rasterizer-write branch with only the service object supplied by the harness.
struct WriteHarness {
    struct Service {
        Service& GPU() { return *this; }
        bool OnCPUWrite(DAddr address,u64 size) {
            Eden::Performance::DeferWrite(address,size,0);
            return false;
        }
    } system;
    std::array<Core::GPUDirtyMemoryManager,6> gpu_dirty_managers;
    void Handle(DAddr address,u64 size) {
        struct { u64 last_address=~u64{0}; } current_area;
        const u64 subaddress=address>>12;
        const size_t core=0;
'''+write_branch+r'''
    }
};
int main() {
    namespace Perf=Eden::Performance;
    using VideoCore::DownloadLanding;
    using namespace std::chrono_literals;
    assert(Perf::cpu_write_defer.load());
    GPU gpu;
    BufferCache<int> buffer;
    buffer.pending_cpu_writes=[&](const auto& callback) { gpu.ForEachPendingCpuWrite(callback); };
    const auto clear_dirty=[&] {
        std::function<void(PAddr,size_t)> discard=[](PAddr,size_t){};
        gpu.system.GatherGPUDirtyMemory(discard);
    };
    const auto collect_batch=[&](const auto& batch) {
        for(const auto& w:batch->writes) {
            for(u64 address=w.address;address<w.end;) {
                constexpr u64 region_size=1ULL<<(Core::DEVICE_PAGEBITS-1);
                const u64 size=std::min<u64>(w.end-address,region_size-(address&(region_size-1)));
                gpu.system.GetGPUDirtyMemoryManager()[0].Collect(address,size);
                address+=size;
            }
        }
    };
    const auto all_bytes=[&](u8 value) {
        return std::all_of(buffer.device_memory.bytes.begin(),buffer.device_memory.bytes.end(),
                           [value](u8 b){return b==value;});
    };
    std::array<u8,4096> old_gpu{};
    std::mutex cache_mutex;
    {
        std::scoped_lock lock(cache_mutex);
        std::thread guest([&] {
            assert(!Perf::GuestCacheLockOrDefer(cache_mutex));
            WriteHarness harness;
            harness.Handle(DevicePage,4096);
            buffer.device_memory.bytes.fill(0xa5);
        });
        guest.join();
    }
    auto queued=buffer.PendingCpuWriteRanges();
    assert(queued.Intersects(DevicePage,4096));
    buffer.WriteDownloadedMemory(queued,DevicePage,old_gpu.data(),4096);
    assert(all_bytes(0xa5));
    auto taken=Perf::TakeDeferredWrites(0);
    assert(taken && taken->writes.size()==1);
    assert(Perf::SnapshotDeferredWrites().size()==1); // Taken but not collected remains visible.
    auto in_flight=buffer.PendingCpuWriteRanges();
    buffer.WriteDownloadedMemory(in_flight,DevicePage,old_gpu.data(),4096);
    assert(all_bytes(0xa5));
    collect_batch(taken);
    Perf::CompleteDeferredWrites(0,taken);
    assert(Perf::SnapshotDeferredWrites().empty());
    auto collected=buffer.PendingCpuWriteRanges();
    buffer.WriteDownloadedMemory(collected,DevicePage,old_gpu.data(),4096);
    assert(all_bytes(0xa5));
    clear_dirty();
    buffer.WriteDownloadedMemory(buffer.PendingCpuWriteRanges(),DevicePage,old_gpu.data(),4096);
    assert(all_bytes(0)); // Genuine GPU-only bytes still land after CPU writes are applied.

    // Extend only queued entries; multiple replay batches and new queued writes coexist.
    Perf::DeferWrite(DevicePage+0x100,16,0);
    assert(Perf::ExtendDeferredWrite(DevicePage+0x110,16,0));
    auto first=Perf::TakeDeferredWrites(0);
    assert(first->writes[0].address==DevicePage+0x100 && first->writes[0].end==DevicePage+0x120);
    assert(!Perf::ExtendDeferredWrite(DevicePage+0x120,16,0));
    Perf::DeferWrite(DevicePage+0x200,16,0);
    auto second=Perf::TakeDeferredWrites(0);
    Perf::DeferWrite(DevicePage+0x300,16,0);
    assert(Perf::SnapshotDeferredWrites().size()==3);
    Perf::CompleteDeferredWrites(0,first);
    assert(Perf::SnapshotDeferredWrites().size()==2);
    // Dropping a caller's failed-replay handle must not drop protection.
    second.reset();
    assert(Perf::SnapshotDeferredWrites().size()==2);
    {
        auto& queue=Perf::deferred_queues[0];
        std::shared_ptr<const Perf::DeferredWriteBatch> retained;
        { std::scoped_lock lock(queue.mutex); retained=queue.replaying.front(); }
        Perf::CompleteDeferredWrites(0,retained); // Model successful recovery of retained work.
    }
    auto last=Perf::TakeDeferredWrites(0);
    Perf::CompleteDeferredWrites(0,last);
    assert(!Perf::TakeDeferredWrites(0));

    // Exact range boundaries: adjacent GPU bytes must not be protected accidentally.
    Perf::DeferWrite(DevicePage+63,2,0);
    buffer.device_memory.bytes.fill(0xa5);
    auto partial=buffer.PendingCpuWriteRanges();
    buffer.WriteDownloadedMemory(partial,DevicePage,old_gpu.data(),4096);
    for(size_t i=0;i<4096;++i) assert(buffer.device_memory.bytes[i]==((i==63||i==64)?0xa5:0));
    auto boundary=Perf::TakeDeferredWrites(0);
    Perf::CompleteDeferredWrites(0,boundary);

    // Callbacks can enqueue another write: no deferred-queue lock is held during enumeration.
    Perf::DeferWrite(DevicePage,16,0);
    unsigned callbacks=0;
    gpu.ForEachPendingCpuWrite([&](DAddr,size_t) {
        if(callbacks++==0) Perf::DeferWrite(DevicePage+0x100,16,0);
    });
    assert(callbacks==1 && Perf::SnapshotDeferredWrites().size()==2);
    auto callback_batch=Perf::TakeDeferredWrites(0);
    Perf::CompleteDeferredWrites(0,callback_batch);

    // CPU arrival after the download snapshot must wait before its store.
    std::thread guest;
    std::atomic<bool> stored{};
    const u64 waits=DownloadLanding::Waits();
    {
        DownloadLanding landing(DevicePage,DevicePage+4096);
        auto empty=buffer.PendingCpuWriteRanges();
        assert(empty.Empty());
        guest=std::thread([&] {
            WriteHarness harness;
            harness.Handle(DevicePage,4096);
            buffer.device_memory.bytes.fill(0xa5);
            stored.store(true,std::memory_order_release);
        });
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(DownloadLanding::Waits()==waits && std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        assert(DownloadLanding::Waits()>waits && !stored.load(std::memory_order_acquire));
        // Snapshot can take queue locks while the guest awaits a landing.
        assert(Perf::SnapshotDeferredWrites().size()==1);
        DownloadLanding::Await(DevicePage+4096,16); // Adjacent, nonoverlapping writes do not wait.
        buffer.WriteDownloadedMemory(empty,DevicePage,old_gpu.data(),4096);
        assert(all_bytes(0));
    }
    guest.join();
    assert(stored.load() && all_bytes(0xa5));
    auto late=Perf::TakeDeferredWrites(0);
    Perf::CompleteDeferredWrites(0,late);
    std::cout<<"PASS: queued/in-flight/collected handoff, retained failed replay, extend, concurrent batches, "
                "range boundaries, callback lock order, late landing barrier and GPU-only writes\n";
}

'''
with tempfile.TemporaryDirectory(prefix="deferred-cpu-writes-") as directory:
    cpp=Path(directory)/"test.cpp"
    binary=Path(directory)/"test"
    cpp.write_text(source)
    subprocess.run([os.environ.get("CXX","c++"),"-std=c++20","-O1","-g","-pthread",
                    "-I"+str(BASE),"-I"+str(BASE/"Eglantine/src"),str(cpp),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
