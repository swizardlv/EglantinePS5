// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};
std::atomic<bool> g_back_busy{};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::fprintf(stderr, "GPU split owner check: %s on thread %lu while the back runs\n", where,
                 thread);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
    std::fflush(stderr);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const bool blocks = [] {
        const char* value = std::getenv("EDEN_NX2_REG_BLOCKS");
        return value == nullptr || value[0] != '0';
    }();
    reg_blocks = blocks;
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    regs_entry = nullptr;
    regs_count = 0;
    write_position = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_releases.clear();
    front_pending.clear();
    front_lower_bounds.clear();
    front_adds.clear();
    front_reports.clear();
    reported_words.clear();
    sync_pending = false;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            return;
        }
        if (spins < 4096) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::Publish() {
    CloseRegs();
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

std::optional<u32> GpuSplit::PendingLowerBound(u64 key, u32 memory) {
    const u64 done = consumed.load(std::memory_order_acquire);
    std::optional<u64> bound;
    if (const auto it = front_adds.find(key); it != front_adds.end()) {
        auto& adds = it->second;
        std::erase_if(adds, [done](const FrontRelease& add) { return done >= add.position; });
        u64 sum = 0;
        for (const auto& add : adds) sum += add.value;
        if (adds.empty()) {
            front_adds.erase(it);
        } else if (u64{memory} + sum <= 0xffffffffULL) {
            bound = u64{memory} + sum;
        }
    }
    if (const auto it = front_lower_bounds.find(key); it != front_lower_bounds.end()) {
        if (done >= it->second.position) {
            front_lower_bounds.erase(it);
        } else {
            bound = (std::max)(bound.value_or(0), u64{(std::max)(memory, it->second.value)});
        }
    }
    if (front_adds.size() > 4096 || front_lower_bounds.size() > 4096) {
        std::erase_if(front_lower_bounds, [done](const auto& entry) { return done >= entry.second.position; });
        std::erase_if(front_adds, [done](auto& entry) {
            std::erase_if(entry.second, [done](const FrontRelease& add) { return done >= add.position; });
            return entry.second.empty();
        });
    }
    return bound ? std::optional<u32>{static_cast<u32>(*bound)} : std::nullopt;
}

std::optional<u64> GpuSplit::PendingPosition(u64 key) {
    const u64 done = consumed.load(std::memory_order_acquire);
    if (front_pending.size() > 4096) {
        std::erase_if(front_pending, [done](const auto& entry) { return done >= entry.second; });
    }
    const auto it = front_pending.find(key);
    if (it == front_pending.end()) {
        return std::nullopt;
    }
    if (done >= it->second) {
        front_pending.erase(it);
        return std::nullopt;
    }
    return it->second;
}

std::optional<u32> GpuSplit::PendingRelease(u64 key) {
    const u64 done = consumed.load(std::memory_order_acquire);
    if (front_releases.size() > 4096) {
        std::erase_if(front_releases, [done](const auto& entry) { return done >= entry.second.position; });
    }
    const auto it = front_releases.find(key);
    if (it == front_releases.end()) {
        return std::nullopt;
    }
    if (done >= it->second.position) {
        front_releases.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

bool GpuSplit::WritePending(u64 key) {
    const u64 done = consumed.load(std::memory_order_acquire);
    if (front_reports.size() > 4096) {
        std::erase_if(front_reports, [done](const auto& entry) { return done >= entry.second; });
    }
    if (const auto it = front_reports.find(key); it != front_reports.end()) {
        if (done < it->second) {
            return true;
        }
        front_reports.erase(it);
    }
    return PendingRelease(key) || PendingPosition(key) || PendingLowerBound(key, 0);
}

void GpuSplit::Drain() {
    sync_pending = false;
    CloseRegs();
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            publish_every = 8;
            return;
        }
        if (spins < 16384) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::DrainToLastWriter() {
    // Waiting only up to the last compute, copy or upload method crashed Mario Kart World
    // while loading on the console (a list read before its writer ran, then a clear with a
    // garbage render target): something the front does not count as a writer fills them. Off
    // unless EDEN_NX2_LIST_DRAIN_TO_WRITER=1; the default waits for everything sent.
    static const bool to_writer = [] {
        const char* value = std::getenv("EDEN_NX2_LIST_DRAIN_TO_WRITER");
        return value != nullptr && value[0] == '1';
    }();
    if (!to_writer) {
        Drain();
        return;
    }
    DrainTo(last_writer_position);
}

void GpuSplit::DrainTo(u64 position) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= position) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= position) {
            return;
        }
        if (spins < 16384) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Regs: {
        const u32 count = static_cast<u32>((head >> 8) & 0xff);
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        for (u32 i = 0; i < count; ++i) {
            engine->method_sink.emplace_back(static_cast<u32>(entry[1 + i]), static_cast<u32>(entry[1 + i] >> 32));
        }
        engine->ConsumeSink(*system);
        return 1 + count;
    }
    case Kind::CbData: {
        const u32 count = static_cast<u32>((head >> 8) & 0xff);
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        static_cast<Tegra::Engines::Maxwell3D*>(engine)->ProcessCBDataBlock(entry + 1, count);
        return 1 + count;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    back_thread_context = true;
    Common::SetCurrentThreadName("GPUBack");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position; ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                if (available == read_position && !stop_token.stop_requested()) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read_position;
        g_back_busy.store(true, std::memory_order_relaxed);
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            Tegra::g_nx2_back_position.store(read_position, std::memory_order_relaxed);
            const size_t size = Execute(&ring[index]);
            read_position += size != 0 ? size : RingWords - index;
            // Hand ring space back now and then while working through a long batch.
            // Often enough for the front's partial waits (DrainTo a pending release) too.
            if (read_position - last_store >= 256 && read_position != available) {
                consumed.store(read_position, std::memory_order_release);
                last_store = read_position;
            }
        }
        g_back_busy.store(false, std::memory_order_release);
        consumed.store(read_position, std::memory_order_release);
    }
}

} // namespace VideoCommon
