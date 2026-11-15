# Mario Kart World on the BC250 Linux test build

Mario Kart World failed before rendering with the 8 GiB guest layout used in the
first BC250 test. Guest debugging showed its `0x1c2600000`-byte (7.04 GiB) heap
request returning `0x1003` (out of memory). The 8 GiB layout provides a 6,547 MiB
application pool. The game then followed the null heap pointer and jumped to zero.
The intended 12 GiB Switch 2 layout clears that failure.

Dense graphics allocation subsequently exhausted memory while loading assets,
including at half resolution. The qualified configuration uses the existing
sparse-texture override, half resolution, and 1024/1536 MiB texture-cache thresholds.
No emulator executable was changed in this experiment.

## Prerequisites and launch

These helpers require an **already installed Linux SDL/Vulkan test build** at
`~/Applications/EglantinePS5`, with its extracted `squashfs-root/usr/bin/eden-cli`,
shared libraries and initialized `profile/config/eden/sdl2-config.ini`. They do not
build or install the emulator, keys, firmware or games. Linux packaging work is
separate from this PR. Override the installation directory with
`EGLANTINE_LINUX_APP_DIR` if needed.

On the BC250:

```sh
DISPLAY=:1 bash tools/linux/launch-mkw-bc250.sh '/path/to/Mario Kart World.nup'
```

The optional game path defaults to
`~/Emulation/roms/switch/Mario Kart World.nup`. Additional CLI arguments follow it.
On first launch, the helper copies the installed SDL configuration into
`mkw-profile/config/eden/sdl2-config.ini` and sets `resolution_setup=1` and
`resolution_setup\default=false` in `[Renderer]` (0.5×). It preserves an existing
Mario Kart profile. Data and caches are shared with the installed test build;
the general configuration is untouched.

The launcher sets:

```text
EDEN_NX2_MEMORY_LAYOUT_GB=12
EDEN_NX2_SPARSE_UNBACKED=1
EDEN_NX2_TC_EXPECTED_MIB=1024
EDEN_NX2_TC_CRITICAL_MIB=1536
```

Sparse handling explicitly overrides RADV's default policy. Qualification here is
limited to this title/idle-attract sequence and driver. Logs still include nonfatal
unimplemented service/texture diagnostics, unmapped sparse device reads and skipped
mesh-shader draws; this is not a claim of complete graphical accuracy.

## Repeat a bounded capture

When the BC250 is shared, the verifier acquires
`~/.cache/eglantineps5-bc250/board.lock` before creating its output directory or
starting the game timeout. Other capture and direct-launch entrypoints must use
the same relay helper, for example:

```sh
bash tools/linux/bc250-relay.sh \
  bash ~/eglantine-debug/capture-bc250.sh appimage '/path/to/another-game.nup'
```

Place the relay outside the timeout so a queued capture gets its full test
window. The active game finishes normally; the next test then takes the board.
Nested launchers inherit `BC250_RELAY_HELD=1` to avoid a second acquisition. A
standalone AppImage invocation can bypass this advisory lock, so exclude any
period with another emulator from performance comparisons. Do not stop another
agent's game to reclaim a turn.

The verifier uses `focus-mkw-bc250.py`, whose third argument is the capture output
directory. The general Linux `focus-game.py` helper instead accepts an evidence
tag; its existing callers retain that interface.

The helper targets the Bazzite gaming-mode session: X11 `:0` for Gamescope control,
`:1` for the game, and Wayland `gamescope-0`. It temporarily selects the emulator
window and restores the previous Gamescope selection afterward. Run it as the
logged-in gaming-session user. Dependencies are Python 3, GNU `time`/`timeout`,
`xdotool`, `xprop`, and `gamescopectl`.

```sh
python3 tools/linux/verify-mkw-bc250.py \
  --out ~/eglantine-debug/evidence/mkw-new-run \
  --seconds 600 '/path/to/Mario Kart World.nup'
```

Use a new output directory for each run. The command records process RSS, CPU,
DRM VRAM/GTT allocation, graphics-engine activity, system memory/swap, screenshots,
stdout/stderr and the exit status. Only descendant emulator processes are measured.
Its success exit code means the bounded capture reached its time limit, **not**
that the game reached the title or remained healthy. Inspect the images and logs.

Completion requires visible, changing title frames, a responsive start screen,
no guest panic or device-loss failure, and no new GPU reset/OOM events in the
kernel journal. `run.json` records the start epoch for checking the journal:

```sh
journalctl -k --since '@START_EPOCH_FROM_RUN_JSON' --no-pager
```

The capture does not automatically press buttons. To check returning from the
idle attract sequence, focus the game and press its mapped A button (the tested
keyboard profile maps this to `a`); confirm the title and L+R prompt return.

## Recorded qualification — 2026-11-14

The installed build was based on port commit `382549d077554ba3341e1b04e7b7620b1d45f6d7`
and source snapshot `eglantine-3140648a668aaebf87bae2dea17512fdca125ba0-d0c38de995e4`,
with the separate Linux NUP/frontend/wait64/audio/compute patches. The environment
was Bazzite on AMD BC-250 / RADV GFX1013, Mesa 26.2.1, Vulkan 1.4.354.
This is the Linux frontend/shared core, not the PS5 frontend or driver.

The initial corrected 180-second trial and subsequent 600-second run reached the
animated title screen. The longer run collected 592 samples over 606.4 seconds
including shutdown. The title and L+R prompt were visible at 120 seconds and
590 seconds; a button press returned from the idle attract sequence around eight
minutes. Exit 124 was the deliberate timeout, with no emulator process left.
The kernel journal had no entries in the verification window, and the emulator
log had no guest null-execution fault, panic or Vulkan device-loss exception.

The game displayed **Ver. 1.8.0**, despite the NUP base metadata's version field
being zero. Resource means below cover 120–590 seconds, including idle attract:

| Measurement | Mean | Full-run sampled peak |
| --- | ---: | ---: |
| Emulator RSS | 6.04 GiB | 9.09 GiB |
| Emulator CPU (100% = one logical CPU) | 232.7% | 546.2% |
| Process GPU graphics-engine activity | 16.3% | 58.4% |
| Emulator VRAM allocation | 3.16 GiB | 3.30 GiB |
| Emulator GTT allocation | 2.49 GiB | See raw telemetry |

GPU activity uses deltas of `drm-engine-gfx` nanoseconds, deduplicating DRM client
IDs. It is not all-device utilization or a separate compute-engine measurement.
RSS can include mapped graphics pages, so adding RSS, VRAM and GTT would double
count some memory. Swap use was substantial; this was not a no-swap qualification.
The driver's `gpu_busy_percent` sysfs interface was unsupported.

**FPS was not directly captured.** Cache-frame counters suggest roughly 12 FPS
in this run, but also advance for idle cache work. Treat that only as an estimate,
not a measured average FPS benchmark. Race stability and long-term graphical
correctness remain untested.

The full report, raw telemetry, screenshots and logs were delivered separately
as `Mario-Kart-World-BC250-fixed-evidence.zip`; its concise receipt is alongside
this document in `evidence/bc250-mkw-title-2026-11-14.json`.
