// The fastmem fault callback without code writes (headless/dynarmic/fastmem-callback.cmake),
// in the A64 unit (EDEN_FASTMEM_A64_UNIT): the A32 unit keeps upstream's code.
#ifdef EDEN_FASTMEM_A64_UNIT
extern "C" bool eden_jit_invalidate_running(unsigned long long pc) __attribute__((weak));  // jit_impl.inc
inline std::atomic<unsigned long long> eden_fastmem_demotions{0};
namespace {
std::mutex& EdenFastmemSiteMutex() {
    static std::mutex mutex;
    return mutex;
}
// Sites a fault took off fastmem, on any core: (block location, IR instruction index).
std::set<std::pair<u64, u32>>& EdenFastmemDemoted() {
    static std::set<std::pair<u64, u32>> demoted;
    return demoted;
}
[[maybe_unused]] u64 EdenFastmemSitePC(A64::Jit*, IR::LocationDescriptor location) {
    return A64::LocationDescriptor{location}.PC();
}
[[maybe_unused]] u64 EdenFastmemSitePC(A32::Jit*, IR::LocationDescriptor location) {
    return A32::LocationDescriptor{location}.PC();
}
}  // namespace

// For the page-reuse pass (headless/dynarmic/page_reuse_pass.inc): whether a site is off the window.
extern "C" bool eden_fastmem64_site_demoted(unsigned long long location, unsigned inst) {
    std::lock_guard lock{EdenFastmemSiteMutex()};
    return EdenFastmemDemoted().count({location, static_cast<u32>(inst)}) > 0;
}

// The demoted sites of a title, kept across sessions (headless/main.cpp): a later session compiles
// them off the window from the start instead of learning them again through faults. Saved relative
// to the game's code, which loads elsewhere each session, with the game's build ID; another build's
// list is ignored, and so is an older format's list learned under other window rules (EF64: before
// stacks were given pages that agree with the window, full of sites only their misalignment
// demoted; EF65: before pieces with an unmapped page were shown, full of sites only those
// demoted). Bump the format whenever the window's rules change. File: "EF66", the build ID (32 bytes), then per site its location less the code
// start (8 bytes) and its IR instruction index (4 bytes).
extern "C" std::size_t eden_fastmem64_load_sites(const char* path, unsigned long long code_start,
                                                 const unsigned char* build) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return 0;
    char magic[4];
    unsigned char saved[32];
    std::size_t loaded = 0;
    if (std::fread(magic, 1, 4, file) == 4 && std::memcmp(magic, "EF66", 4) == 0 &&
        std::fread(saved, 1, 32, file) == 32 && std::memcmp(saved, build, 32) == 0) {
        std::lock_guard lock{EdenFastmemSiteMutex()};
        u64 offset;
        u32 inst;
        while (std::fread(&offset, sizeof(offset), 1, file) == 1 && std::fread(&inst, sizeof(inst), 1, file) == 1)
            loaded += EdenFastmemDemoted().insert({offset + code_start, inst}).second;
    }
    std::fclose(file);
    return loaded;
}
extern "C" std::size_t eden_fastmem64_save_sites(const char* path, unsigned long long code_start,
                                                 unsigned long long code_size, const unsigned char* build) {
    const std::string fresh = std::string{path} + ".new";
    std::FILE* file = std::fopen(fresh.c_str(), "wb");
    if (!file) return 0;
    std::size_t written = 0;
    bool ok = std::fwrite("EF66", 1, 4, file) == 4 && std::fwrite(build, 1, 32, file) == 32;
    {
        std::lock_guard lock{EdenFastmemSiteMutex()};
        for (const auto& [location, inst] : EdenFastmemDemoted()) {
            const u64 pc = A64::LocationDescriptor{IR::LocationDescriptor{location}}.PC();
            if (!ok || pc < code_start || pc - code_start >= code_size) continue;
            const u64 offset = location - code_start;
            ok = std::fwrite(&offset, sizeof(offset), 1, file) == 1 && std::fwrite(&inst, sizeof(inst), 1, file) == 1;
            written += ok;
        }
    }
    ok = std::fclose(file) == 0 && ok && std::rename(fresh.c_str(), path) == 0;
    return ok ? written : 0;
}
#endif
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#define CONCATENATE_TOKENS(x, y) CONCATENATE_TOKENS_IMPL(x, y)
#define CONCATENATE_TOKENS_IMPL(x, y) x##y

#define AxxEmitX64 CONCATENATE_TOKENS(Axx, EmitX64)
#define AxxEmitContext CONCATENATE_TOKENS(Axx, EmitContext)
#define AxxJitState CONCATENATE_TOKENS(Axx, JitState)
#define AxxUserConfig Axx::UserConfig

namespace {
using Vector = std::array<u64, 2>;
}
#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5

constexpr bool checked_fastmem = std::is_same_v<AxxEmitContext, A32EmitContext>;
#endif // PS5

std::optional<AxxEmitX64::DoNotFastmemMarker> AxxEmitX64::ShouldFastmem(AxxEmitContext& ctx, IR::Inst* inst) const {
    if (!conf.fastmem_pointer || !exception_handler.SupportsFastmem()) {
        return std::nullopt;
    }

    const auto marker = std::make_tuple(ctx.Location(), inst->GetName());
#ifdef EDEN_FASTMEM_A64_UNIT
    std::lock_guard eden_lock{EdenFastmemSiteMutex()};
    if (EdenFastmemDemoted().count({IR::LocationDescriptor{std::get<0>(marker)}.Value(), static_cast<u32>(std::get<1>(marker))}) > 0) {
        return std::nullopt;
    }
#endif
    if (do_not_fastmem.count(marker) > 0) {
        return std::nullopt;
    }
    return marker;
}

void AxxEmitX64::AddFastmemPatch(CodePtr location, CodePtr resume_location, void (*callback)(), const DoNotFastmemMarker& marker, bool recompile) {
#ifdef EDEN_FASTMEM_A64_UNIT
    std::lock_guard eden_lock{EdenFastmemSiteMutex()};
#endif
    const u64 base = std::bit_cast<u64>(code.getCode());
    const auto offset = [base](u64 address) {
        ASSERT(address >= base && address - base <= UINT32_MAX);
        return u32(address - base);
    };
    FastmemPatchInfo info{
        .rip = offset(std::bit_cast<u64>(location)),
        .resume_rip = offset(std::bit_cast<u64>(resume_location)),
        .callback = offset(std::bit_cast<u64>(callback)),
        .inst_name = 0,
        .recompile = 0,
        .location = std::get<0>(marker),
    };
    info.inst_name = std::get<1>(marker);
    info.recompile = recompile ? 1 : 0;
    auto it = fastmem_patch_info.end();
    if (!fastmem_patch_info.empty() && fastmem_patch_info.back().rip >= info.rip) {
        it = std::lower_bound(fastmem_patch_info.begin(), fastmem_patch_info.end(), info.rip,
                              [](const FastmemPatchInfo& entry, u32 value) { return entry.rip < value; });
        if (it != fastmem_patch_info.end() && it->rip == info.rip) {
            return;
        }
    }
    fastmem_patch_info.insert(it, info);
}

FakeCall AxxEmitX64::FastmemCallback(u64 rip_) {
#ifdef EDEN_FASTMEM_A64_UNIT
    std::unique_lock eden_lock{EdenFastmemSiteMutex()};
#endif
    const u64 base = std::bit_cast<u64>(code.getCode());
    const auto iter = rip_ >= base && rip_ - base <= UINT32_MAX
                        ? std::lower_bound(fastmem_patch_info.begin(), fastmem_patch_info.end(), u32(rip_ - base),
                                           [](const FastmemPatchInfo& entry, u32 value) { return entry.rip < value; })
                        : fastmem_patch_info.end();
    if (iter != fastmem_patch_info.end() && iter->rip == u32(rip_ - base)) {
        FakeCall result{
            .call_rip = base + iter->callback,
            .ret_rip = base + iter->resume_rip,
        };
        if (iter->recompile) {
#ifndef EDEN_FASTMEM_A64_UNIT
            // A32 (its checked fastmem window, which faults only on races): upstream's handling.
            const DoNotFastmemMarker marker{iter->location, static_cast<unsigned>(iter->inst_name)};
            do_not_fastmem.insert(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
#else
            // No code is written here (headless/dynarmic/fastmem-callback.cmake).
            const DoNotFastmemMarker marker{iter->location, static_cast<unsigned>(iter->inst_name)};
            do_not_fastmem.insert(marker);
            EdenFastmemDemoted().insert({IR::LocationDescriptor{std::get<0>(marker)}.Value(), static_cast<u32>(std::get<1>(marker))});
            eden_lock.unlock();
            eden_fastmem_demotions.fetch_add(1, std::memory_order_relaxed);
            const u64 pc = EdenFastmemSitePC(jit_interface, std::get<0>(marker));
            if (!eden_jit_invalidate_running || !eden_jit_invalidate_running(pc))
                jit_interface->InvalidateCacheRange(pc, 4);
#endif
        }
        return result;
    }
    fmt::print("dynarmic: Segfault happened within JITted code at rip = {:016x}\n"
        "Segfault wasn't at a fastmem patch location!\n", rip_);
    UNREACHABLE(); //("iter != fastmem_patch_info.end()");
}

// the full page-table lookup of a grouped access whose group lookup gave 0
// (headless/dynarmic/page-reuse.cmake), for code inside a deferred block: the direct table, its
// address space range-checked, misalignment detected only on page boundaries.
template<typename Config>
size_t EdenReuseAddressBits(const Config& config) {
    if constexpr (requires { config.page_table_address_space_bits; }) {
        return config.page_table_address_space_bits;
    } else {
        return 32;  // A32: never grouped
    }
}

inline void EdenReuseSlowLookup(BlockOfCode& code, size_t bitsize, bool detect, size_t valid_page_index_bits,
        Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 page) {
    if (bitsize != 8 && detect) {
        const u32 align_mask = static_cast<u32>(bitsize / 8 - 1);
        const u32 page_align_mask = static_cast<u32>(page_table_const_size - 1) & ~align_mask;
        Xbyak::Label aligned;
        code.test(vaddr, align_mask);
        code.jz(aligned);
        code.mov(page, vaddr);
        code.and_(page, page_align_mask);
        code.cmp(page, page_align_mask);
        code.je(abort, code.T_NEAR);
        code.L(aligned);
    }
    code.mov(page, vaddr);
    code.shr(page, int(page_table_const_bits));
    code.test(page, u32(-(1 << valid_page_index_bits)));
    code.jnz(abort, code.T_NEAR);
    code.mov(page, code.qword[r14 + page * 8]);
    code.test(page, page);
    code.jz(abort, code.T_NEAR);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitMemoryRead(AxxEmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    // a grouped access (headless/dynarmic/page-reuse.cmake), its group's page in args[0].
    if (!args[0].IsImmediate()) {
        const bool vector = (bitsize == 32 || bitsize == 64) && args[2].GetImmediateAccType() == IR::AccType::VEC;
        const Xbyak::Reg64 host = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
        const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
        const int value_idx = bitsize == 128 || vector ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();
        const auto wrapped_fn = read_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), vector ? page.getIdx() : value_idx)];
        const bool detect = (conf.detect_misaligned_access_via_page_table & bitsize) != 0;
        const size_t valid_page_index_bits = EdenReuseAddressBits(conf) - page_table_const_bits;
        SharedLabel slow = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        const auto read = [this, vector, value_idx](const Xbyak::RegExp& address) {
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(Xbyak::Xmm{value_idx}, code.dword[address]);
                    else code.movq(Xbyak::Xmm{value_idx}, code.qword[address]);
                    return;
                }
            }
            EmitReadMemoryMov<bitsize>(code, value_idx, address, false);
        };
        code.test(host, host);
        code.jz(*slow, code.T_NEAR);
        read(host + vaddr);
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*slow);
            EdenReuseSlowLookup(code, bitsize, detect, valid_page_index_bits, *abort, vaddr, page);
            read(page + vaddr);
            code.jmp(*end, code.T_NEAR);
            code.L(*abort);
            code.call(wrapped_fn);
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(Xbyak::Xmm{value_idx}, page.cvt32());
                    else code.movq(Xbyak::Xmm{value_idx}, page);
                }
            }
            code.jmp(*end, code.T_NEAR);
        });
        code.L(*end);
        if (bitsize == 128 || vector) {
            ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
        } else {
            ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
        }
        return;
    }
    const auto fastmem_marker = ShouldFastmem(ctx, inst);

    if (!conf.page_table && !fastmem_marker) {
        // Neither fastmem nor page table: Use callbacks
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.HostCall(code, nullptr, {}, args[1]);
            if (ordered) code.mfence();
            code.CallFunction(memory_read_128);
            ctx.reg_alloc.DefineValue(code, inst, xmm1);
        } else {
            ctx.reg_alloc.HostCall(code, inst, {}, args[1]);
            code.mov(code.ABI_PARAM3.cvt32(), bitsize / CHAR_BIT);
            if (ordered) code.mfence();
            Devirtualize<callback>(conf.callbacks).EmitCall(code);
            code.ZeroExtendFrom(bitsize, code.ABI_RETURN);
        }
        EmitCheckMemoryAbort(ctx, inst);
        return;
    }

#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5
    if constexpr (checked_fastmem && (bitsize == 32 || bitsize == 64)) {
        if (fastmem_marker && args[2].GetImmediateAccType() == IR::AccType::VEC) {
            const auto vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
            const auto temporary = ctx.reg_alloc.ScratchGpr(code);
            const auto vector = ctx.reg_alloc.ScratchXmm(code);
            const auto wrapped_fn = read_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), temporary.getIdx())];
            SharedLabel blocked = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
            EmitCheckedFastmemTest(code, vaddr, temporary, checked_read_blocked, *blocked);
            const auto location = code.getCurr();
            if constexpr (bitsize == 32) code.movd(vector, code.dword[r13 + vaddr]);
            else code.movq(vector, code.qword[r13 + vaddr]);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*blocked);
                const auto pointer = EmitCheckedFallbackLookup(code, ctx, bitsize, *abort, vaddr, temporary);
                if constexpr (bitsize == 32) code.movd(vector, code.dword[pointer]);
                else code.movq(vector, code.qword[pointer]);
                code.jmp(*end, code.T_NEAR);
                code.L(*abort);
                code.call(wrapped_fn);
                // A faulting direct load also returns here, with the value in temporary.
                AddFastmemPatch(location, code.getCurr(), wrapped_fn, *fastmem_marker, conf.recompile_on_fastmem_failure);
                if constexpr (bitsize == 32) code.movd(vector, temporary.cvt32());
                else code.movq(vector, temporary);
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
            });
            code.L(*end);
            ctx.reg_alloc.DefineValue(code, inst, vector);
            return;
        }
    }
#endif // PS5
    if constexpr (bitsize == 32 || bitsize == 64) {
        if (!fastmem_marker && args[2].GetImmediateAccType() == IR::AccType::VEC) {
            const auto vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
            const auto temporary = ctx.reg_alloc.ScratchGpr(code);
            const auto vector = ctx.reg_alloc.ScratchXmm(code);
            const auto wrapped_fn = read_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), temporary.getIdx())];
            SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
            const auto pointer = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
            if constexpr (bitsize == 32) code.movd(vector, code.dword[pointer]);
            else code.movq(vector, code.qword[pointer]);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*abort);
                code.call(wrapped_fn);
                if constexpr (bitsize == 32) code.movd(vector, temporary.cvt32());
                else code.movq(vector, temporary);
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
            });
            code.L(*end);
            ctx.reg_alloc.DefineValue(code, inst, vector);
            return;
        }
    }

    if (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128 ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();

    const auto wrapped_fn = read_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value_idx)];

    SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();

    if (fastmem_marker) {
        // Use fastmem
#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5
        if constexpr (checked_fastmem) {
            const Xbyak::Reg64 index = ctx.reg_alloc.ScratchGpr(code);
            SharedLabel blocked = ctx.GenSharedLabel();
            EmitCheckedFastmemTest(code, vaddr, index, checked_read_blocked, *blocked);
            const auto location = EmitReadMemoryMov<bitsize>(code, value_idx, r13 + vaddr, ordered);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*blocked);
                const auto fallback_ptr = EmitCheckedFallbackLookup(code, ctx, bitsize, *abort, vaddr, index);
                EmitReadMemoryMov<bitsize>(code, value_idx, fallback_ptr, ordered);
                code.jmp(*end, code.T_NEAR);
                code.L(*abort);
                code.call(wrapped_fn);
                AddFastmemPatch(location, code.getCurr(), wrapped_fn, *fastmem_marker, conf.recompile_on_fastmem_failure);
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
            });
        } else {
#endif // PS5
        bool require_abort_handling = false;
        const auto src_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        const auto location = EmitReadMemoryMov<bitsize>(code, value_idx, src_ptr, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);

            AddFastmemPatch(location, code.getCurr(), wrapped_fn, *fastmem_marker, conf.recompile_on_fastmem_failure);

            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5
        }
#endif // PS5
    } else {
        // Use page table
        ASSERT(conf.page_table);
        const auto src_ptr = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
        EmitReadMemoryMov<bitsize>(code, value_idx, src_ptr, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    }
    code.L(*end);

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
    } else {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
    }
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitMemoryWrite(AxxEmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    // a grouped access (headless/dynarmic/page-reuse.cmake), its group's page in args[0].
    if (!args[0].IsImmediate()) {
        const bool vector = (bitsize == 32 || bitsize == 64) && args[3].GetImmediateAccType() == IR::AccType::VEC;
        const Xbyak::Reg64 host = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
        const int value_idx = bitsize == 128 || vector ? ctx.reg_alloc.UseXmm(code, args[2]).getIdx() : ctx.reg_alloc.UseGpr(code, args[2]).getIdx();
        const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
        const auto wrapped_fn = write_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), vector ? page.getIdx() : value_idx)];
        const bool detect = (conf.detect_misaligned_access_via_page_table & bitsize) != 0;
        const size_t valid_page_index_bits = EdenReuseAddressBits(conf) - page_table_const_bits;
        SharedLabel slow = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        const auto write = [this, vector, value_idx](const Xbyak::RegExp& address) {
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(code.dword[address], Xbyak::Xmm{value_idx});
                    else code.movq(code.qword[address], Xbyak::Xmm{value_idx});
                    return;
                }
            }
            EmitWriteMemoryMov<bitsize>(code, address, value_idx, false);
        };
        code.test(host, host);
        code.jz(*slow, code.T_NEAR);
        write(host + vaddr);
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*slow);
            EdenReuseSlowLookup(code, bitsize, detect, valid_page_index_bits, *abort, vaddr, page);
            write(page + vaddr);
            code.jmp(*end, code.T_NEAR);
            code.L(*abort);
            if constexpr (bitsize == 32 || bitsize == 64) {
                if (vector) {
                    if constexpr (bitsize == 32) code.movd(page.cvt32(), Xbyak::Xmm{value_idx});
                    else code.movq(page, Xbyak::Xmm{value_idx});
                }
            }
            code.call(wrapped_fn);
            code.jmp(*end, code.T_NEAR);
        });
        code.L(*end);
        return;
    }
    const auto fastmem_marker = ShouldFastmem(ctx, inst);

    if (!conf.page_table && !fastmem_marker) {
        // Neither fastmem nor page table: Use callbacks
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
            ctx.reg_alloc.Use(code, args[2], HostLoc::XMM1);
            ctx.reg_alloc.EndOfAllocScope();
            ctx.reg_alloc.HostCall(code, nullptr);
            code.CallFunction(memory_write_128);
        } else {
            // { this, vaddr, value, size }
            ctx.reg_alloc.HostCall(code, nullptr, {}, args[1], args[2]);
            code.mov(code.ABI_PARAM4.cvt32(), bitsize / CHAR_BIT);
            Devirtualize<callback>(conf.callbacks).EmitCall(code);
        }
        if (ordered) code.mfence();
        EmitCheckMemoryAbort(ctx, inst);
        return;
    }

#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5
    if constexpr (checked_fastmem && (bitsize == 32 || bitsize == 64)) {
        if (fastmem_marker && args[3].GetImmediateAccType() == IR::AccType::VEC) {
            const auto vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
            const auto temporary = ctx.reg_alloc.ScratchGpr(code);
            const auto vector = ctx.reg_alloc.UseXmm(code, args[2]);
            const auto wrapped_fn = write_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), temporary.getIdx())];
            SharedLabel blocked = ctx.GenSharedLabel(), abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
            EmitCheckedFastmemTest(code, vaddr, temporary, checked_write_blocked, *blocked);
            const auto location = code.getCurr();
            if constexpr (bitsize == 32) code.movd(code.dword[r13 + vaddr], vector);
            else code.movq(code.qword[r13 + vaddr], vector);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*blocked);
                const auto pointer = EmitCheckedFallbackLookup(code, ctx, bitsize, *abort, vaddr, temporary);
                if constexpr (bitsize == 32) code.movd(code.dword[pointer], vector);
                else code.movq(code.qword[pointer], vector);
                code.jmp(*end, code.T_NEAR);
                // A faulting direct store enters here: the fallback takes its value in temporary.
                const auto fault_entry = code.getCurr();
                if constexpr (bitsize == 32) code.movd(temporary.cvt32(), vector);
                else code.movq(temporary, vector);
                code.jmp(reinterpret_cast<const void*>(wrapped_fn), code.T_NEAR);
                code.L(*abort);
                if constexpr (bitsize == 32) code.movd(temporary.cvt32(), vector);
                else code.movq(temporary, vector);
                code.call(wrapped_fn);
                AddFastmemPatch(location, code.getCurr(), std::bit_cast<void (*)()>(fault_entry), *fastmem_marker, conf.recompile_on_fastmem_failure);
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
            });
            code.L(*end);
            return;
        }
    }
#endif // PS5
    if constexpr (bitsize == 32 || bitsize == 64) {
        if (!fastmem_marker && args[3].GetImmediateAccType() == IR::AccType::VEC) {
            const auto vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
            const auto temporary = ctx.reg_alloc.ScratchGpr(code);
            const auto vector = ctx.reg_alloc.UseXmm(code, args[2]);
            const auto wrapped_fn = write_fallbacks[std::make_tuple(false, bitsize, vaddr.getIdx(), temporary.getIdx())];
            SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
            const auto pointer = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
            if constexpr (bitsize == 32) code.movd(code.dword[pointer], vector);
            else code.movq(code.qword[pointer], vector);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*abort);
                if constexpr (bitsize == 32) code.movd(temporary.cvt32(), vector);
                else code.movq(temporary, vector);
                code.call(wrapped_fn);
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
            });
            code.L(*end);

            return;
        }
    }

    if (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128
                            ? ctx.reg_alloc.UseXmm(code, args[2]).getIdx()
                            : (ordered ? ctx.reg_alloc.UseScratchGpr(code, args[2]).getIdx() : ctx.reg_alloc.UseGpr(code, args[2]).getIdx());

    const auto wrapped_fn = write_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value_idx)];

    SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();

    if (fastmem_marker) {
        // Use fastmem
#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5
        if constexpr (checked_fastmem) {
            const Xbyak::Reg64 index = ctx.reg_alloc.ScratchGpr(code);
            SharedLabel blocked = ctx.GenSharedLabel();
            EmitCheckedFastmemTest(code, vaddr, index, checked_write_blocked, *blocked);
            const auto location = EmitWriteMemoryMov<bitsize>(code, r13 + vaddr, value_idx, ordered);
            ctx.deferred_emits.emplace_back([=, this, &ctx] {
                code.L(*blocked);
                const auto fallback_ptr = EmitCheckedFallbackLookup(code, ctx, bitsize, *abort, vaddr, index);
                EmitWriteMemoryMov<bitsize>(code, fallback_ptr, value_idx, ordered);
                code.jmp(*end, code.T_NEAR);
                code.L(*abort);
                code.call(wrapped_fn);
                AddFastmemPatch(location, code.getCurr(), wrapped_fn, *fastmem_marker, conf.recompile_on_fastmem_failure);
                EmitCheckMemoryAbort(ctx, inst, end);
                code.jmp(*end, code.T_NEAR);
            });
        } else {
#endif // PS5
        bool require_abort_handling = false;
        const auto dest_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        const auto location = EmitWriteMemoryMov<bitsize>(code, dest_ptr, value_idx, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);

            AddFastmemPatch(location, code.getCurr(), wrapped_fn, *fastmem_marker, conf.recompile_on_fastmem_failure);

            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
#else // PS5
        }
#endif // PS5
    } else {
        // Use page table
        ASSERT(conf.page_table);
        const auto dest_ptr = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
        EmitWriteMemoryMov<bitsize>(code, dest_ptr, value_idx, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    }
    code.L(*end);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveReadMemory(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor != nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    if constexpr (bitsize != 128) {
        using T = mcl::unsigned_integer_of_size<bitsize>;

        ctx.reg_alloc.HostCall(code, inst, {}, args[1]);

        code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
        EDEN_CONF_ARG();
        if (ordered) {
            code.mfence();
        }
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr) -> T {
            return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
                return (conf.callbacks->*callback)(vaddr, bitsize / CHAR_BIT);
            });
        });
        code.ZeroExtendFrom(bitsize, code.ABI_RETURN);
    } else {
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
        ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.HostCall(code, nullptr);

        code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
        EDEN_CONF_ARG();
        ctx.reg_alloc.AllocStackSpace(code, 16 + ABI_SHADOW_SPACE);
        code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
        if (ordered) {
            code.mfence();
        }
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, Vector& ret) {
            ret = conf.global_monitor->ReadAndMark<Vector>(conf.processor_id, vaddr, [&]() -> Vector {
                return (conf.callbacks->*callback)(vaddr);
            });
        });
        code.movups(result, xword[rsp + ABI_SHADOW_SPACE]);
        ctx.reg_alloc.ReleaseStackSpace(code, 16 + ABI_SHADOW_SPACE);

        ctx.reg_alloc.DefineValue(code, inst, result);
    }

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveWriteMemory(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor != nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
        ctx.reg_alloc.Use(code, args[2], HostLoc::XMM1);
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.HostCall(code, inst);
    } else {
        ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
    }

    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    Xbyak::Label end;
    code.mov(code.ABI_RETURN, u32(1));
    code.movzx(tmp.cvt32(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.test(tmp.cvt8(), tmp.cvt8());
    code.je(end);
    code.xor_(tmp.cvt32(), tmp.cvt32());
    code.xchg(tmp.cvt8(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    EDEN_CONF_ARG();
    if constexpr (bitsize != 128) {
        using T = mcl::unsigned_integer_of_size<bitsize>;
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, T value) -> u32 {
            return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr, [&](T expected) -> bool {
                return (conf.callbacks->*callback)(vaddr, value, expected);
            }) ? 0 : 1;
        });
        if (ordered) {
            code.mfence();
        }
    } else {
        ctx.reg_alloc.AllocStackSpace(code, 16 + ABI_SHADOW_SPACE);
        code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
        code.movaps(xword[code.ABI_PARAM3], xmm1);
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, Vector& value) -> u32 {
            return conf.global_monitor->DoExclusiveOperation<Vector>(conf.processor_id, vaddr, [&](Vector expected) -> bool {
                return (conf.callbacks->*callback)(vaddr, value, expected);
            }) ? 0 : 1;
        });
        if (ordered) {
            code.mfence();
        }
        ctx.reg_alloc.ReleaseStackSpace(code, 16 + ABI_SHADOW_SPACE);
    }
    code.L(end);

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveReadMemoryInline(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor && (conf.fastmem_pointer || conf.page_table));
    if (!conf.page_table && !exception_handler.SupportsFastmem()) {
        EmitExclusiveReadMemory<bitsize, callback>(ctx, inst);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    constexpr bool ordered = true;

    if constexpr (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128 ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();
    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp2 = ctx.reg_alloc.ScratchGpr(code);

    const auto wrapped_fn = read_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value_idx)];

    EmitExclusiveLock(code, conf, tmp, tmp2.cvt32());

    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
    EDEN_EXCLUSIVE_ADDRESS(tmp);
    code.mov(qword[tmp], vaddr);

#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
    const auto fastmem_marker = ShouldFastmem(ctx, inst);
#else // PS5
    const auto fastmem_marker = checked_fastmem ? decltype(ShouldFastmem(ctx, inst)){} : ShouldFastmem(ctx, inst);
#endif // PS5
    if (fastmem_marker || conf.page_table) {
        SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        bool require_abort_handling = !fastmem_marker;

        const auto src_ptr = fastmem_marker ? EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling) : EmitExclusiveVAddrLookup(code, ctx, bitsize, *abort, vaddr, tmp2, tmp);

        const auto location = EmitReadMemoryMov<bitsize>(code, value_idx, src_ptr, ordered);

        if (fastmem_marker) {
            AddFastmemPatch(location, code.getCurr(), wrapped_fn, *fastmem_marker,
                            conf.recompile_on_exclusive_fastmem_failure);
        }

        code.L(*end);

        if (require_abort_handling) {
            ctx.deferred_emits.emplace_back([=, this] {
                code.L(*abort);
                code.call(wrapped_fn);
                code.jmp(*end, code.T_NEAR);
            });
        }
    } else {
        code.call(wrapped_fn);
    }

    EDEN_EXCLUSIVE_VALUE(tmp);
    EmitWriteMemoryMov<bitsize>(code, tmp, value_idx, false);

    EmitExclusiveUnlock(code, conf, tmp, tmp2.cvt32());

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
    } else {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
    }

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveWriteMemoryInline(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor && (conf.fastmem_pointer || conf.page_table));
    if (!conf.page_table && !exception_handler.SupportsFastmem()) {
        EmitExclusiveWriteMemory<bitsize, callback>(ctx, inst);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    constexpr bool ordered = true;

    const auto value = [&] {
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
            return ctx.reg_alloc.UseXmm(code, args[2]);
        } else {
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
            return ctx.reg_alloc.UseGpr(code, args[2]);
        }
    }();
    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const Xbyak::Reg32 status = ctx.reg_alloc.ScratchGpr(code).cvt32();
    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp2 = ctx.reg_alloc.ScratchGpr(code);

    const auto wrapped_fn = exclusive_write_fallbacks[std::make_tuple(ordered, bitsize, vaddr.getIdx(), value.getIdx())];

    EmitExclusiveLock(code, conf, tmp, tmp2.cvt32());

    SharedLabel end = ctx.GenSharedLabel();

    code.mov(status, u32(1));
    code.movzx(tmp.cvt32(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.test(tmp.cvt8(), tmp.cvt8());
    code.je(*end, code.T_NEAR);
    EDEN_EXCLUSIVE_ADDRESS(tmp);
    code.cmp(qword[tmp], vaddr);
    code.jne(*end, code.T_NEAR);

    EmitExclusiveTestAndClear(code, conf, vaddr, tmp, rax);

    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(0));
    EDEN_EXCLUSIVE_VALUE(tmp);

    if constexpr (bitsize == 128) {
        code.mov(rax, qword[tmp + 0]);
        code.mov(rdx, qword[tmp + 8]);
        if (code.HasHostFeature(HostFeature::SSE41)) {
            code.movq(rbx, value);
            code.pextrq(rcx, value, 1);
        } else {
            code.movaps(xmm0, value);
            code.movq(rbx, xmm0);
            code.punpckhqdq(xmm0, xmm0);
            code.movq(rcx, xmm0);
        }
    } else {
        EmitReadMemoryMov<bitsize>(code, rax.getIdx(), tmp, false);
    }

#if !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
    const auto fastmem_marker = ShouldFastmem(ctx, inst);
#else // PS5
    const auto fastmem_marker = checked_fastmem ? decltype(ShouldFastmem(ctx, inst)){} : ShouldFastmem(ctx, inst);
#endif // PS5
    if (fastmem_marker || conf.page_table) {
        SharedLabel abort = ctx.GenSharedLabel();
        bool require_abort_handling = !fastmem_marker;

        const auto dest_ptr = fastmem_marker ? EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling, tmp) : EmitExclusiveVAddrLookup(code, ctx, bitsize, *abort, vaddr, tmp2, tmp);

        const auto location = code.getCurr();

        switch (bitsize) {
        case 8:
            code.lock();
            code.cmpxchg(code.byte[dest_ptr], value.cvt8());
            break;
        case 16:
            code.lock();
            code.cmpxchg(word[dest_ptr], value.cvt16());
            break;
        case 32:
            code.lock();
            code.cmpxchg(dword[dest_ptr], value.cvt32());
            break;
        case 64:
            code.lock();
            code.cmpxchg(qword[dest_ptr], value.cvt64());
            break;
        case 128:
            code.lock();
            code.cmpxchg16b(ptr[dest_ptr]);
            break;
        default:
            UNREACHABLE();
        }

        code.setnz(status.cvt8());
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*abort);
            code.call(wrapped_fn);

            if (fastmem_marker) AddFastmemPatch(location, code.getCurr(), wrapped_fn, *fastmem_marker, conf.recompile_on_exclusive_fastmem_failure);

            code.xor_(status.cvt32(), status.cvt32()); //dep-break
            code.test(code.al, code.al);
            code.setz(status.cvt8());
            code.jmp(*end, code.T_NEAR);
        });
    } else {
        code.call(wrapped_fn);
        code.xor_(status.cvt32(), status.cvt32()); //dep-break
        code.test(code.al, code.al);
        code.setz(status.cvt8());
    }

    code.L(*end);
    EmitExclusiveUnlock(code, conf, tmp, eax);
    ctx.reg_alloc.DefineValue(code, inst, status);
    EmitCheckMemoryAbort(ctx, inst);
}

#undef AxxEmitX64
#undef AxxEmitContext
#undef AxxJitState
#undef AxxUserConfig
