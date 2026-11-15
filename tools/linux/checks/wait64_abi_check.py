#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and exercise the source tree's actual WaitForAddress ABI wrappers.

Extracted code comes from the supplied GPL-3.0-or-later Eden/Eglantine tree.
The harness provides callback stubs at the kernel-operation boundary, then runs
both real generated wrappers, argument conversion helpers and arbiter dispatch.
It checks register ABI and comparison width; it does not simulate thread sleep,
guest memory access or permissions. Those need integration/guest execution tests.

Usage: wait64_abi_check.py SOURCE [--negative-control]
The negative control mutates only the temporary C++ translation unit to restore
the old AArch64 truncation. Success requires the compiled check to reject it.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("--negative-control", action="store_true")
args = parser.parse_args()
source = args.source.resolve()
def function(text, signature):
 start=text.index(signature); brace=text.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
svc=(source/'src/core/hle/kernel/svc.cpp').read_text()
# Check the generator's register-allocation model without emitting source files.
import runpy
model=runpy.run_path(str(source/'tools/svc_generator.py'))
declaration=next(decl for number,decl in model['SVCS'] if number==0x34)
for bitness,value_type,timeout_regs in ((0,'int32_t',[3,4]),(1,'int64_t',[3])):
    regs=model['get_registers'](model['parse_declaration'](declaration,bitness),bitness)[2]
    assert regs[2]==[value_type,'value',[2]], ('Generator comparison-value ABI', regs)
    assert regs[3]==['int64_t','timeout_ns',timeout_regs], ('Generator timeout ABI', regs)
header=(source/'src/core/hle/kernel/svc_types.h').read_text()
arbiter=(source/'src/core/hle/kernel/k_address_arbiter.h').read_text()
validators=(source/'src/core/hle/kernel/svc/svc_address_arbiter.cpp').read_text()
helpers=svc[svc.index('static uint32_t GetArg32'):svc.index('static_assert')]
wrappers='\n'.join(function(svc,'static void '+name) for name in ('SvcWrap_WaitForAddress64(', 'SvcWrap_WaitForAddress64From32('))
enum=function(header,'enum class ArbitrationType')+';'
validation=function(validators,'constexpr bool IsValidArbitrationType')
dispatch=function(arbiter,'Result WaitForAddress(uint64_t addr')
program=r'''
#include <array>
#include <span>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
namespace Core { struct System {}; }
using u32=uint32_t;using u64=uint64_t;using s64=int64_t;using s32=int32_t;using Result=uint32_t;
#define R_RETURN(x) return(x)
#define UNREACHABLE() std::abort()
#define CHECK(x) do { if(!(x)) {std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);std::exit(1);} }while(0)
namespace Kernel::Svc {
ENUM
struct Record{uint64_t address;ArbitrationType type;int64_t value;int64_t timeout;bool a32;} record{};
Result WaitForAddress64(Core::System&,uint64_t a,ArbitrationType t,int64_t v,int64_t ns){record={a,t,v,ns,false};return 0x1234;}
Result WaitForAddress64From32(Core::System&,uint32_t a,ArbitrationType t,int32_t v,int64_t ns){record={a,t,v,ns,true};return 0x5678;}
HELPERS
WRAPPERS
VALIDATION
}
namespace Kernel {
struct DispatchRecord{u64 address;s64 value;s64 timeout;bool wide;bool decrement;} dispatch{};
struct Probe {
 Result WaitIfLessThan(u64 a,s32 v,bool d,s64 t){dispatch={a,v,t,false,d};return 1;}
 Result WaitIfEqual(u64 a,s64 v,s64 t,bool w){dispatch={a,v,t,w,false};return 2;}
 DISPATCH
};
}
int main(){
 using namespace Kernel::Svc;
 Core::System system;
 std::array<uint64_t,8> regs{};
 for(uint64_t expected: {0x0123456789abcdefULL,0xfedcba9876543210ULL,0x0000000100000000ULL}){
  regs={0x1234567890ULL,3,expected,0x0123456789abcdefULL,0x9876,0,0,0};
  SvcWrap_WaitForAddress64(system,regs);
  CHECK(!record.a32);CHECK(record.type==ArbitrationType::WaitIfEqual64);CHECK(record.address==0x1234567890ULL);CHECK(uint64_t(record.value)==expected);
  CHECK(record.timeout==0x0123456789abcdefLL);CHECK(regs[0]==0x1234);CHECK(regs[4]==0x9876);
 }
 regs={0xdeadbeef00001000ULL,2,0xdeadbeefffffffffULL,0x89abcdef,0x01234567,0xdead,0,0};
 SvcWrap_WaitForAddress64From32(system,regs);
 CHECK(record.a32);CHECK(record.type==ArbitrationType::WaitIfEqual);CHECK(record.address==0x1000);CHECK(record.value==-1);
 CHECK(record.timeout==0x0123456789abcdefLL);CHECK(regs[0]==0x5678);CHECK(regs[5]==0xdead);
 CHECK(IsValidArbitrationType(ArbitrationType::WaitIfEqual64));
 CHECK(!IsValidArbitrationType(static_cast<ArbitrationType>(4)));
 Kernel::Probe p;
 p.WaitForAddress(0x1000,ArbitrationType::WaitIfEqual64,0x12345678ffffffffLL,0);
 CHECK(Kernel::dispatch.wide);CHECK(Kernel::dispatch.value==0x12345678ffffffffLL);
 p.WaitForAddress(0x1004,ArbitrationType::WaitIfEqual,0x12345678ffffffffLL,0);
 CHECK(!Kernel::dispatch.wide);CHECK(Kernel::dispatch.value==-1);
 p.WaitForAddress(0x1004,ArbitrationType::DecrementAndWaitIfLessThan,0x12345678ffffffffLL,0);
 CHECK(Kernel::dispatch.decrement);CHECK(Kernel::dispatch.value==-1);
 p.WaitForAddress(0x1004,ArbitrationType::WaitIfLessThan,0x1234567880000000LL,0);
 CHECK(!Kernel::dispatch.decrement);CHECK(Kernel::dispatch.value==int64_t(INT32_MIN));
 std::puts("WaitForAddress actual-wrapper ABI + dispatch checks PASS (A64 high bits, A32 register layout, legacy narrowing)");
}
'''
for key,value in [('ENUM',enum),('HELPERS',helpers),('WRAPPERS',wrappers),('VALIDATION',validation),('DISPATCH',dispatch)]:
 program=program.replace(key,value)
negative=args.negative_control
if negative:
    old = 'int64_t value = Convert<int64_t>(GetArg64(args, 2));'
    if program.count(old) != 1:
        raise RuntimeError('Cannot uniquely inject the AArch64 truncation negative control')
    program = program.replace(old, 'int32_t value = Convert<int32_t>(GetArg64(args, 2));')
with tempfile.TemporaryDirectory(prefix='oot-wait64-abi-') as d:
 cpp=Path(d)/'check.cpp';exe=Path(d)/'check';cpp.write_text(program)
 subprocess.run([os.environ.get('CXX') or 'clang++','-std=c++20','-O2',str(cpp),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)])
 if negative:
    assert result.returncode!=0, 'Regression negative control unexpectedly passed'
    print('Negative control catches AArch64 value truncation PASS')
 else:
    result.check_returncode()
