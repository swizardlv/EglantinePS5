# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description="Check archived GPU-drain wait protocols")
parser.add_argument(
    "--patch",
    choices=("blocking-drain.patch", "blocking-drain-both.patch"),
    default="blocking-drain-both.patch",
)
args = parser.parse_args()
fixture = Path(__file__).resolve().parent
temporary = tempfile.TemporaryDirectory(prefix="mkw-drain-check-")
b = Path(temporary.name)
source = b / "src/video_core/gpu_split.cpp"
source.parent.mkdir(parents=True)
shutil.copyfile(fixture / "gpu_split.before.cpp", source)
with (fixture / args.patch).open() as patch:
    subprocess.run(
        ["patch", "--batch", "--fuzz=0", "-p1", "-d", str(b)], stdin=patch, check=True
    )
s = source.read_text()
drain = s.split("void GpuSplit::Drain() {", 1)[1].split(
    "\nvoid GpuSplit::DrainToLastWriter()", 1
)[0]
drain_to = s.split("void GpuSplit::DrainTo(u64 position) {", 1)[1].split(
    "\nvoid GpuSplit::SwitchEngine(", 1
)[0]
helper = s.split("bool BlockingDrainEnabled() {", 1)[1].split("\nvoid Pause()", 1)[0]
store = "consumed.store(read_position, std::memory_order_release);\n        if (BlockingDrainEnabled()) {\n            consumed.notify_all();\n        }"
assert store in s
code = (
    """#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cassert>
#include <iostream>
using u64=unsigned long long; using u32=unsigned;
namespace Nx2Profile { enum class Zone{SplitDrain}; struct Scope { Scope(Zone){} }; }
bool BlockingDrainEnabled() {"""
    + helper
    + """
void Pause(){__builtin_ia32_pause();}
struct GpuSplit {
 std::atomic<u64> consumed{0};u64 consumed_cache{},write_position{};bool sync_pending{};unsigned publish_every=8;
 void CloseRegs(){} void Publish(){} void Drain();void DrainTo(u64);
 void Progress(u64 read_position){"""
    + store
    + """}
};
void GpuSplit::Drain() {"""
    + drain
    + """
void GpuSplit::DrainTo(u64 position) {"""
    + drain_to
    + """
int main(){
 GpuSplit queue;
 for(unsigned iter=1;iter<=4000;++iter){
  u64 mid=iter*32-16, full=iter*32;queue.write_position=full;
  unsigned first=0,last=0;
  if(iter%4==0){ // notification before the waiter begins
   first=iter;last=iter;queue.Progress(full);queue.Drain();assert(first==iter&&last==iter);continue;
  }
  std::jthread back([&]{
   if(iter%3==0)std::this_thread::sleep_for(std::chrono::microseconds(80));
   first=iter;queue.Progress(mid);
   if(iter%5==0)std::this_thread::sleep_for(std::chrono::microseconds(80));
   last=iter;queue.Progress(full);
  });
  queue.DrainTo(mid);assert(queue.consumed.load()>=mid);assert(first==iter);
  queue.Drain();assert(queue.consumed.load()==full);assert(last==iter);
 }
 std::cout<<"PASS: actual Drain/DrainTo and progress notification code; 4000 partial/final, pre-notified and delayed completions\\n";
}
"""
)
(b / "drain-protocol-check.cpp").write_text(code)
subprocess.run(
    [
        "c++",
        "-std=c++20",
        "-O2",
        "-pthread",
        str(b / "drain-protocol-check.cpp"),
        "-o",
        str(b / "drain-protocol-check"),
    ],
    check=True,
)
for mode in ("0", "1"):
    subprocess.run(
        [str(b / "drain-protocol-check")],
        env={**os.environ, "EDEN_NX2_BLOCKING_DRAIN": mode},
        check=True,
        timeout=20,
    )
