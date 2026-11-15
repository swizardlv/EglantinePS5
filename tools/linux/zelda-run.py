#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture a bounded Zelda run on the BC250 with the companion shell helpers."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time


class CaptureInterrupted(Exception):
    def __init__(self, signum):
        self.signum = signum


def valid_tag(value):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", value):
        raise argparse.ArgumentTypeError("use 1–64 letters, digits, underscores or hyphens")
    return value


def capture_duration(value):
    seconds = int(value)
    if not 10 <= seconds <= 3600:
        raise argparse.ArgumentTypeError("duration must be between 10 and 3600 seconds")
    return seconds


def resolve_game(explicit, evidence):
    if explicit is not None:
        game = explicit.expanduser().resolve()
    else:
        receipt = evidence / "zelda-download.json"
        if not receipt.is_file():
            raise ValueError("specify --game /path/to/game.nup (no previous download receipt found)")
        try:
            game = Path(json.loads(receipt.read_text())["path"]).expanduser().resolve()
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise ValueError(f"cannot read game path from {receipt}: {error}") from error
    if not game.is_file():
        raise ValueError(f"game file does not exist: {game}")
    return game


def is_descendant(pid, ancestor):
    for _ in range(32):
        if pid == ancestor:
            return True
        if pid <= 1:
            break
        stat = (Path("/proc") / str(pid) / "stat").read_text()
        pid = int(stat.split(") ", 1)[1].split()[1])
    return False


def find_test_window(capture_pid, executable, gui, env):
    for process in Path("/proc").glob("[0-9]*"):
        try:
            exe = (process / "exe").resolve()
            if gui:
                if exe.name not in ("lichii", "eglantine"):
                    continue
            elif exe != executable:
                continue
            # A different instance of the same binary must never receive test focus.
            if not is_descendant(int(process.name), capture_pid):
                continue
            result = subprocess.run(
                ["xdotool", "search", "--onlyvisible", "--pid", process.name],
                env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=3,
            )
            if result.returncode == 0 and result.stdout.strip():
                return int(process.name), int(result.stdout.splitlines()[0])
        except (OSError, ValueError, subprocess.TimeoutExpired):
            continue
    return None


def stop_capture(process):
    """Stop the private capture group, including the game and its timer."""
    # capture-bc250.sh uses timeout --foreground so its children stay in this group.
    for signum, grace in ((signal.SIGINT, 5), (signal.SIGTERM, 3), (signal.SIGKILL, 1)):
        try:
            os.killpg(process.pid, signum)
        except ProcessLookupError:
            break
        deadline = time.monotonic() + grace
        while time.monotonic() < deadline:
            process.poll()  # Reap the group leader when it exits.
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                process.wait()
                return
            time.sleep(0.1)
    process.wait(timeout=2)


def stop_focus(process):
    if process is None:
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            process.terminate()  # The helper restores X11 properties in its finally block.
        except ProcessLookupError:
            process.wait()
            return
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                process.kill()
            except ProcessLookupError:
                pass
            process.wait()
            print("warning: focus helper required killing; inspect focus-restored.txt", file=sys.stderr)


def interrupt(signum, _frame):
    raise CaptureInterrupted(signum)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", type=valid_tag, help="a new evidence-directory name")
    parser.add_argument("--game", type=Path, help="game file; defaults to a previous local download receipt")
    parser.add_argument("--seconds", type=capture_duration, default=120)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--cli", type=Path, help="test a deployed CLI candidate")
    mode.add_argument("--gui", action="store_true", help="test the installed AppImage GUI")
    parser.add_argument("--focus", action="store_true", help="temporarily select the game in Gamescope")
    parser.add_argument("--fresh-cache", action="store_true")
    args = parser.parse_args(argv)
    base = Path(__file__).resolve().parent
    app = Path(os.environ.get("EDEN_LINUX_APP_DIR", str(Path.home() / "Applications/EglantinePS5"))).expanduser().resolve()
    evidence = base / "evidence"
    try:
        game = resolve_game(args.game, evidence)
    except ValueError as error:
        parser.error(str(error))
    executable = (args.cli or app / "squashfs-root/usr/bin/eden-cli").expanduser().resolve()
    if args.cli and (not executable.is_file() or not os.access(executable, os.X_OK)):
        parser.error(f"CLI candidate is not executable: {executable}")
    output = evidence / args.tag
    if output.exists():
        parser.error(f"evidence directory already exists; choose a new tag: {output}")
    output.mkdir(parents=True)

    env = os.environ.copy()
    env["CAPTURE_SECONDS"] = str(args.seconds)
    env["MANGOHUD"] = "1"
    env["MANGOHUD_CONFIG"] = f"autostart_log=1,log_duration={args.seconds - 5},output_folder={output}"
    if args.cli:
        env["EDEN_LINUX_CLI"] = str(executable)
    if args.fresh_cache:
        (output / "cache").mkdir()
        env["EDEN_LINUX_CACHE_HOME"] = str(output / "cache")
    if args.focus:
        env.update(SDL_VIDEO_DRIVER="x11", DISPLAY=":1")

    started_wall = time.time()
    started = time.monotonic()
    shots = []
    focus = capture = None
    code = None
    interrupted_signal = None
    error = None
    old_handlers = {sig: signal.signal(sig, interrupt) for sig in (signal.SIGINT, signal.SIGTERM)}
    with (output / "session.txt").open("w") as session, (output / "focus-helper.log").open("w") as focuslog:
        try:
            capture = subprocess.Popen(
                [str(base / "capture-bc250.sh"), "appimage" if args.gui else "run", str(game)],
                env=env, stdout=session, stderr=subprocess.STDOUT, start_new_session=True,
            )
            shot_times = [5, 15, 30, 60, 90, 120, 180, 240]
            while capture.poll() is None:
                elapsed = time.monotonic() - started
                if args.focus and focus is None:
                    window = find_test_window(capture.pid, executable, args.gui, env)
                    if window is not None:
                        pid, window_id = window
                        focus = subprocess.Popen(
                            [sys.executable, str(base / "focus-game.py"), str(pid), str(window_id), args.tag],
                            env=env, stdout=focuslog, stderr=subprocess.STDOUT,
                        )
                if shot_times and elapsed >= shot_times[0]:
                    scheduled = shot_times.pop(0)
                    path = output / f"frame-{scheduled:03d}s.png"
                    shotenv = env | {"XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}", "WAYLAND_DISPLAY": "gamescope-0"}
                    result = subprocess.run(
                        ["gamescopectl", "screenshot", str(path), "1"], env=shotenv,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10,
                    )
                    shots.append({"seconds": scheduled, "path": str(path), "request_exit_status": result.returncode,
                                  "output": result.stdout.decode(errors="replace")})
                time.sleep(1)
            code = capture.wait()
        except CaptureInterrupted as interruption:
            interrupted_signal = interruption.signum
            code = 128 + interrupted_signal
        except (OSError, subprocess.SubprocessError) as failure:
            error = str(failure)
            code = 1
        finally:
            # Repeated Ctrl-C must not interrupt restoration or leave the game running.
            for sig in old_handlers:
                signal.signal(sig, signal.SIG_IGN)
            try:
                try:
                    if capture is not None:
                        stop_capture(capture)
                except (OSError, subprocess.SubprocessError) as failure:
                    error = f"capture cleanup failed: {failure}"
                finally:
                    stop_focus(focus)
                if focus is not None and focus.returncode:
                    error = f"focus helper exited {focus.returncode}; inspect focus-restored.txt"
            finally:
                for sig, previous in old_handlers.items():
                    signal.signal(sig, previous)

    logs = [app / f"profile/data/{name}/log/eden_log.txt" for name in ("lichii", "eden")]
    available_logs = [path for path in logs if path.is_file() and path.stat().st_mtime >= started_wall]
    if available_logs:
        shutil.copy2(max(available_logs, key=lambda path: path.stat().st_mtime), output / "emulator.log")
    result = {"tag": args.tag, "game": str(game), "exit_status": code,
              "duration_seconds": round(time.monotonic() - started, 3), "guest_memory_layout": "core default",
              "timeout_seconds": args.seconds, "screenshots": shots,
              "cli": str(executable) if args.cli else ("installed AppImage Qt GUI" if args.gui else "installed AppImage CLI"),
              "fresh_shader_cache": args.fresh_cache, "requested_focus": args.focus,
              "focus_started": focus is not None, "interrupted_signal": interrupted_signal, "error": error}
    (output / "run.json").write_text(json.dumps(result, indent=2) + "\n")
    if error:
        print(error, file=sys.stderr)
    # A configured timeout is expected, but crashes and other failures must
    # reach the caller as well as the evidence receipt.
    if error:
        return 1
    if code in (0, 124):
        return 0
    if code is None:
        return 1
    return 128 - code if code < 0 else code


if __name__ == "__main__":
    raise SystemExit(main())
