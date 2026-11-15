#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import csv
import json
import os
import pathlib
import re
import resource
import subprocess
import sys
import time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
parser = argparse.ArgumentParser(
    description="Bounded BC250 Mario Kart title/idle-attract qualification"
)
parser.add_argument("--seconds", type=int, default=600)
parser.add_argument("--out", type=pathlib.Path, required=True)
parser.add_argument("game", type=pathlib.Path)
args = parser.parse_args()
if args.seconds < 30:
    parser.error("--seconds must be at least 30")
b = pathlib.Path(__file__).resolve().parent
o = args.out.expanduser().resolve()
if os.environ.get("BC250_RELAY_HELD") != "1":
    os.execvpe(
        "bash",
        [
            "bash",
            str(b / "bc250-relay.sh"),
            sys.executable,
            str(pathlib.Path(__file__).resolve()),
            *sys.argv[1:],
        ],
        os.environ,
    )
o.mkdir(parents=True, exist_ok=False)
launcher = b / "launch-mkw-bc250.sh"

env = os.environ.copy()
env.update(DISPLAY=":1", CAPTURE_SECONDS=str(args.seconds))
start = time.monotonic()
wall = time.time()
hz = os.sysconf("SC_CLK_TCK")
pg = os.sysconf("SC_PAGE_SIZE")
prev = {}
rows = []
shots = []
focus = None
with (o / "session.txt").open("w") as log:
    proc = subprocess.Popen(
        [
            "/usr/bin/time",
            "-v",
            "timeout",
            "--signal=INT",
            "--kill-after=10s",
            str(args.seconds) + "s",
            str(launcher),
            str(args.game.expanduser().resolve()),
        ],
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    while proc.poll() is None:
        now = time.monotonic()
        elapsed = now - start
        rss = cpu = gpu = 0
        vram = gtt = 0
        pids = []
        engine_valid = False
        for d in pathlib.Path("/proc").glob("[0-9]*"):
            try:
                if (d / "exe").resolve().name != "eden-cli":
                    continue
                parent = int(d.name)
                while parent > 1 and parent != proc.pid:
                    parent = int(
                        (pathlib.Path("/proc") / str(parent) / "stat")
                        .read_text()
                        .split(") ", 1)[1]
                        .split()[1]
                    )
                if parent != proc.pid:
                    continue
                st = (d / "stat").read_text().split(") ", 1)[1].split()
                ct = (int(st[11]) + int(st[12])) / hz
                rss += int(st[21]) * pg
                pids.append(d.name)
                clients = set()
                eng = 0
                for fd in (d / "fdinfo").iterdir():
                    t = fd.read_text()
                    m = re.search(r"drm-client-id:\s*(\d+)", t)
                    if not m or m[1] in clients:
                        continue
                    clients.add(m[1])
                    z = re.search(r"drm-engine-gfx:\s*(\d+) ns", t)
                    if z:
                        eng += int(z[1])
                        engine_valid = True
                    for k in ["vram", "gtt"]:
                        z = re.search(r"drm-memory-" + k + r":\s*(\d+) KiB", t)
                        if z:
                            if k == "vram":
                                vram += int(z[1]) * 1024
                            else:
                                gtt += int(z[1]) * 1024
                if d.name in prev:
                    old = prev[d.name]
                    cpu += (ct - old[0]) / (now - old[2]) * 100
                    if engine_valid:
                        gpu += (eng - old[1]) / 1e9 / (now - old[2]) * 100
                prev[d.name] = (ct, eng, now)
            except (OSError, ValueError):
                pass
        mi = {
            k: int(v.split()[0]) * 1024
            for k, v in (
                x.split(":", 1)
                for x in pathlib.Path("/proc/meminfo").read_text().splitlines()
            )
        }
        rows.append(
            {
                "seconds": round(elapsed, 2),
                "pids": ";".join(pids),
                "rss_bytes": rss,
                "cpu_percent_one_core": round(cpu, 2),
                "gpu_gfx_engine_percent": round(gpu, 2) if engine_valid else "",
                "emulator_vram_bytes": vram,
                "emulator_gtt_bytes": gtt,
                "system_available_bytes": mi["MemAvailable"],
                "swap_used_bytes": mi["SwapTotal"] - mi["SwapFree"],
            }
        )
        with (o / "telemetry.csv").open("w") as f:
            w = csv.DictWriter(f, fieldnames=rows[0].keys())
            w.writeheader()
            w.writerows(rows)
        if focus is None and pids:
            q = subprocess.run(
                ["xdotool", "search", "--onlyvisible", "--pid", pids[0]],
                env=env,
                check=False,
                capture_output=True,
                text=True,
            )
            if q.returncode == 0 and q.stdout.strip():
                focus = subprocess.Popen(
                    [
                        "python3",
                        str(b / "focus-mkw-bc250.py"),
                        pids[0],
                        q.stdout.splitlines()[0],
                        str(o),
                    ],
                    env=env,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )
        for sec in sorted(
            {30, 60, 120, 180, 240, 300, 360, 420, 480, 540, args.seconds - 10}
        ):
            if elapsed >= sec and sec not in shots:
                se = env.copy()
                se.update(
                    XDG_RUNTIME_DIR="/run/user/1000", WAYLAND_DISPLAY="gamescope-0"
                )
                try:
                    subprocess.run(
                        [
                            "gamescopectl",
                            "screenshot",
                            str(o / f"frame-{sec:03}s.png"),
                            "1",
                        ],
                        env=se,
                        check=False,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        timeout=10,
                    )
                except subprocess.TimeoutExpired:
                    log.write("Screenshot timeout\n")
                shots.append(sec)
        time.sleep(1)
    code = proc.wait()
    if focus:
        try:
            focus.wait(timeout=5)
        except subprocess.TimeoutExpired:
            focus.terminate()
            focus.wait()
(o / "run.json").write_text(
    json.dumps(
        {
            "exit_status": code,
            "duration_seconds": time.monotonic() - start,
            "start_epoch": wall,
            "samples": len(rows),
            "screenshots": shots,
            "layout_gib": 12,
            "resolution": "0.5x",
            "sparse_override": 1,
            "texture_cache_expected_mib": 1024,
            "texture_cache_critical_mib": 1536,
        },
        indent=2,
    )
)

raise SystemExit(0 if code == 124 else code or 1)
