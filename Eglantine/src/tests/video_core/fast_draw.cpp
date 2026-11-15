// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <functional>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/rasterizer_interface.h"

namespace {

using Tegra::Engines::Maxwell3D;
namespace FastDraw = VideoCommon::Nx2FastDraw;
constexpr u32 ViewportScaleX = MAXWELL3D_REG_INDEX(viewport_transform);

class NoDma final : public Tegra::Engines::AccelerateDMAInterface {
public:
    bool BufferCopy(GPUVAddr, GPUVAddr, u64) override { return true; }
    bool BufferClear(GPUVAddr, u64, u32) override { return true; }
    bool ImageToBuffer(const Tegra::DMA::ImageCopy&, const Tegra::DMA::ImageOperand&,
                       const Tegra::DMA::BufferOperand&) override { return true; }
    bool BufferToImage(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                       const Tegra::DMA::ImageOperand&) override { return true; }
};

/// Records the fast draw serial each draw sees.
class SerialRasterizer final : public VideoCore::RasterizerInterface {
public:
    std::vector<u64> draws;
    std::function<void(bool, u32)> on_draw;
    std::function<void(VideoCommon::QueryType)> on_reset;

    void Draw(bool indexed, u32 instances) override {
        draws.push_back(FastDraw::Serial());
        if (on_draw) {
            on_draw(indexed, instances);
        }
    }
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType type) override {
        if (on_reset) {
            on_reset(type);
        }
    }
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& func) override { func(); }
    void SyncOperation(std::function<void()>&& func) override { func(); }
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override { return false; }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override { return false; }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override { return dma; }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

private:
    NoDma dma;
};

struct FastDrawFixture {
    FastDrawFixture() : memory{system, device_memory, 32, 1ULL << 31}, maxwell3d{memory} {
        SetSwitch(true);
        FastDraw::g_delta = {};
        maxwell3d.BindRasterizer(&rasterizer);
    }
    ~FastDrawFixture() {
        FastDraw::SetEnabledForTest(previous);
        FastDraw::g_delta = {};
    }

    static void SetSwitch(bool on) {
        FastDraw::SetEnabledForTest(on);
    }

    u32 Reg(u32 method) const {
        return maxwell3d.regs.reg_array[method];
    }

    void Write(u32 method, u32 value) {
        maxwell3d.CallMethod(system, method, value, true);
    }

    void DrawArrays(u32 first, u32 count) {
        Write(MAXWELL3D_REG_INDEX(vertex_buffer.first), first);
        Write(MAXWELL3D_REG_INDEX(vertex_buffer.count), count);
        Write(MAXWELL3D_REG_INDEX(draw.begin), 0x4); // first instance, triangles
        Write(MAXWELL3D_REG_INDEX(draw.end), 0);
    }

    /// Whether the last two draws saw the same serial (the second could reuse the first).
    bool Reusable() const {
        const auto& draws = rasterizer.draws;
        REQUIRE(draws.size() >= 2);
        return draws[draws.size() - 1] == draws[draws.size() - 2];
    }

    const bool previous = FastDraw::Enabled();
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory;
    SerialRasterizer rasterizer;
    Maxwell3D maxwell3d;
};

} // namespace

TEST_CASE("Fast draw keeps the serial across draws that change only draw parameters",
          "[fast_draw]") {
    FastDrawFixture f;
    f.DrawArrays(0, 3);
    f.DrawArrays(3, 6);
    REQUIRE(f.rasterizer.draws.size() == 2);
    CHECK(f.Reusable());

    f.Write(ViewportScaleX, f.Reg(ViewportScaleX));
    f.DrawArrays(9, 3);
    CHECK(f.Reusable());

    f.Write(MAXWELL3D_REG_INDEX(const_buffer.offset), 0x40);
    f.Write(MAXWELL3D_REG_INDEX(const_buffer.address_low), 0x1000);
    f.DrawArrays(12, 3);
    CHECK(f.Reusable());
}

TEST_CASE("Fast draw breaks reuse on anything that changes what a draw binds", "[fast_draw]") {
    FastDrawFixture f;
    const auto breaks = [&](auto&& change) {
        f.DrawArrays(0, 3);
        change();
        f.DrawArrays(0, 3);
        return !f.Reusable();
    };
    constexpr u32 blend = MAXWELL3D_REG_INDEX(blend.enable[0]);
    CHECK(breaks([&] { f.Write(blend, f.Reg(blend) ^ 1); }));
    CHECK(breaks([&] {
        f.Write(ViewportScaleX,
                f.Reg(ViewportScaleX) + 1);
    }));
    CHECK(breaks([&] { f.Write(MAXWELL3D_REG_INDEX(clear_surface), 0x3c); }));
    CHECK(breaks([&] { f.Write(MAXWELL3D_REG_INDEX(tiled_cache_barrier), 0); }));
    CHECK(breaks([&] { FastDraw::Break(FastDraw::Reason::Fence); }));
}

TEST_CASE("Fast draw serial never moves while disabled", "[fast_draw]") {
    FastDrawFixture f;
    FastDrawFixture::SetSwitch(false);
    f.DrawArrays(0, 3);
    f.Write(MAXWELL3D_REG_INDEX(blend.enable[0]), f.Reg(MAXWELL3D_REG_INDEX(blend.enable[0])) ^ 1);
    f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), 1);
    f.DrawArrays(0, 3);
    CHECK(f.Reusable());
}

namespace {

struct DeltaFixture : FastDrawFixture {
    void SetConstantBuffer(GPUVAddr address, u32 size, u32 offset) {
        Write(MAXWELL3D_REG_INDEX(const_buffer.size), size);
        Write(MAXWELL3D_REG_INDEX(const_buffer.address_high), static_cast<u32>(address >> 32));
        Write(MAXWELL3D_REG_INDEX(const_buffer.address_low), static_cast<u32>(address));
        Write(MAXWELL3D_REG_INDEX(const_buffer.offset), offset);
    }
};

} // namespace

TEST_CASE("Fast draw records bind and constant data changes, keeps index buffer changes light",
          "[fast_draw]") {
    DeltaFixture f;
    f.DrawArrays(0, 3);
    const u64 serial = FastDraw::Serial();
    FastDraw::g_delta = {};

    f.Write(MAXWELL3D_REG_INDEX(index_buffer.start_addr_low),
            f.Reg(MAXWELL3D_REG_INDEX(index_buffer.start_addr_low)) + 0x100);
    f.Write(MAXWELL3D_REG_INDEX(index_buffer.format), f.Reg(MAXWELL3D_REG_INDEX(index_buffer.format)) ^ 1);
    CHECK(FastDraw::Serial() == serial);
    CHECK(FastDraw::g_delta.Empty());

    f.SetConstantBuffer(0x10000, 0x100, 0);
    f.Write(MAXWELL3D_REG_INDEX(bind_groups[4].raw_config), 0x31); // slot 3, valid
    CHECK(FastDraw::g_delta.binds[4] == 1U << 3);
    FastDraw::g_delta.binds[4] = 0;
    f.Write(MAXWELL3D_REG_INDEX(bind_groups[4].raw_config), 0x31);
    CHECK(FastDraw::g_delta.binds[4] == 0);

    f.SetConstantBuffer(0x20000, 0x100, 0x10);
    f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), 1);
    f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1, 2);
    REQUIRE(FastDraw::g_delta.num_writes == 1);
    CHECK(FastDraw::g_delta.write_begin[0] == 0x20010);
    CHECK(FastDraw::g_delta.write_end[0] == 0x20018);

    CHECK(FastDraw::Serial() == serial);
    f.DrawArrays(0, 3);
    CHECK(f.Reusable());
    CHECK_FALSE(FastDraw::g_delta.Empty());
}

TEST_CASE("Fast draw breaks on state, texture headers and delta overflow",
          "[fast_draw]") {
    DeltaFixture f;
    f.Write(MAXWELL3D_REG_INDEX(tex_header.address_high), 0);
    f.Write(MAXWELL3D_REG_INDEX(tex_header.address_low), 0x40000);
    f.Write(MAXWELL3D_REG_INDEX(tex_header.limit), 7);
    f.Write(MAXWELL3D_REG_INDEX(tex_sampler.address_high), 0);
    f.Write(MAXWELL3D_REG_INDEX(tex_sampler.address_low), 0x50000);
    f.Write(MAXWELL3D_REG_INDEX(tex_sampler.limit), 7);
    const auto breaks = [&](auto&& change) {
        FastDraw::g_delta = {};
        const u64 serial = FastDraw::Serial();
        change();
        return FastDraw::Serial() != serial;
    };
    constexpr u32 blend = MAXWELL3D_REG_INDEX(blend.enable[0]);
    CHECK(breaks([&] { f.Write(blend, f.Reg(blend) ^ 1); }));
    CHECK(breaks([&] {
        f.SetConstantBuffer(0x40000, 0x1000, 0xe0); // the last texture header
        f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), 1);
    }));
    CHECK(breaks([&] {
        f.SetConstantBuffer(0x50000, 0x1000, 0);
        f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), 1);
    }));
    CHECK_FALSE(breaks([&] {
        f.SetConstantBuffer(0x40000, 0x1000, 0x100); // just past the headers
        f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), 1);
    }));
    CHECK(breaks([&] {
        f.SetConstantBuffer(0x60000, 0x1000, 0);
        for (u32 i = 0; i <= FastDraw::Delta::MaxWrites; ++i) {
            f.Write(MAXWELL3D_REG_INDEX(const_buffer.offset), i * 0x20);
            f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), i);
        }
    }));
}

TEST_CASE("Fast draw breaks on texture header data written through another mapping",
          "[fast_draw]") {
    DeltaFixture f;
    f.memory.BindRasterizer(&f.rasterizer);
    constexpr GPUVAddr headers = 0x40000;
    constexpr GPUVAddr alias = 0x90000;
    f.memory.Map(headers, 0x100000, 0x1000, Tegra::PTEKind::PITCH, false);
    f.memory.Map(alias, 0x100000, 0x1000, Tegra::PTEKind::PITCH, false);
    f.Write(MAXWELL3D_REG_INDEX(tex_header.address_high), 0);
    f.Write(MAXWELL3D_REG_INDEX(tex_header.address_low), static_cast<u32>(headers));
    f.Write(MAXWELL3D_REG_INDEX(tex_header.limit), 7);
    const auto breaks = [&](u32 offset) {
        FastDraw::g_delta = {};
        const u64 serial = FastDraw::Serial();
        f.SetConstantBuffer(alias, 0x1000, offset);
        f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), 1);
        return FastDraw::Serial() != serial;
    };
    CHECK(breaks(0xe0));        // the last texture header
    CHECK_FALSE(breaks(0x100)); // just past the headers
}

TEST_CASE("Fast draw finds texture handle data written through another mapping",
          "[fast_draw]") {
    DeltaFixture f;
    f.memory.BindRasterizer(&f.rasterizer);
    constexpr GPUVAddr handles = 0x100000; // a buffer of texture handles, 0x100 bytes
    constexpr GPUVAddr alias = 0x300000;   // its device memory, mapped again
    constexpr GPUVAddr other = 0x500000;   // other device memory
    constexpr GPUVAddr unmapped = 0x700000;
    constexpr auto Kind = Tegra::PTEKind::PITCH;
    f.memory.Map(handles, 0x200000, 0x1000, Kind, false);
    f.memory.Map(alias, 0x200000, 0x1000, Kind, false);
    f.memory.Map(other, 0x400000, 0x1000, Kind, false);
    FastDraw::DeviceSpans spans;
    const auto reaches = [&](GPUVAddr handle_buffer, GPUVAddr buffer, u32 offset,
                             std::vector<u32> words) {
        FastDraw::g_delta = {};
        f.SetConstantBuffer(buffer, 0x2000, offset);
        f.maxwell3d.CallMultiMethod(f.system, MAXWELL3D_REG_INDEX(const_buffer.buffer),
                                    words.data(), static_cast<u32>(words.size()),
                                    static_cast<u32>(words.size()));
        spans.Update(f.memory, handle_buffer, 0x100);
        return FastDraw::g_delta.Reaches(handle_buffer, 0x100, spans);
    };
    CHECK(reaches(handles, handles, 0x10, {1}));
    CHECK(reaches(handles, alias, 0x10, {1})); // missed when compared by GPU address only
    CHECK(reaches(handles, alias, 0xfc, {1, 2}));
    CHECK_FALSE(reaches(handles, alias, 0x100, {1})); // just past the handles
    CHECK_FALSE(reaches(handles, other, 0x10, {1}));
    // Nothing mapped there: compared by GPU address, as before.
    CHECK(reaches(unmapped, unmapped, 0x10, {1}));
    CHECK_FALSE(reaches(unmapped, other, 0x10, {1}));

    // A write across two pages that map device memory apart lands on both.
    f.memory.Map(alias, 0x201000, 0x1000, Kind, false);
    f.memory.Map(alias + 0x1000, 0x200000, 0x1000, Kind, false);
    CHECK(reaches(handles, alias, 0xffc, {1, 2}));
    CHECK(FastDraw::g_delta.num_device_writes == 2);
    CHECK_FALSE(reaches(handles, alias, 0xff8, {1, 2}));

    // Mapping changes translate the handle buffer again.
    CHECK_FALSE(reaches(handles, other, 0x10, {1}));
    f.memory.Map(handles, 0x400000, 0x1000, Kind, false);
    CHECK(reaches(handles, other, 0x10, {1}));
    CHECK_FALSE(reaches(handles, alias + 0x1000, 0x10, {1}));
}

TEST_CASE("Fast draw records and breaks nothing while switched off", "[fast_draw]") {
    DeltaFixture f;
    FastDrawFixture::SetSwitch(false);
    const u64 serial = FastDraw::Serial();
    f.Write(MAXWELL3D_REG_INDEX(blend.enable[0]), f.Reg(MAXWELL3D_REG_INDEX(blend.enable[0])) ^ 1);
    f.SetConstantBuffer(0x10000, 0x100, 0);
    f.Write(MAXWELL3D_REG_INDEX(bind_groups[0].raw_config), 0x11);
    f.Write(MAXWELL3D_REG_INDEX(const_buffer.buffer), 1);
    FastDraw::Break(FastDraw::Reason::Fence);
    CHECK(FastDraw::Serial() == serial);
    CHECK(FastDraw::g_delta.Empty());
}

TEST_CASE("Fast draw keeps base vertex and instance as draw parameters", "[fast_draw]") {
    FastDrawFixture f;
    f.DrawArrays(0, 3);
    f.Write(MAXWELL3D_REG_INDEX(global_base_vertex_index),
            f.Reg(MAXWELL3D_REG_INDEX(global_base_vertex_index)) + 64);
    f.Write(MAXWELL3D_REG_INDEX(global_base_instance_index),
            f.Reg(MAXWELL3D_REG_INDEX(global_base_instance_index)) + 1);
    f.DrawArrays(0, 3);
    CHECK(f.Reusable());
}

TEST_CASE("Fast draw retains vertex ID writes and shadow replay without changing indexed draw bases",
          "[fast_draw][vertex_id_base]") {
    constexpr u32 vertex_id = MAXWELL3D_REG_INDEX(vertex_id_base);
    constexpr u32 shadow_control = MAXWELL3D_REG_INDEX(shadow_ram_control);
    using Control = Maxwell3D::Regs::ShadowRamControl;
    for (const bool fast : {false, true}) {
        CAPTURE(fast);
        FastDrawFixture f;
        f.maxwell3d.fast_maxwell3d = fast;
        Tegra::MacroInterpreterImpl macro;
        // Observe both dirty-table writes even though this register has no renderer consumer.
        f.maxwell3d.dirty.tables[0][vertex_id] = VideoCommon::Dirty::VertexBuffers;
        f.maxwell3d.dirty.tables[1][vertex_id] = VideoCommon::Dirty::IndexBuffer;
        u32 expected_base = 23;
        u32 expected_vertex_id = 0;
        f.rasterizer.on_draw = [&](bool indexed, u32 instances) {
            CHECK(indexed);
            CHECK(instances == 1);
            CHECK(f.maxwell3d.draw_manager.draw_state.base_index == expected_base);
            CHECK(macro.Read(f.maxwell3d, vertex_id) == expected_vertex_id);
        };
        const auto draw = [&] {
            f.Write(MAXWELL3D_REG_INDEX(index_buffer.first), 0);
            f.Write(MAXWELL3D_REG_INDEX(index_buffer.count), 3);
            f.Write(MAXWELL3D_REG_INDEX(draw.begin), 4);
            f.Write(MAXWELL3D_REG_INDEX(draw.end), 0);
        };
        const auto write_vertex_id = [&](u32 argument, u32 expected) {
            f.maxwell3d.dirty.flags.reset();
            const u64 serial = FastDraw::Serial();
            f.Write(vertex_id, argument);
            CHECK(FastDraw::Serial() == serial);
            CHECK(f.Reg(vertex_id) == expected);
            CHECK(macro.Read(f.maxwell3d, vertex_id) == expected);
            CHECK(f.maxwell3d.dirty.flags[VideoCommon::Dirty::VertexBuffers]);
            CHECK(f.maxwell3d.dirty.flags[VideoCommon::Dirty::IndexBuffer]);
            expected_vertex_id = expected;
        };

        f.Write(shadow_control, static_cast<u32>(Control::Track));
        f.Write(MAXWELL3D_REG_INDEX(global_base_vertex_index), expected_base);
        draw();
        write_vertex_id(11, 11);
        CHECK(f.maxwell3d.shadow_state.vertex_id_base == 11);
        draw();
        CHECK(f.Reusable());

        f.Write(shadow_control, static_cast<u32>(Control::Passthrough));
        write_vertex_id(29, 29);
        CHECK(f.maxwell3d.shadow_state.vertex_id_base == 11);
        draw();
        f.Write(shadow_control, static_cast<u32>(Control::Replay));
        write_vertex_id(99, 11);
        CHECK(f.maxwell3d.shadow_state.vertex_id_base == 11);
        draw();

        f.Write(shadow_control, static_cast<u32>(Control::Passthrough));
        expected_base = 47;
        f.Write(MAXWELL3D_REG_INDEX(global_base_vertex_index), expected_base);
        write_vertex_id(73, 73);
        draw();
        REQUIRE(f.rasterizer.draws.size() == 5);
    }
}

TEST_CASE("A vertex ID write flushes deferred instances before storing the new value",
          "[fast_draw][vertex_id_base]") {
    for (const bool fast : {false, true}) {
        CAPTURE(fast);
        FastDrawFixture f;
        f.maxwell3d.fast_maxwell3d = fast;
        f.Write(MAXWELL3D_REG_INDEX(vertex_id_base), 17);
        f.Write(MAXWELL3D_REG_INDEX(global_base_vertex_index), 31);
        f.rasterizer.on_draw = [&](bool indexed, u32 instances) {
            CHECK(indexed);
            CHECK(instances == (f.rasterizer.draws.size() == 1 ? 1 : 3));
            CHECK(f.maxwell3d.regs.vertex_id_base == 17);
            CHECK(f.maxwell3d.draw_manager.draw_state.base_index == 31);
        };
        const auto draw = [&](u32 begin) {
            f.Write(MAXWELL3D_REG_INDEX(draw.begin), begin);
            f.Write(MAXWELL3D_REG_INDEX(index_buffer.first), 0);
            f.Write(MAXWELL3D_REG_INDEX(index_buffer.count), 3);
            f.Write(MAXWELL3D_REG_INDEX(draw.end), 0);
        };
        draw(4);
        draw(4 | (1U << 26));
        draw(4 | (1U << 26));
        REQUIRE(f.rasterizer.draws.size() == 1);
        REQUIRE(f.maxwell3d.draw_manager.HasDeferredDraw());
        const u64 serial = FastDraw::Serial();
        f.Write(MAXWELL3D_REG_INDEX(vertex_id_base), 71);
        REQUIRE(f.rasterizer.draws.size() == 2);
        CHECK_FALSE(f.maxwell3d.draw_manager.HasDeferredDraw());
        CHECK(f.maxwell3d.regs.vertex_id_base == 71);
        CHECK(FastDraw::Serial() == serial);
        CHECK(f.Reusable());
    }
}

TEST_CASE("Fast draw keeps vertex stream addresses and sizes light", "[fast_draw]") {
    DeltaFixture f;
    const auto stream = [](u32 index, u32 word) {
        return static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_streams) + index * 4 + word);
    };
    const auto light = [&](u32 method) {
        FastDraw::g_delta = {};
        const u64 serial = FastDraw::Serial();
        f.Write(method, f.Reg(method) + 0x40);
        return FastDraw::Serial() == serial;
    };
    CHECK(light(stream(0, 1)));
    CHECK(light(stream(3, 2)));
    CHECK(light(0x600 / 4 + 1));          // SET_VERTEX_STREAM_SIZE_B(0)
    CHECK(light(0x600 / 4 + 31 * 2));     // SET_VERTEX_STREAM_SIZE_A(31)
    CHECK(light(MAXWELL3D_REG_INDEX(vertex_stream_limits) + 5 * 2 + 1));
    CHECK_FALSE(light(stream(0, 0)));     // stride and enable
    CHECK_FALSE(light(stream(0, 3)));     // frequency
    CHECK_FALSE(light(0x600 / 4 + 64));   // past the sizes
}

TEST_CASE("Query reset writes keep their dispatch, dirty flags and shadow replay",
          "[fast_draw][query_reset]") {
    constexpr u32 reset = MAXWELL3D_REG_INDEX(clear_report_value);
    constexpr u32 shadow_control = MAXWELL3D_REG_INDEX(shadow_ram_control);
    using Query = VideoCommon::QueryType;
    using Control = Maxwell3D::Regs::ShadowRamControl;
    for (const bool fast : {false, true}) {
        CAPTURE(fast);
        FastDrawFixture f;
        f.maxwell3d.fast_maxwell3d = fast;
        f.maxwell3d.dirty.tables[0][reset] = VideoCommon::Dirty::VertexBuffers;
        f.maxwell3d.dirty.tables[1][reset] = VideoCommon::Dirty::IndexBuffer;
        std::vector<Query> resets;
        f.rasterizer.on_reset = [&](Query type) { resets.push_back(type); };
        const auto reset_counter = [&](u32 argument, u32 stored, Query type) {
            CAPTURE(argument, stored, type);
            f.DrawArrays(0, 3);
            f.maxwell3d.dirty.flags.reset();
            const size_t calls = resets.size();
            const u64 serial = FastDraw::Serial();
            f.Write(reset, argument);
            REQUIRE(resets.size() == calls + 1);
            CHECK(resets.back() == type);
            CHECK(f.Reg(reset) == stored);
            CHECK(f.maxwell3d.dirty.flags[VideoCommon::Dirty::VertexBuffers]);
            CHECK(f.maxwell3d.dirty.flags[VideoCommon::Dirty::IndexBuffer]);
            CHECK(FastDraw::Serial() == serial);
            f.DrawArrays(0, 3);
            CHECK(f.Reusable());
        };

        f.Write(shadow_control, static_cast<u32>(Control::Track));
        reset_counter(0x01, 0x01, Query::ZPassPixelCount64);
        reset_counter(0x01, 0x01, Query::ZPassPixelCount64); // Same value still resets.
        reset_counter(0x10, 0x10, Query::StreamingPrimitivesSucceeded);
        reset_counter(0x1f, 0x1f, Query::VtgPrimitivesOut);
        reset_counter(0x13, 0x13, Query::PrimitivesGenerated);
        reset_counter(0xff, 0xff, Query::Payload); // Unsupported selector's existing dispatch.
        reset_counter(0x01, 0x01, Query::ZPassPixelCount64);
        CHECK(f.maxwell3d.shadow_state.reg_array[reset] == 0x01);

        f.Write(shadow_control, static_cast<u32>(Control::Passthrough));
        reset_counter(0x10, 0x10, Query::StreamingPrimitivesSucceeded);
        CHECK(f.maxwell3d.shadow_state.reg_array[reset] == 0x01);
        f.Write(shadow_control, static_cast<u32>(Control::Replay));
        reset_counter(0x1f, 0x01, Query::ZPassPixelCount64);
        reset_counter(0xff, 0x01, Query::ZPassPixelCount64);
        CHECK(f.maxwell3d.shadow_state.reg_array[reset] == 0x01);
    }
}

TEST_CASE("Query resets flush deferred instances before updating and resetting the counter",
          "[fast_draw][query_reset]") {
    constexpr u32 reset = MAXWELL3D_REG_INDEX(clear_report_value);
    using Query = VideoCommon::QueryType;
    for (const bool fast : {false, true}) {
        for (const u32 selector : {0x01U, 0x10U}) {
            CAPTURE(fast, selector);
            FastDrawFixture f;
            f.maxwell3d.fast_maxwell3d = fast;
            f.Write(reset, 0x01);
            f.Write(MAXWELL3D_REG_INDEX(global_base_vertex_index), 31);
            std::vector<char> events;
            u32 expected_selector = 0x01;
            // Models ordering at the rasterizer interface, not Vulkan query results.
            u32 counted_instances = 0;
            f.rasterizer.on_draw = [&](bool indexed, u32 instances) {
                CHECK(indexed);
                CHECK(f.Reg(reset) == expected_selector);
                CHECK(f.maxwell3d.draw_manager.draw_state.base_index == 31);
                CHECK(instances == (events.size() == 1 ? 3 : 1));
                counted_instances += instances;
                events.push_back('D');
            };
            f.rasterizer.on_reset = [&](Query type) {
                CHECK((events == std::vector<char>{'D', 'D'}));
                CHECK(counted_instances == 4);
                CHECK(f.Reg(reset) == selector);
                CHECK(type == (selector == 0x01 ? Query::ZPassPixelCount64
                                               : Query::StreamingPrimitivesSucceeded));
                CHECK_FALSE(f.maxwell3d.draw_manager.HasDeferredDraw());
                counted_instances = 0;
                events.push_back('R');
            };
            const auto draw = [&](u32 begin) {
                f.Write(MAXWELL3D_REG_INDEX(draw.begin), begin);
                f.Write(MAXWELL3D_REG_INDEX(index_buffer.first), 0);
                f.Write(MAXWELL3D_REG_INDEX(index_buffer.count), 3);
                f.Write(MAXWELL3D_REG_INDEX(draw.end), 0);
            };
            draw(4);
            draw(4 | (1U << 26));
            draw(4 | (1U << 26));
            REQUIRE(f.rasterizer.draws.size() == 1);
            REQUIRE(f.maxwell3d.draw_manager.HasDeferredDraw());
            const u64 serial = FastDraw::Serial();
            f.Write(reset, selector);
            CHECK((events == std::vector<char>{'D', 'D', 'R'}));
            CHECK(counted_instances == 0);
            CHECK_FALSE(f.maxwell3d.draw_manager.HasDeferredDraw());
            CHECK(FastDraw::Serial() == serial);
            expected_selector = selector;
            draw(4);
            REQUIRE(f.rasterizer.draws.size() == 3);
            CHECK((events == std::vector<char>{'D', 'D', 'R', 'D'}));
            CHECK(counted_instances == 1);
            CHECK(f.Reusable());
        }
    }
}

TEST_CASE("Query reset reuse does not exempt surface clears or render enable methods",
          "[fast_draw][query_reset]") {
    using Enable = Maxwell3D::Regs::RenderEnable;
    for (const bool fast : {false, true}) {
        CAPTURE(fast);
        FastDrawFixture f;
        f.maxwell3d.fast_maxwell3d = fast;
        const auto breaks = [&](u32 method, u32 value) {
            f.DrawArrays(0, 3);
            const u64 serial = FastDraw::Serial();
            f.Write(method, value);
            CHECK(FastDraw::Serial() != serial);
            f.DrawArrays(0, 3);
            CHECK_FALSE(f.Reusable());
        };
        breaks(MAXWELL3D_REG_INDEX(clear_surface), 0x3c);
        // True needs no guest-memory operand and leaves subsequent draws enabled.
        breaks(MAXWELL3D_REG_INDEX(render_enable.mode), static_cast<u32>(Enable::Mode::True));
        breaks(MAXWELL3D_REG_INDEX(render_enable.mode), static_cast<u32>(Enable::Mode::True));
    }
}
