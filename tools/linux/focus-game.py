#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Temporarily select a BC250 test window for native Gamescope screenshots."""

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


BASE_WINDOW = "GAMESCOPECTRL_BASELAYER_WINDOW"
TEST_APP_ID = 98515001


def xprop(display, *arguments):
    env = os.environ.copy()
    env["DISPLAY"] = display
    result = subprocess.run(
        ["xprop", *arguments], env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=3,
    )
    if result.returncode:
        raise RuntimeError(result.stdout.strip() or "xprop failed")
    return result.stdout.strip()


def cardinal_value(raw):
    if "=" not in raw:
        return None
    if "(CARDINAL)" not in raw:
        raise ValueError(f"expected a CARDINAL property, got: {raw}")
    return tuple(int(value.strip(), 0) for value in raw.split("=", 1)[1].split(","))


def root_property(atom):
    return xprop(":0", "-root", atom)


def write_property(display, target, atom, value):
    if value is None:
        xprop(display, *target, "-remove", atom)
    else:
        text = ", ".join(str(number) for number in value)
        xprop(display, *target, "-f", atom, "32c", "-set", atom, text)


def process_identity(pid):
    try:
        fields = (Path("/proc") / str(pid) / "stat").read_text().split(") ", 1)[1].split()
        return None if fields[0] == "Z" else fields[19]  # Linux start-time field.
    except (OSError, IndexError):
        return None


class FocusSelection:
    def __init__(self, pid, window, output):
        self.pid = pid
        self.window = window
        self.output = output
        self.identity = process_identity(pid)
        self.original_window = root_property(BASE_WINDOW)
        self.original_game = xprop(":1", "-id", hex(window), "STEAM_GAME")
        self.window_value = cardinal_value(self.original_window)
        self.game_value = cardinal_value(self.original_game)
        self.state = {
            "pid": pid, "window": window,
            "original_base_window": self.original_window,
            "original_steam_game": self.original_game,
            "original_appids": root_property("GAMESCOPECTRL_BASELAYER_APPID"),
        }

    def save(self):
        (self.output / "focus-state.json").write_text(json.dumps(self.state, indent=2) + "\n")

    def alive(self):
        return self.identity is not None and process_identity(self.pid) == self.identity

    def select(self):
        if not self.alive():
            raise RuntimeError("game ended before the focus request")
        self.save()
        write_property(":1", ("-id", hex(self.window)), "STEAM_GAME", (TEST_APP_ID,))
        write_property(":0", ("-root",), BASE_WINDOW, (self.window,))
        time.sleep(1)
        self.state["selected_window"] = root_property("GAMESCOPE_FOCUSED_WINDOW")
        self.state["selected_app"] = root_property("GAMESCOPE_FOCUSED_APP")
        self.save()

    def restore(self):
        errors = []
        try:
            if cardinal_value(root_property(BASE_WINDOW)) == (self.window,):
                write_property(":0", ("-root",), BASE_WINDOW, self.window_value)
        except (RuntimeError, ValueError, subprocess.SubprocessError) as error:
            errors.append(str(error))
        if self.alive():
            try:
                current = xprop(":1", "-id", hex(self.window), "STEAM_GAME")
                if cardinal_value(current) == (TEST_APP_ID,):
                    write_property(":1", ("-id", hex(self.window)), "STEAM_GAME", self.game_value)
            except (RuntimeError, ValueError, subprocess.SubprocessError) as error:
                # A window can disappear slightly before its process exits.
                if "BadWindow" not in str(error):
                    errors.append(str(error))
        try:
            restored = root_property(BASE_WINDOW)
        except (RuntimeError, subprocess.SubprocessError) as error:
            restored = f"Could not read restored focus: {error}"
            errors.append(str(error))
        (self.output / "focus-restored.txt").write_text(restored + "\n")
        if errors:
            raise RuntimeError("focus restoration failed: " + "; ".join(errors))


def stop(_signum, _frame):
    raise SystemExit(0)


def positive_id(value):
    number = int(value, 0)
    if not 0 < number <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("expected a positive 32-bit identifier")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=positive_id)
    parser.add_argument("window", type=positive_id)
    parser.add_argument("tag")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", args.tag):
        parser.error("invalid evidence tag")
    try:
        duration = int(os.environ.get("CAPTURE_SECONDS", "180"))
        if not 10 <= duration <= 3600:
            raise ValueError
    except ValueError:
        parser.error("CAPTURE_SECONDS must be between 10 and 3600")
    output = Path(__file__).resolve().parent / "evidence" / args.tag
    output.mkdir(parents=True, exist_ok=True)
    selection = None
    handlers = {sig: signal.signal(sig, stop) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        selection = FocusSelection(args.pid, args.window, output)
        selection.select()
        env = os.environ | {"XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}", "WAYLAND_DISPLAY": "gamescope-0"}
        subprocess.run(
            ["gamescopectl", "screenshot", str(output / "focus-confirmed.png"), "1"],
            env=env, check=True, timeout=10,
        )
        deadline = time.monotonic() + max(180, duration + 15)
        while selection.alive() and time.monotonic() < deadline:
            time.sleep(1)
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        for sig in handlers:
            signal.signal(sig, signal.SIG_IGN)
        restoration_failed = False
        try:
            if selection is not None:
                selection.restore()
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            print(error, file=sys.stderr)
            restoration_failed = True
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
        if restoration_failed:
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
