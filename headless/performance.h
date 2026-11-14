// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "common/cpu_features.h"
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace Eden::Performance {
// Emulated guest cores: Core::Hardware::NUM_CPU_CORES of the Eglantine base (Switch 2: 6).
inline constexpr unsigned kGuestCores = 6;
void RegisterWorker(const char* name);
// Development: keep other named threads off guest cores 0-2 and their SMT siblings.
void SetSecondaryPlacement(bool enabled);
void PlatformChecks();
// Main thread only, between GPU readiness and guest shutdown.
void Snapshot();
// GPU worker only: firmware rejects cross-thread CPU-time sampling.
void SampleGpuFrame(unsigned frame);
#ifdef EDEN_DEV_PROFILE
void BeginPcSampling();
void PollGpuPc();
// Development: A64 block compilation split into translate, optimize, emit and the emitter's
// block-range registration (part of emit), per guest core, in nanoseconds.
inline std::array<std::array<std::atomic<unsigned long long>, 4>, 6> jit_phase_ns{};
// Development: A64 blocks per guest core that no core had compiled before, that this core had
// compiled before (after an invalidation), and that another core compiled first, by how long
// before: under 1 ms, 10 ms, 100 ms, 1 s, 10 s, and later. Counted only with dev-settings
// jit_dups=on (a process-wide lock and map insert per compiled block).
inline std::array<std::array<std::atomic<unsigned long long>, 8>, 6> jit_duplicates{};
inline std::atomic<bool> jit_duplicate_tracking{false};
// Guest core whose host PCs the development sampler collects (dev-settings pc_core=N, default 0).
inline std::atomic<unsigned> pc_sample_core{0};
// Development: sample that core at ~500 Hz from its registration (dev-settings pc_fast=on).
inline std::atomic<bool> pc_fast{false};
// Development: honour capture-once.txt from a session's first 30 s segment instead of its fifth
// (dev-settings capture=early; the runner's lifecycle returns need a capture within ~80 s).
inline std::atomic<bool> capture_early{false};
// Development: keep Eden's default library applets, which start some from the firmware
// (dev-settings applets=firmware). The port otherwise uses the built-in ones (main.cpp).
inline std::atomic<bool> firmware_applets{false};
#endif
// One writer per guest core. JIT state is read only by its owning worker after Run.
enum class CpuPhase : unsigned { Kernel, Guest, Idle };
struct alignas(64) CpuState { std::atomic<CpuPhase> phase{}; };
inline std::array<CpuState, 6> cpu_state{};
void SampleCpu(unsigned core, unsigned long long thread, unsigned long long pc, unsigned svc, unsigned fpcr);
// The overlay's CPU field (hud.h, HudMachine::cpu): how busy the busiest emulated core was since
// the previous call, as a percentage of one CPU; ~0u until two readings of a core are known, or
// where the console's thread clocks cannot be read. A game is held back by its busiest core, which
// a share of every CPU of the console would hide. Each call asks the cores for new readings, so
// with the field off nothing more is read. One caller at a time (the overlay, once a second).
unsigned BusiestGuestCore();
struct alignas(64) Totals {
    std::atomic<unsigned long long> calls{}, nanoseconds{}, requested_bytes{};
};
inline std::array<Totals, 6> compilation;
// Temporary MKW timing probes.
inline Totals mkw_transcode, mkw_transcode_finish;
inline std::array<Totals, 512> mkw_guest_run{}, mkw_main_wakers{};
// Temporary bounded dependency and run-exit samples. Exit PCs are not statistical PC samples.
inline std::array<std::array<Totals, 512>, 11> mkw_worker_wakers{};
struct MkwRunExit {
    std::atomic<unsigned long long> key{}, calls{}, ns{}, max_ns{};
};
inline std::array<MkwRunExit, 8192> mkw_run_exits{};
inline std::atomic<unsigned long long> mkw_run_exit_overflow{};
// Latest slow slice for each observed guest worker, sampled at most once per second.
// Atomic sequence brackets protect the external reader from partially published records.
inline std::array<std::array<std::atomic<unsigned long long>, 24>, 11> mkw_slow_runs{};
template <typename Jit, typename Memory>
inline void MkwSlowRun(unsigned long long tid, unsigned long long now,
                       unsigned long long ns, unsigned halt, const Jit& jit, Memory& memory) {
    if (tid < 181 || tid > 191 || ns < 2'000'000) return;
    auto& row = mkw_slow_runs[tid - 181];
    if (now - row[1].load(std::memory_order_relaxed) < 1'000'000'000) return;
    row[0].fetch_add(1, std::memory_order_seq_cst);
    row[1].store(now); row[2].store(ns); row[3].store(jit.GetPC());
    row[4].store(jit.GetRegister(30)); row[5].store(jit.GetFpcr()); row[6].store(halt);
    row[7].store(jit.GetRegister(0)); row[8].store(jit.GetRegister(1));
    auto fp = jit.GetRegister(29);
    row[9].store(fp);
    for (unsigned i = 10; i < 24; ++i) {
        unsigned long long ret = 0;
        if (fp && (fp & 7) == 0 && memory.IsValidVirtualAddressRange(fp, 16)) {
            ret = memory.Read64(fp + 8);
            const auto next = memory.Read64(fp);
            fp = next > fp && next - fp < 1024 * 1024 ? next : 0;
        } else fp = 0;
        row[i].store(ret);
    }
    row[0].fetch_add(1, std::memory_order_seq_cst);
}

inline void MkwRecordRunExit(unsigned long long tid, unsigned long long pc,
                             unsigned long long ns) {
    if (tid < 181 || tid > 191 || pc >= (1ULL << 48)) return;
    const auto key = (tid << 48) | pc;
    unsigned slot = static_cast<unsigned>((pc >> 2) ^ (pc >> 16) ^ tid) % mkw_run_exits.size();
    for (unsigned probe = 0; probe < mkw_run_exits.size(); ++probe) {
        auto& row = mkw_run_exits[slot];
        auto held = row.key.load(std::memory_order_relaxed);
        if (held == 0) {
            row.key.compare_exchange_strong(held, key, std::memory_order_relaxed);
            held = row.key.load(std::memory_order_relaxed);
        }
        if (held == key) {
            row.calls.fetch_add(1, std::memory_order_relaxed);
            row.ns.fetch_add(ns, std::memory_order_relaxed);
            auto max = row.max_ns.load(std::memory_order_relaxed);
            while (max < ns && !row.max_ns.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {}
            return;
        }
        slot = (slot + 1) % mkw_run_exits.size();
    }
    mkw_run_exit_overflow.fetch_add(1, std::memory_order_relaxed);
}

inline std::array<std::atomic<unsigned long long>, 6> evacuations{};
inline Totals storage;
inline Totals jit_protection;
inline Totals rasterizer_draw;
// The six phases of that draw path (RasterizerVulkan::PrepareDraw) in order: flushing the pending
// work, taking the current pipeline, waiting for the two cache mutexes (and binding the pipeline
// to the engine), configuring it, applying dynamic state, and recording. Calls count each phase's
// early returns as well as its work - a draw with no pipeline stops at draw_pipeline, one
// Configure rejects stops at draw_config - so subtracting their sum from rasterizer_draw gives
// what went unmarked. Two clock reads per phase rather than a Timer apiece, a draw has six of them.
inline Totals draw_flush, draw_pipeline, draw_lock_wait, draw_config, draw_state, draw_record;
// The six phases inside draw_config - which is GraphicsPipeline::Configure, past SetEngine's two
// pointer assignments - in the order ConfigureImpl runs them: syncing the sampler and image tables
// and the uniform-buffer state, reading every texture handle out of guest memory and filling the
// views, binding the buffers, queueing the descriptor pushes and the upload barrier, updating
// render targets and checking feedback, and ConfigureDraw's compare-and-record. They nest inside
// draw_config, so their sum cannot exceed it; calls count the early returns that leave a later
// phase unmarked.
inline Totals cfg_sync, cfg_read, cfg_bind, cfg_desc, cfg_rt, cfg_draw;
// The three inside cfg_bind, on a clock of their own started where cfg_read ends so that they
// chain against each other and still sum to cfg_bind: the texture-buffer loop over the enabled
// stages, the buffer update pass, and the host geometry binds.
inline Totals bind_tbo, bind_update, bind_geometry;
// The four calls inside bind_geometry and the three inside bind_update. Both walk the memory
// tracker's pages through SynchronizeBuffer once per buffer on every draw whether or not a byte
// of it is dirty, so which call carries the time cannot be read off the source - the loop over
// many stages looked expensive and measured at 2.8%.
inline Totals geo_index, geo_vertex, geo_xf, geo_indirect;
inline Totals upd_state, upd_stage, upd_indirect;
inline Totals gpu_queue_wait, gpu_dispatch;
// The three phases of the dispatch (Tegra::Control::Scheduler::Push), measured inside it: taking
// the channel lock and binding it, queueing the submit list, and then executing the guest's
// methods - the O(draws) part. gpu_dispatch is their sum plus the loop's own overhead, so these
// say whether the time is the lock, the queueing, or the methods themselves.
inline Totals gpu_sched_lock, gpu_dma_push, gpu_dma_dispatch;
// Producer-side waits outside the dispatch timer: forced fence drains on the GPU
// thread, free presentation-frame waits, and guest pushes into a full GPU queue.
inline Totals gpu_fence_drain, gpu_present_wait, gpu_queue_full;
// Texture-cache GC GPU drains (RunGarbageCollector). A dirty eviction copies the image back to
// guest memory and waits for the GPU first (Scheduler::Finish). The batched path spends one drain
// on up to sixteen images; the unbatched path is the per-image fallback in Cleanup, which the
// prefetch cannot take (an image larger than its byte budget, or one that arrives after the
// lookahead stopped). calls = drains, requested_bytes = images covered: one drain per image in the
// unbatched case is the stall to drive out.
inline Totals gc_batched_drain, gc_unbatched_drain;
// Guest threads entering the GPU caches: CPU writes to tracked pages and flush-area lookups.
inline Totals guest_cpu_write, guest_cpu_read;
// Those entries take the buffer and texture cache locks, which the GPU thread holds for one draw at
// a time. A guest core that sleeps in the kernel for each wait loses more than most holds last
// (a large open-world game: 1.5-3.2 ms per frame at 8-12 us per write), so it first retries try_lock for a bounded
// ~25 us (dev-settings cache_spin=N retries; 0 blocks at once, as upstream). Guest cores are
// pinned alone, so the spin takes no CPU from the lock holder.
inline std::atomic<unsigned> cache_lock_spins{256};
inline std::atomic<unsigned long long> cache_lock_contended{0}, cache_lock_blocked{0};
// Guest threads asleep on a cache lock now (YieldToGuests).
inline std::atomic<unsigned> cache_lock_sleepers{0};
// A guest write that leaves a buffer page tracked by the GPU caches costs a trip through
// OnCPUWrite, mostly waiting for the buffer cache lock the GPU thread holds while it prepares each
// draw (a game writing ~3,300 buffer pages per frame spent a third of two guest cores there). With
// N > 0 the same trip also marks up to N following buffer pages CPU-modified, as their own first
// write would, stopping at a page no buffer covers or one the GPU modified; texture and shader
// pages stay tracked by their caches. A page marked ahead and never written is uploaded again
// unchanged. That game wrote its buffers in order: 48.6 fps with 0, 58.7 with 16, 60 (its cap)
// with 64, the trips falling from 159,000 to 4,500 a second. dev-settings cpu_write_ahead=N
// (0-256) overrides it.
inline std::atomic<unsigned> cpu_write_ahead{64};
// The guest core of the write OnCPUWrite serves, set by HandleRasterizerWrite (headless/
// CMakeLists.txt). Threads that are not guest cores share the last one, as Eden's dirty
// managers do.
inline thread_local unsigned cpu_write_core = kGuestCores - 1;
// The GPU command interpreter (DmaPusher::ProcessCommands) calls out once per data word. An
// incrementing write whose registers only store their value (no engine work until a later method
// executes) is handed to the engine's pending-write list in one pass instead; registers that start
// work keep the per-word path. Not while Eglantine's split GPU thread is active (its back thread
// owns the engines; the words go through its ring). dev-settings dma_batch=off restores one call
// per word (upstream).
inline std::atomic<bool> dma_batch{true};
// Development (EDEN_DEV_PROFILE): incrementing runs taken in one pass, their words, and the data
// words that went through the per-word path. Written by the GPU thread only.
inline std::atomic<unsigned long long> dma_batch_runs{}, dma_batch_words{}, dma_single_words{};
template <typename Mutex>
inline void GuestCacheLock(Mutex& mutex) {
    if (mutex.try_lock()) return;
    cache_lock_contended.fetch_add(1, std::memory_order_relaxed);
    for (unsigned tries = cache_lock_spins.load(std::memory_order_relaxed); tries != 0; --tries) {
        for (int pause = 0; pause < 4; ++pause) __builtin_ia32_pause();
        if (mutex.try_lock()) return;
    }
    cache_lock_blocked.fetch_add(1, std::memory_order_relaxed);
    cache_lock_sleepers.fetch_add(1, std::memory_order_acq_rel);
    mutex.lock();
    cache_lock_sleepers.fetch_sub(1, std::memory_order_acq_rel);
}
// The GPU thread, holding neither cache lock, before it takes them for a draw: while a guest
// thread sleeps on one, wait for it to have the lock (it wakes within a fraction of a
// millisecond, and what it does under the lock is short). Bounded at about 2 ms, for a
// sleeper that is held up by something else. One load of a counter when nobody waits.
inline void YieldToGuests() {
    if (cache_lock_sleepers.load(std::memory_order_relaxed) == 0) return;
    const auto now = [] { return Common::g_wall_clock.GetTimeNS().count(); };
    const long long until = now() + 2'000'000;
    while (cache_lock_sleepers.load(std::memory_order_acquire) != 0 && now() < until)
        for (int pause = 0; pause < 16; ++pause) __builtin_ia32_pause();
}
// A guest write to a GPU-tracked page whose cache lock the GPU thread holds (it holds both for each
// draw's preparation, up to 40% of the time in heavy scenes, longer when a scene change creates and
// uploads resources) no longer waits: OnCPUWrite queues it here and returns as for an untracked
// write. Before its next draw, clear, dispatch or engine copy (MemoryManager::FlushCaching) the GPU
// thread runs each queued write through OnCPUWrite as the guest would have, and a write to
// GPU-modified data goes to that core's dirty ranges for the next fence, as upstream: so the caches
// see the write no later than had the guest waited, and GPU-written data keeps upstream's handling.
// dev-settings cpu_write_defer=off waits as before.
inline std::atomic<bool> cpu_write_defer{true};
// One queue per guest core (the last: other threads). Writes to the page of the queue's last
// entry, made before the GPU thread takes the queue, widen that entry instead of trying the locks
// again, so a burst of writes to a page costs one replay.
struct DeferredWrite {
    unsigned long long address, end;
};
struct DeferredWriteBatch {
    std::vector<DeferredWrite> writes;
};
struct DeferredQueue {
    std::mutex mutex;
    std::vector<DeferredWrite> writes;
    std::vector<std::shared_ptr<const DeferredWriteBatch>> replaying;
    unsigned taken = 0;  // Takes so far, under mutex.
};
inline std::array<DeferredQueue, kGuestCores> deferred_queues;
inline thread_local unsigned long long deferred_page = ~0ULL;
inline thread_local unsigned deferred_take = ~0U;
// Set by the GPU thread while it runs the queues (FlushCaching): a replayed write waits for a
// lock a guest thread holds rather than being queued again behind the draw it must precede.
inline thread_local bool deferred_replay = false;
// Set after a write is queued; the GPU thread clears it and runs the queues.
inline std::atomic<bool> gpu_dirty_pending{false};
inline std::atomic<unsigned long long> cpu_write_deferred{}, gpu_dirty_replays{};
inline bool ExtendDeferredWrite(unsigned long long address, unsigned long long size, unsigned core) {
    if ((address >> 12) != deferred_page) return false;
    auto& queue = deferred_queues[core % kGuestCores];
    std::scoped_lock lock{queue.mutex};
    if (queue.taken != deferred_take || queue.writes.empty() || (queue.writes.back().address >> 12) != deferred_page)
        return false;
    auto& last = queue.writes.back();
    last.address = std::min(last.address, address);
    last.end = std::max(last.end, address + size);
    return true;
}
inline void DeferWrite(unsigned long long address, unsigned long long size, unsigned core) {
    auto& queue = deferred_queues[core % kGuestCores];
    {
        std::scoped_lock lock{queue.mutex};
        queue.writes.push_back({address, address + size});
        deferred_page = address >> 12;
        deferred_take = queue.taken;
    }
    gpu_dirty_pending.store(true, std::memory_order_release);
}
// Keep taken writes visible until replay has invalidated their caches or collected them in the
// dirty manager. A failed replay retains its batch: dropping it would allow a stale download.
inline std::shared_ptr<const DeferredWriteBatch> TakeDeferredWrites(unsigned core) {
    auto& queue = deferred_queues[core];
    std::scoped_lock lock{queue.mutex};
    if (queue.writes.empty()) return {};
    auto batch = std::make_shared<DeferredWriteBatch>();
    queue.replaying.push_back(batch);
    batch->writes.swap(queue.writes);
    ++queue.taken;
    return batch;
}
inline void CompleteDeferredWrites(unsigned core,
                                   const std::shared_ptr<const DeferredWriteBatch>& batch) {
    auto& queue = deferred_queues[core];
    std::scoped_lock lock{queue.mutex};
    std::erase(queue.replaying, batch);
}
// Snapshot before enumerating dirty managers so a write handed from replay to Collect cannot
// disappear between the two snapshots. Callbacks and landing waits run after these locks release.
inline std::vector<DeferredWrite> SnapshotDeferredWrites() {
    std::vector<DeferredWrite> writes;
    for (auto& queue : deferred_queues) {
        std::scoped_lock lock{queue.mutex};
        writes.insert(writes.end(), queue.writes.begin(), queue.writes.end());
        for (const auto& batch : queue.replaying) {
            writes.insert(writes.end(), batch->writes.begin(), batch->writes.end());
        }
    }
    return writes;
}
// A read of a GPU-tracked page whose texture cache lock is held answers "no download" without
// waiting, as an empty area the read does not remember, so the next read checks again. A first read
// of a newly GPU-written image can then miss its download: off by default (dev-settings
// cpu_read_skip=on). In Eglantine the ordered read runs GetFlushArea on the GPU thread, so the
// lock is held only by a guest write at that moment.
inline std::atomic<bool> cpu_read_skip{false};
inline std::atomic<unsigned long long> cpu_read_skipped{};
template <typename Mutex>
inline bool GuestCacheLockOrDefer(Mutex& mutex) {
    if (!cpu_write_defer.load(std::memory_order_relaxed) || deferred_replay) {
        GuestCacheLock(mutex);
        return true;
    }
    if (mutex.try_lock()) return true;
    cpu_write_deferred.fetch_add(1, std::memory_order_relaxed);
    return false;
}
// Guest waits: nvhost_ctrl syncpoint event registration -> signal, and
// BufferQueueProducer::DequeueBuffer waiting for a free buffer slot.
inline Totals guest_sync_wait, guest_dequeue_wait;
// fsp-srv IFile/IStorage reads (guest asset streaming): time in the backend and bytes.
inline Totals guest_fs_file, guest_fs_storage;
inline std::atomic<unsigned long long> guest_fs_file_bytes{}, guest_fs_storage_bytes{};
// Guest svcSendSyncRequest latency (queueing + HLE handling), and HLE handling time per
// service command (RecordHle, reported as EDEN_DEV_HLE).
inline Totals guest_ipc_wait;
inline void AddNs(Totals& totals, long long ns) {
    totals.nanoseconds.fetch_add(static_cast<unsigned long long>(ns), std::memory_order_relaxed);
    totals.calls.fetch_add(1, std::memory_order_relaxed);
}
// Which guest thread that wait belongs to. The sum alone cannot say whether a frame is waiting or
// whether background threads are: the same 13 ms spread over a dozen idle helpers costs nothing on
// the frame, while one thread holding it does. A fixed table keyed by the guest thread id - the
// first send from a thread claims a slot and takes its name once, every later one only adds. More
// distinct threads than slots fall into ipc_thread_overflow rather than displacing a claim.
inline constexpr unsigned ipc_thread_slots = 32;
inline Totals ipc_thread_overflow;
struct IpcThreadSlot {
    std::atomic<unsigned long long> tid{}, calls{}, ns{};
    char name[24]{};
};
inline std::array<IpcThreadSlot, ipc_thread_slots> ipc_threads{};
// Shape of the wait across every send, so a report can tell one blocking wait a frame from many
// short ones: below 50 us, below 500 us, below 5 ms, and 5 ms and over.
inline std::array<std::atomic<unsigned long long>, 4> ipc_hist{};
// Claims the slot for tid, probing past slots other threads hold. *fresh says this call claimed it,
// which is the only place a thread id is published - the caller then fills in the name.
inline unsigned IpcThreadSlotFor(unsigned long long tid, bool* fresh) {
    unsigned slot = static_cast<unsigned>(tid) % ipc_thread_slots;
    for (unsigned probe = 0; probe < ipc_thread_slots; ++probe) {
        const unsigned long long held = ipc_threads[slot].tid.load(std::memory_order_relaxed);
        if (held == tid) {
            *fresh = false;
            return slot;
        }
        if (held == 0) {
            unsigned long long expected = 0;
            if (ipc_threads[slot].tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) {
                *fresh = true;
                return slot;
            }
        }
        slot = (slot + 1) % ipc_thread_slots;
    }
    *fresh = false;
    return ipc_thread_slots;
}
inline void SetIpcThreadName(unsigned slot, const char* name) {
    if (slot >= ipc_thread_slots || name == nullptr) return;
    unsigned i = 0;
    while (name[i] != '\0' && i + 1 < sizeof ipc_threads[slot].name) {
        ipc_threads[slot].name[i] = name[i];
        ++i;
    }
    ipc_threads[slot].name[i] = '\0';
}
inline void AddIpcThread(unsigned slot, long long ns) {
    const unsigned long long n = static_cast<unsigned long long>(ns > 0 ? ns : 0);
    ipc_hist[n < 50000 ? 0 : n < 500000 ? 1 : n < 5000000 ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
    if (slot >= ipc_thread_slots) {
        AddNs(ipc_thread_overflow, static_cast<long long>(n));
        return;
    }
    ipc_threads[slot].ns.fetch_add(n, std::memory_order_relaxed);
    ipc_threads[slot].calls.fetch_add(1, std::memory_order_relaxed);
}
// Blocking time under ThreadWaitReasonForDebugging, stamped by KThread::BeginWait and closed by
// KThread::SetState - the single place the reason is cleared, so the reason read there is the one
// the wait ran under. A core with nothing to run and a thread asleep until a timeout are the same
// reading without this, and only one of them is ours to fix.
//
// Its own table, deliberately not the IPC one: every guest thread waits while only some send, so
// sharing let the threads parked since boot claim every slot and starve both at once - one run
// recorded 92% of its waits and none of its 93,788 sends. 512 covers a session's thread churn;
// wait_overflow_calls counts whatever still does not fit.
inline constexpr unsigned wait_thread_slots = 512;
inline std::atomic<unsigned long long> wait_overflow_calls{};
// Indexed by ThreadWaitReasonForDebugging (k_thread.h), whose values run 0 through 6.
inline constexpr unsigned wait_reason_slots = 8;
struct WaitThreadSlot {
    std::atomic<unsigned long long> tid{}, calls{}, ns{};
    std::atomic<long long> start{};
    // Who closed the wait. A thread woken by one fixed tid was waiting on an event, and one woken
    // by many different tids was queued behind whoever held its lock - both read as ConditionVar
    // in the reason, so this is what tells the two apart.
    std::atomic<unsigned long long> waker{};
    // The reason histogram below is every thread summed together, and condvar wins that on volume
    // alone. The reason for the one thread core0 sits idle behind is what names a fix, so each
    // slot keeps its own split rather than only adding to the shared total.
    std::array<std::atomic<unsigned long long>, wait_reason_slots> reason_ns{};
};
inline std::array<WaitThreadSlot, wait_thread_slots> wait_threads{};
inline std::array<std::atomic<unsigned long long>, wait_reason_slots> wait_reason_ns{};
inline std::array<std::atomic<unsigned long long>, wait_reason_slots> wait_reason_calls{};
inline unsigned WaitThreadSlotFor(unsigned long long tid) {
    unsigned slot = static_cast<unsigned>(tid) % wait_thread_slots;
    for (unsigned probe = 0; probe < wait_thread_slots; ++probe) {
        const unsigned long long held = wait_threads[slot].tid.load(std::memory_order_relaxed);
        if (held == tid) return slot;
        if (held == 0) {
            unsigned long long expected = 0;
            if (wait_threads[slot].tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed))
                return slot;
        }
        slot = (slot + 1) % wait_thread_slots;
    }
    return wait_thread_slots;
}
// NowNs comes from the caller: both hooks sit under the scheduler lock, and a second clock lookup
// per transition is the whole cost this diagnostic adds to the guest's own blocking.
inline void WaitBegin(unsigned long long tid, long long now_ns) {
    const unsigned slot = WaitThreadSlotFor(tid);
    if (slot >= wait_thread_slots) return;
    wait_threads[slot].start.store(now_ns, std::memory_order_relaxed);
}
inline void WaitEnd(unsigned long long tid, unsigned reason, long long now_ns,
                    unsigned long long waker_tid) {
    const unsigned slot = WaitThreadSlotFor(tid);
    if (slot >= wait_thread_slots) {
        // Counted here rather than in WaitBegin: a wait whose start did not fit ends without one,
        // so this is the single place that sees both halves of a dropped wait.
        wait_overflow_calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const long long start = wait_threads[slot].start.exchange(0, std::memory_order_relaxed);
    if (start == 0) return;
    const unsigned long long n = static_cast<unsigned long long>(now_ns > start ? now_ns - start : 0);
    if (tid == 86 && waker_tid < mkw_main_wakers.size()) {
        mkw_main_wakers[waker_tid].calls.fetch_add(1, std::memory_order_relaxed);
        mkw_main_wakers[waker_tid].nanoseconds.fetch_add(n, std::memory_order_relaxed);
    }
    if (tid >= 181 && tid <= 191 && waker_tid < 512) {
        auto& row = mkw_worker_wakers[tid - 181][waker_tid];
        row.calls.fetch_add(1, std::memory_order_relaxed);
        row.nanoseconds.fetch_add(n, std::memory_order_relaxed);
    }
    wait_threads[slot].ns.fetch_add(n, std::memory_order_relaxed);
    wait_threads[slot].calls.fetch_add(1, std::memory_order_relaxed);
    wait_threads[slot].waker.store(waker_tid, std::memory_order_relaxed);
    if (reason < wait_reason_slots) {
        wait_threads[slot].reason_ns[reason].fetch_add(n, std::memory_order_relaxed);
        wait_reason_ns[reason].fetch_add(n, std::memory_order_relaxed);
        wait_reason_calls[reason].fetch_add(1, std::memory_order_relaxed);
    }
}
inline long long NowNs() { return Common::g_wall_clock.GetTimeNS().count(); }
// Durations on hot paths (a draw passes a dozen marks and a Timer): the TSC read inline and
// unfenced, at the wall clock's rate. GetTimeNS is an out-of-line lfence; rdtsc; lfence (27 ns in a
// loop on the console, and each lfence waits for everything before it); unfenced, a read may move a
// few cycles, nothing over 5-second totals. Both ends of a duration come from here: only the rate
// matters.
inline long long DurationNs() {
#if defined(__x86_64__)
    static const unsigned long long factor = [] {
        if (!Common::g_wall_clock.IsNative()) return 0ULL;
        const auto ns = static_cast<unsigned long long>(NowNs());
        const unsigned long long ticks = __rdtsc();
        return ns < ticks ? static_cast<unsigned long long>((static_cast<unsigned __int128>(ns) << 64) / ticks) : 0ULL;
    }();
    if (factor) return static_cast<long long>((static_cast<unsigned __int128>(__rdtsc()) * factor) >> 64);
#endif
    return NowNs();
}
// Dispatch latency: how long a thread sat runnable before a host core entered RunThread on it.
// MainThread blocks ~419 us per handoff against a worker that is itself blocked, while cores 1
// and 2 are idle for most of the frame, so that gap is either the worker's own work or the time
// the scheduler takes to get it running - and only the second is ours to fix. SetState stamps
// the transition into Runnable; RunThread consumes and clears it, so one stamp buys one reading
// and the cost is one clock lookup per actual dispatch rather than per guest halt.
inline constexpr unsigned dispatch_slots = 512;
inline std::array<std::atomic<long long>, dispatch_slots> dispatch_since{};
inline std::array<std::atomic<unsigned long long>, dispatch_slots> dispatch_ns{};
inline std::array<std::atomic<unsigned long long>, dispatch_slots> dispatch_calls{};
inline std::atomic<unsigned long long> dispatch_overflow_calls{};
inline void NoteRunnable(unsigned long long tid, long long now_ns) {
    if (tid >= dispatch_slots) {
        dispatch_overflow_calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    dispatch_since[tid].store(now_ns, std::memory_order_relaxed);
}
inline void NoteRunning(unsigned long long tid, long long now_ns) {
    if (tid >= dispatch_slots) return;
    const long long since = dispatch_since[tid].exchange(0, std::memory_order_relaxed);
    if (since == 0) return;
    dispatch_ns[tid].fetch_add(
        static_cast<unsigned long long>(now_ns > since ? now_ns - since : 0),
        std::memory_order_relaxed);
    dispatch_calls[tid].fetch_add(1, std::memory_order_relaxed);
}
void RecordHle(const char* service, unsigned command, long long ns);
// Texture cache garbage collection (tools/prepare-vulkan-port.py, vulkan_gc_downloads.inc).
// Past its "expected" memory use Eden's collector also evicts images the GPU wrote, and each of
// those is first copied back to guest memory behind a wait for the GPU. On the console a large
// open-world game sat just past that mark (4.8 GiB against 4.5 with the FSR filter's images):
// entering gameplay took ten seconds at 3-9 FPS (15 collector runs took 4.6 s of one 5 s window)
// and frames of 70-115 ms kept coming. Those images are now kept, as they are below the mark,
// until memory is really short: use reaches the "critical" mark, or the largest free block of
// direct memory (what the next allocation needs) is under kShortMemory.
// dev-settings gc_dirty=upstream restores Eden's rule.
inline std::atomic<bool> gc_keep_dirty{true};
// The collector's per-run budget counts evictions, not visits (tools/
// prepare-vulkan-port.py): skipped images used to burn it and stop the scan,
// so kept images shielded everything behind them. dev-settings gc_scan=upstream
// restores Eden's visit counting.
inline std::atomic<bool> gc_count_visits{false};
// GPU thread only: the collector's pass over costly images long unused while the pool is short
// (tools/prepare-vulkan-port.py), which may take them.
inline bool gc_old_costly = false;
inline std::atomic<bool> graphics_memory_short{false};
// 1 GiB: the pool is the CPU's too, and the largest thing a game asked of it at once was a block of
// 486 MiB (a texture array of 121 layers, decoded before it is uploaded). With less here the collector
// let the pool run down to 620 MiB in one block, and that request ended the session.
inline constexpr std::size_t kShortMemory = std::size_t{1} << 30;
// The guest's headroom in the pool: 1 GiB that a game's own memory may take at once while the graphics
// caches give memory back. The memory service holds it as blocks of its own (src/guest_memory_service.inc).
inline constexpr std::size_t kGuestReserve = std::size_t{1} << 30;
// What the caches see of the pool (performance.cpp, RefreshFreeMemory): its largest free block less the part
// of the guest's headroom the service does not hold yet (all of it when no service holds any), and memory
// short when that is under kShortMemory or a burst of the game's writes has taken half the headroom, so the
// caches start giving memory back before a write has to wait for them. `known`: the pool could be read;
// `held`, `target`: the service's reserve now and in full (bytes, both 0 without).
struct PoolView {
    std::size_t free;
    bool short_memory;
};
inline PoolView ViewOfPool(bool known, std::size_t largest, std::size_t held, std::size_t target) {
    if (!known) return {0, true};
    const std::size_t headroom = target == 0 ? kGuestReserve : held < target ? target - held : 0;
    const std::size_t free = largest > headroom ? largest - headroom : 0;
    return {free, free < kShortMemory || (target != 0 && held < target / 2)};
}
// Development: the texture cache reports its memory use and marks every 300 frames.
inline std::atomic<bool> texture_budget_log{false};
// "Is memory short?", installed by the PS5 build (performance.cpp, GraphicsMemoryShort). This
// header is compiled into libraries with and without PS5_NATIVE, so the function below must read
// the same in all of them: the platform part is behind this pointer, not behind an #ifdef.
inline std::atomic<bool (*)()> graphics_memory_probe{nullptr};
// The kernel's count of the guest's mapped physical memory, installed while a game runs (main.cpp):
// the memory report tells it from the slots the sparse backing committed.
inline std::atomic<unsigned long long (*)()> guest_kernel_memory{nullptr};
// Per guest-kernel pool: what the kernel hands out and the host memory behind the pool (main.cpp).
inline std::atomic<void (*)()> guest_pool_report{nullptr};
// Graphics memory still free: the largest block of the direct memory pool, which is what the
// driver's next allocation can be given. Installed by the PS5 build (performance.cpp).
//
// The texture and buffer caches decide what to evict from "memory in use" against a budget. On
// the console that figure is the budget less this free block (tools/prepare-vulkan-port.py,
// Device::GetDeviceMemoryUsage), not the driver's own count, for two reasons. The CPU and the
// GPU share one pool, so what limits graphics is what is left of it, whoever took the rest; and
// the driver reports its allocations twice over (as VRAM and as visible VRAM), which made the
// caches evict at half the use they were set to. dev-settings graphics_usage=driver restores
// the driver's count.
inline std::atomic<unsigned long long (*)()> graphics_memory_free{nullptr};
inline std::atomic<bool> graphics_usage_from_pool{true};
// The pool is short now (the probe above): the texture collector then collects as if past its
// first mark. Switch 2 titles set the marks near the driver's device memory, which on the console
// is the pool the guest's memory also takes, so use stayed under them while the pool ran out
// (Mario Kart World: 5.4 GB of textures against a 5.5 GB mark, then no block for 42 MiB).
inline bool GraphicsMemoryIsShort() {
    const auto probe = graphics_memory_probe.load(std::memory_order_relaxed);
    return probe && probe();
}
// Under this much free memory (graphics_memory_free: the largest free block less the guest's
// headroom; 1 while a guest write waits for a block) the pool is critical: the collector then
// takes what 10 frames left unused, costly images included only past its own critical mark.
// Short but not critical, it takes what 600 frames left unused (EDEN_NX2_TC_SHORT_AGE): taking
// everything 10 frames left unused recreated 1,000-2,400 images per 600 frames in Mario Kart
// World's races. Half of kShortMemory, as ProsperoEden's 1 GiB is half of its 2 GiB short mark.
inline constexpr std::size_t kCriticalMemory = kShortMemory / 2;
inline bool GraphicsMemoryIsCritical() {
    const auto free = graphics_memory_free.load(std::memory_order_relaxed);
    return free && free() < kCriticalMemory;
}
// GPU thread only (the collector).
inline bool KeepDirtyTextures() {
    if (!gc_keep_dirty.load(std::memory_order_relaxed)) return false;
    const auto probe = graphics_memory_probe.load(std::memory_order_relaxed);
    return !probe || !probe();
}
// Development boot trace (dev-settings boot_trace=START:END, milliseconds after the settings
// are read): guest GPU submissions and GPU-thread dispatches inside it are logged one by one.
inline std::atomic<long long> boot_trace_begin{0}, boot_trace_end{0};
// dev-settings fs_callers=on: log the guest caller of each IFileSystem request (its backtrace
// walk delays that request by ~0.1 s, so it is off unless asked for).
inline std::atomic<bool> trace_fs_callers{false};
// Maxwell render-enable evaluations (Maxwell3D::ProcessQueryCondition): [0] host conditional
// rendering, [1]/[2] always/never overrides, [3 + mode * 2 + result] per render_enable mode
// (False, True, Conditional, IfEqual, IfNotEqual) and the CPU-evaluated result. With
// VK_EXT_conditional_rendering, the host path's decisions (tools/prepare-vulkan-port.py): [13] CPU
// evaluation, [14] drawn unconditionally, [15] predicate read from one value, [16] compute
// compare, [17]/[18] hcr=cpu constant predicate draw/skip, [19] hcr=exact pending-query fallback.
inline std::array<std::atomic<unsigned long long>, 20> render_conditions{};
// Guest core idle waits (PhysicalCore::Idle): calls, nanoseconds until the
// interrupt, and calls that went to sleep because nothing arrived while spinning.
struct IdleCounters {
    std::atomic<unsigned long long> calls{0}, nanoseconds{0}, sleeps{0};
    // Wake-up latency: from the interrupt that woke a sleeping core to its resumption.
    std::atomic<unsigned long long> wakes{0}, wake_ns{0}, wake_max_ns{0};
};
inline std::array<IdleCounters, 6> guest_idle{};
// Frame pacing (backported from ProsperoEden; see also guest_clock.h).
// PAUSE iterations a guest core spins on its interrupt flag before sleeping (~20 ns each on the
// console). Guest job systems hand work between cores thousands of times a second; sleeping on a
// handoff puts the host's thread wake-up latency on the critical path. 100 us took a racing game's
// heavy phase from ~55 to ~59.6 FPS; 500 us (the default) cut a tested game's requests for
// 30 FPS from 23% to 15% of frames (sleeps per core 800 -> 330 a second). 2 ms cut them to 12% but
// made some effects flicker. dev-settings idle_spin_us=N overrides it (0 sleeps at once).
inline std::atomic<unsigned> idle_spin_iterations{25000};
// Signal the guest vsync first and compose 1.5 ms later, so a frame the game queues in response
// to the vsync is latched at that same vsync (headless/CMakeLists.txt, VI conductor; multicore
// only). dev-settings compose_delay_us=N overrides it (0 = upstream order).
inline std::atomic<int> compose_delay_us{1500};
// Draws between the Vulkan rasterizer's hand-offs to its worker, minus one (a power of two minus
// one; tools/prepare-vulkan-port.py). Upstream hands off every 8 draws; dev-settings
// dispatch_draws=N (8 to 512) overrides the 64 used here.
inline std::atomic<unsigned> dispatch_mask{63};
inline void CountWake(std::size_t core, long long nanoseconds) {
    if (core >= guest_idle.size() || nanoseconds < 0) return;
    auto& counters = guest_idle[core];
    counters.wakes.fetch_add(1, std::memory_order_relaxed);
    counters.wake_ns.fetch_add(static_cast<unsigned long long>(nanoseconds), std::memory_order_relaxed);
    unsigned long long seen = counters.wake_max_ns.load(std::memory_order_relaxed);
    while (static_cast<unsigned long long>(nanoseconds) > seen &&
           !counters.wake_max_ns.compare_exchange_weak(seen, static_cast<unsigned long long>(nanoseconds),
                                                       std::memory_order_relaxed)) {
    }
}
inline void CountIdle(std::size_t core, long long nanoseconds, bool slept) {
    if (core >= guest_idle.size()) return;
    guest_idle[core].calls.fetch_add(1, std::memory_order_relaxed);
    guest_idle[core].nanoseconds.fetch_add(static_cast<unsigned long long>(nanoseconds), std::memory_order_relaxed);
    if (slept) guest_idle[core].sleeps.fetch_add(1, std::memory_order_relaxed);
}
// Development: reads 32-bit guest words for code dumps (installed by the frontend per session).
inline bool (*guest_read32)(unsigned long long address, unsigned& value) = nullptr;
inline void CountCondition(unsigned slot) {
    if (slot < render_conditions.size()) render_conditions[slot].fetch_add(1, std::memory_order_relaxed);
}
inline bool BootTrace() {
    const auto end = boot_trace_end.load(std::memory_order_relaxed);
    if (end == 0) return false;
    const auto now = NowNs();
    return now >= boot_trace_begin.load(std::memory_order_relaxed) && now < end;
}
inline void AddSince(Totals& totals, long long start) {
    totals.nanoseconds.fetch_add(static_cast<unsigned long long>(NowNs() - start), std::memory_order_relaxed);
    totals.calls.fetch_add(1, std::memory_order_relaxed);
}
// A32/A64 memory accesses that left JIT code through the C++ callback (tracked,
// unmapped or misaligned pages, exclusive stores). Written only by the owning core.
struct alignas(64) JitCallbacks {
    std::atomic<unsigned long long> reads{}, writes{}, exclusive_writes{};
};
inline std::array<JitCallbacks, 6> jit_callbacks{};
inline void CountJit(std::atomic<unsigned long long>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
}
// GPU thread only (Vulkan frame report): cumulative GPU-thread idle/dispatch/wait
// totals with the owner's CPU clock, then a guest-core CPU snapshot.
void ReportGpuThread(unsigned frame);
void ReportMemoryPressure(unsigned seconds);
// The heap's line in stderr.log: what it holds of the pool, what of that its blocks use, what it has given back.
void ReportHeap(const char* why);
// Gives the heap's empty spaces back to the pool; the bytes returned.
std::size_t TrimHeap(const char* why);
inline std::atomic<unsigned> capture_passes{};

// A Timer counts only when timers are on: in a development build (EDEN_DEV_PROFILE), a Mario Kart
// diagnostics build (EDEN_MKW_DIAGNOSTICS) and a cost run (cost-run.txt), all set when a game
// starts (headless/main.cpp). Only those builds' reports read the totals, and timers sit on hot
// paths: one a draw on the GPU thread, one for every guest write of memory the GPU caches follow
// (several thousand of each a frame in a large game). Elsewhere a Timer is one look at this flag.
// `always` is for a timer a report of every build reads.
inline std::atomic<bool> timers_on{false};
class Timer {
public:
    explicit Timer(Totals& target, unsigned long long bytes = 0, bool always = false)
        : totals(target), requested_bytes(bytes),
          on(always || timers_on.load(std::memory_order_relaxed)),
          start(on ? DurationNs() : 0) {}
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    ~Timer() {
        if (!on) return;
        const auto elapsed = DurationNs() - start;
        totals.nanoseconds.fetch_add(static_cast<unsigned long long>(elapsed), std::memory_order_relaxed);
        if (requested_bytes) totals.requested_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
        totals.calls.fetch_add(1, std::memory_order_relaxed);
    }
private:
    Totals& totals;
    unsigned long long requested_bytes;
    bool on;
    long long start;
};

// Accumulate one phase of a sequence under a single running clock instead of giving it a Timer:
// one clock read and two relaxed adds. `since` is where the phase started and advances to now, so
// consecutive marks measure the gap between them and together cover the whole sequence without
// overlap. An early return out of the sequence leaves the tail unmarked - that is what the outer
// Timer is still charging - and the phase that never ran simply does not call this.
//
// Only a development build counts them (draw_marks, set when a game starts there). A draw
// passes about twenty marks, a clock read and two additions each, and only a development
// run's report reads the totals: elsewhere a mark is one look at this flag, and the clock it
// would measure from is not read either (MarkStart). A flag and not an #ifdef: this header
// must read the same in every library that includes it.
inline std::atomic<bool> draw_marks{false};
inline long long MarkStart() { return draw_marks.load(std::memory_order_relaxed) ? DurationNs() : 0; }
inline void Mark(Totals& totals, long long& since) {
    if (!draw_marks.load(std::memory_order_relaxed)) return;
    const auto now = DurationNs();
    const auto elapsed = now - since;
    since = now;
    totals.nanoseconds.fetch_add(static_cast<unsigned long long>(elapsed), std::memory_order_relaxed);
    totals.calls.fetch_add(1, std::memory_order_relaxed);
}

// Opt-in API wall times: calls may overlap across threads, so do not sum them as frame time.
inline std::atomic<bool> vulkan_cost_enabled{false};
inline std::array<Totals, 24> vulkan_api;
// Large textures upload only the block rows guest writes changed (texture_partial.h): dev-settings
// tex_partial=off, tex_partial_kb=N (smallest image, default 1 MiB), tex_partial_check=on (compare
// each partial upload with a whole unswizzle). Counted: partial uploads, their bytes, checks, mismatches.
inline std::atomic<bool> tex_partial{true}, tex_partial_check{false};
inline std::atomic<unsigned long long> tex_partial_min{1ULL << 20};
inline std::atomic<unsigned long long> tex_partial_uploads{}, tex_partial_bytes{}, tex_partial_checks{},
    tex_partial_mismatches{};
inline std::optional<Timer> VulkanTimer(unsigned index) {
    if (vulkan_cost_enabled.load(std::memory_order_relaxed))
        return std::optional<Timer>{std::in_place, vulkan_api.at(index)};
    return std::nullopt;
}
inline void ReportVulkan() {
    if (!vulkan_cost_enabled.load(std::memory_order_relaxed)) return;
    constexpr const char* names[]{"graphics_pipeline", "compute_pipeline", "submit",
                                  "fence_wait", "semaphore_wait", "device_idle",
                                  "guest_fence", "buffer_sync", "texture_sync", "present_sync",
                                  "descriptor_sync", "range_reuse", "astc_submit",
                                  "texture_gc", "texture_cpu_download", "texture_async_release",
                                  "pipeline_ready_wait", "shader_prepare", "pipeline_cache_save",
                                  "shader_pool_reset", "shader_cfg", "shader_ir",
                                  "shader_spirv", "shader_module"};
    static_assert(std::size(names) == vulkan_api.size());
    for (unsigned i = 0; i < vulkan_api.size(); ++i)
        std::printf("EDEN_VULKAN_COST api=%s calls=%llu ns=%llu\n", names[i],
                    vulkan_api[i].calls.load(std::memory_order_relaxed),
                    vulkan_api[i].nanoseconds.load(std::memory_order_relaxed));
}
// Guest run time per guest thread since the last report (mkw_guest_run, the JIT wrapper's timer;
// not in the development profile build): the twelve busiest as tid=ms/runs. A thread near the
// report interval's length is CPU-bound in guest code.
inline void ReportGuestRun() {
    static std::array<unsigned long long, 512> last_ns{}, last_calls{};
    struct Row {
        unsigned tid;
        unsigned long long ns, calls;
    };
    std::array<Row, 512> rows{};
    size_t count = 0;
    for (unsigned tid = 0; tid < mkw_guest_run.size(); ++tid) {
        const auto ns = mkw_guest_run[tid].nanoseconds.load(std::memory_order_relaxed);
        const auto calls = mkw_guest_run[tid].calls.load(std::memory_order_relaxed);
        const auto delta = ns - last_ns[tid];
        const auto delta_calls = calls - last_calls[tid];
        last_ns[tid] = ns;
        last_calls[tid] = calls;
        if (delta != 0) rows[count++] = {tid, delta, delta_calls};
    }
    std::sort(rows.begin(), rows.begin() + count,
              [](const Row& a, const Row& b) { return a.ns > b.ns; });
    std::printf("EDEN_GUEST_RUN");
    for (size_t i = 0; i < count && i < 12; ++i)
        std::printf(" %u=%llu/%llu", rows[i].tid, rows[i].ns / 1000000ULL, rows[i].calls);
    std::printf("\n");
    // Per guest core since the last report: idles, of them sleeps, average and worst wake-up
    // latency of the sleeps an interrupt ended (microseconds).
    static std::array<IdleCounters, 6> last_idle{};
    std::printf("EDEN_GUEST_WAKE");
    for (std::size_t core = 0; core < guest_idle.size(); ++core) {
        auto& now = guest_idle[core];
        auto& last = last_idle[core];
        const auto calls = now.calls.load(std::memory_order_relaxed) - last.calls.load(std::memory_order_relaxed);
        const auto sleeps = now.sleeps.load(std::memory_order_relaxed) - last.sleeps.load(std::memory_order_relaxed);
        const auto wakes = now.wakes.load(std::memory_order_relaxed) - last.wakes.load(std::memory_order_relaxed);
        const auto wake_ns = now.wake_ns.load(std::memory_order_relaxed) - last.wake_ns.load(std::memory_order_relaxed);
        const auto max_ns = now.wake_max_ns.exchange(0, std::memory_order_relaxed);
        std::printf(" c%zu=%llu/%llu/%llu/%llu", core, calls, sleeps, wakes ? wake_ns / wakes / 1000ULL : 0ULL,
                    max_ns / 1000ULL);
        last.calls.store(now.calls.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last.sleeps.store(now.sleeps.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last.wakes.store(now.wakes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        last.wake_ns.store(now.wake_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    std::printf("\n");
}
// Call only before guest startup and after worker shutdown, respectively.
inline void Reset() {
    for (auto& entry : compilation) {
        entry.calls = 0;
        entry.nanoseconds = 0;
        entry.requested_bytes = 0;
    }
    for (auto& count : evacuations) count = 0;
    storage.calls = 0;
    storage.nanoseconds = 0;
    storage.requested_bytes = 0;
    jit_protection.calls = 0;
    jit_protection.nanoseconds = 0;
    jit_protection.requested_bytes = 0;
}
inline void Report() {
    for (unsigned core = 0; core < compilation.size(); ++core) {
        std::printf("EDEN_PERF_JIT core=%u compilations=%llu compile_ns=%llu evacuations=%llu\n",
                    core, compilation[core].calls.load(), compilation[core].nanoseconds.load(),
                    evacuations[core].load());

    }
    std::printf("EDEN_PERF_STORAGE calls=%llu read_ns=%llu requested_bytes=%llu\n",
                storage.calls.load(), storage.nanoseconds.load(), storage.requested_bytes.load());
    std::printf("EDEN_PERF_JIT_PROTECTION calls=%llu elapsed_ns=%llu requested_bytes=%llu\n",
                jit_protection.calls.load(), jit_protection.nanoseconds.load(), jit_protection.requested_bytes.load());
}
}
