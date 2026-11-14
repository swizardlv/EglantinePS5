#!/usr/bin/env python3
"""Compile the production read verifier against host mocks; no emulator dependencies needed."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'Eglantine/src/video_core/shader_environment.cpp').read_text()
verifier = source[source.index('class NativeReadVerification {'):source.index('// Reads a native program')]
registry = (ROOT / 'Eglantine/src/core/hle/service/nvdrv/devices/nx2_stub_device.h').read_text()
lookup = registry[registry.index('    std::optional<u64> TryCpuAddressOfGpu'):registry.index('    std::optional<u64> DeviceAddressOfGpu')]
# Expose counters only in the test translation unit; Read and Enabled compile unchanged.
verifier = verifier.replace('private:', 'public:')
prefix = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
using u8 = uint8_t;
using u64 = uint64_t;
using GPUVAddr = u64;
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
namespace fmt { template<class... T> std::string format(const char*, T&&...) { return "report"; } }
namespace Common {
struct ProcessAddress { u64 value; };
inline const char* g_scm_rev = "host-test";
u64 CityHash64(const char* bytes, size_t size) { return size ? u8(bytes[0]) : 0; }
}
namespace Shader::Ampere {
struct Instruction { u64 low, high; };
u64 NativeProgramKey(std::span<const Instruction>) { return 1; }
}
struct CpuMemory {
    // Deliberately not dereferenceable: the verifier must compare identity only.
    const u8* pointer = reinterpret_cast<const u8*>(1);
    unsigned lookups{};
    const u8* GetPointerSilent(Common::ProcessAddress) { ++lookups; return pointer; }
};
struct Process { CpuMemory memory; CpuMemory& GetMemory() { return memory; } };
struct System {
    u64 title = 0x0400245021D2C000;
    Process process;
    u64 GetApplicationProcessProgramID() { return title; }
    Process* ApplicationProcess() { return &process; }
};
namespace Tegra {
struct MemoryManager {
    System system;
    std::array<u8, 0x2000> before{}, after{};
    const u8* pointer = reinterpret_cast<const u8*>(1);
    std::optional<u64> device = 0x3000;
    unsigned safe_reads{}, unsafe_reads{}, pointer_reads{}, translations{};
    std::function<void()> on_read;
    System& GetSystem() { return system; }
    const u8* GetPointer(u64) { ++pointer_reads; return pointer; }
    std::optional<u64> GpuToCpuAddress(u64) { ++translations; return device; }
    void ReadBlockUnsafe(u64, void* dest, size_t size) {
        ++unsafe_reads; std::memcpy(dest, before.data(), size);
    }
    void ReadBlock(u64, void* dest, size_t size) {
        ++safe_reads; if (on_read) on_read(); std::memcpy(dest, after.data(), size);
    }
};
}
namespace Service::Nvidia::Devices {
struct Nx2GpuMemoryRegistry {
    struct Mutex {
        bool busy{};
        bool try_lock() { return !busy; }
        void unlock() {}
    } mutex;
    struct Mapping { u64 gpu_address, cpu_address, size; };
    std::vector<Mapping> mappings{{0x1000, 0x7000, 0x1000}};
    static Nx2GpuMemoryRegistry& Get() { static Nx2GpuMemoryRegistry r; return r; }
'''
suffix = r'''
};
}
'''
tests = r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    Tegra::MemoryManager memory;
    std::array<u8, 0x2000> destination{};
    NativeReadVerification verify{0x1000};
    if (std::string_view(argv[1]) == "off" || std::string_view(argv[1]) == "other") {
        if (std::string_view(argv[1]) == "other") memory.system.title = 123;
        assert(!NativeReadVerification::Enabled(memory));
        verify.Read(memory, 0x1000, destination.data(), 0x1000);
        assert(memory.safe_reads == 1 && memory.unsafe_reads == 0);
        assert(memory.pointer_reads == 0 && memory.translations == 0);
        assert(memory.system.process.memory.lookups == 0);
        return 0;
    }
    assert(NativeReadVerification::Enabled(memory));
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    bool busy{};
    assert(registry.TryCpuAddressOfGpu(0x1010, 16, busy) == 0x7010 && !busy);
    assert(!registry.TryCpuAddressOfGpu(0x1000, 0, busy));
    assert(!registry.TryCpuAddressOfGpu(0x2000, 16, busy));
    assert(!registry.TryCpuAddressOfGpu(0xff0, 16, busy));
    assert(!registry.TryCpuAddressOfGpu(0x1ff0, 32, busy));
    assert(!registry.TryCpuAddressOfGpu(0x1010, UINT64_MAX, busy));
    registry.mutex.busy = true;
    assert(!registry.TryCpuAddressOfGpu(0x1000, 16, busy) && busy);
    registry.mutex.busy = false;
    auto& totals = NativeReadVerification::totals;
    memory.before.fill(0x21); memory.after.fill(0x21);
    const auto read = [&] { verify.Read(memory, 0x1000, destination.data(), 0x1000); };
    read(); // Identical mappings and bytes.
    assert(totals.checked == 1 && totals.pointer_mismatch == 0 && totals.read_changed == 0);
    assert(destination[0] == 0x21 && memory.safe_reads == 1 && memory.unsafe_reads == 1);
    memory.system.process.memory.pointer = reinterpret_cast<const u8*>(2);
    read(); // Stable divergent CPU/GPU pointers, but unchanged bytes.
    assert(totals.pointer_mismatch == 1 && totals.mapping_changed == 0 && totals.read_changed == 0);
    memory.system.process.memory.pointer = memory.pointer;
    memory.after.fill(0);
    read(); // Existing safe read replaces bytes with zero.
    assert(totals.read_changed == 1 && totals.zero == 1 && destination[0] == 0);
    memory.before.fill(0);
    read(); // Zero was already present before the safe read.
    assert(totals.read_changed == 1 && totals.zero == 2);
    memory.on_read = [&] {
        memory.pointer = reinterpret_cast<const u8*>(3);
        memory.system.process.memory.pointer = memory.pointer;
        memory.device = 0x4000;
    };
    read(); // Mapping changes during the existing safe read.
    assert(totals.mapping_changed == 1 && totals.pointer_mismatch == 1);
    memory.on_read = {};
    registry.mutex.busy = true;
    read();
    assert(totals.busy == 1 && totals.unknown == 1 && totals.mapping_changed == 1);
    registry.mutex.busy = false;
    registry.mappings.clear();
    read();
    assert(totals.unknown == 2 && totals.busy == 1);
    registry.mappings.push_back({0x1000, 0x7000, 0x1000});
    for (int i = 0; i < 100; ++i) read();
    memory.system.process.memory.pointer = reinterpret_cast<const u8*>(4);
    for (int i = 0; i < 100; ++i) read();
    assert(verify.reports.size() == 32); // Zero-only pages cannot consume mismatch quota.
    unsigned unsafe_before = memory.unsafe_reads;
    unsigned safe_before = memory.safe_reads;
    verify.Read(memory, 0x1000, destination.data(), 0);
    verify.Read(memory, 0x1000, destination.data(), 0x1001);
    verify.Read(memory, 0x1ff0, destination.data(), 32);
    assert(memory.unsafe_reads == unsafe_before && memory.safe_reads == safe_before + 3);
    totals.attempts = 65535;
    read();
    assert(memory.unsafe_reads == unsafe_before + 1);
    read();
    assert(memory.unsafe_reads == unsafe_before + 1); // Budget exhausted, safe read still happens.
    verify.Finish({});
    assert(verify.reports.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='shader-read-verify-') as directory:
    cpp = Path(directory) / 'verify.cpp'
    binary = Path(directory) / 'verify'
    cpp.write_text(prefix + lookup + suffix + verifier + tests)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-Wno-unused-parameter', str(cpp), '-o', str(binary)], check=True)
    for value, mode in [(None, 'off'), ('0', 'off'), ('true', 'off'), ('1', 'other'), ('1', 'on')]:
        env = os.environ.copy()
        env.pop('EDEN_NX2_SHADER_READ_VERIFY', None)
        if value is not None:
            env['EDEN_NX2_SHADER_READ_VERIFY'] = value
        subprocess.run([str(binary), mode], env=env, check=True)
print('PASS: production verifier and registry lookup; disabled/title gates, read ordering, pointer/byte classification, bounds, quotas and budget')
