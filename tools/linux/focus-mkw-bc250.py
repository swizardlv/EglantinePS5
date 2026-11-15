#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# BC250 Gamescope focus helper; accepts a PID, X11 window, and output directory.
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

pid = int(sys.argv[1])
win = int(sys.argv[2])
tag = sys.argv[3]
base = pathlib.Path(tag).expanduser().resolve()
base.mkdir(parents=True, exist_ok=True)


def xp(display, *args):
    e = os.environ.copy()
    e["DISPLAY"] = display
    return subprocess.run(
        ["xprop", *args],
        env=e,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def read(atom):
    return xp(":0", "-root", atom).stdout.strip()


def setroot(atom, value):
    return xp(":0", "-root", "-f", atom, "32c", "-set", atom, value)


atom = "GAMESCOPECTRL_BASELAYER_WINDOW"
old = read(atom)
oldvalue = old.split("=", 1)[1].strip() if "=" in old else None
state = {
    "pid": pid,
    "window": win,
    "original_base_window": old,
    "original_appids": read("GAMESCOPECTRL_BASELAYER_APPID"),
}
(base / "focus-state.json").write_text(json.dumps(state, indent=2) + "\n")
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
try:
    if not pathlib.Path("/proc", str(pid)).exists():
        raise SystemExit("Game ended before focus request")
    xp(
        ":1",
        "-id",
        hex(win),
        "-f",
        "STEAM_GAME",
        "32c",
        "-set",
        "STEAM_GAME",
        "98515001",
    )
    setroot(atom, str(win))
    time.sleep(1)
    state["selected_window"] = read("GAMESCOPE_FOCUSED_WINDOW")
    state["selected_app"] = read("GAMESCOPE_FOCUSED_APP")
    (base / "focus-state.json").write_text(json.dumps(state, indent=2) + "\n")
    e = os.environ.copy()
    e["XDG_RUNTIME_DIR"] = "/run/user/" + str(os.getuid())
    e["WAYLAND_DISPLAY"] = "gamescope-0"
    subprocess.run(
        ["gamescopectl", "screenshot", str(base / "focus-confirmed.png"), "1"],
        env=e,
        check=True,
    )
    deadline = time.monotonic() + max(
        180, int(os.environ.get("CAPTURE_SECONDS", "180")) + 15
    )
    while pathlib.Path("/proc", str(pid)).exists() and time.monotonic() < deadline:
        time.sleep(1)
finally:
    current = read(atom)
    if "=" in current and current.split("=", 1)[1].strip() == str(win):
        if oldvalue is None:
            xp(":0", "-root", "-remove", atom)
        else:
            setroot(atom, oldvalue)
    if pathlib.Path("/proc", str(pid)).exists():
        xp(":1", "-id", hex(win), "-remove", "STEAM_GAME")
    (base / "focus-restored.txt").write_text(read(atom) + "\n")
