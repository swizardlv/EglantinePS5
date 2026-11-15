#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Requires an installed EglantinePS5 Linux test build; see docs/BC250-MARIO-KART-WORLD.md.
set -euo pipefail
ulimit -c 0
app="${EGLANTINE_LINUX_APP_DIR:-$HOME/Applications/EglantinePS5}"
game="${1:-$HOME/Emulation/roms/switch/Mario Kart World.nup}"
[[ $# == 0 ]] || shift
[[ -x "$app/squashfs-root/usr/bin/eden-cli" ]] || { echo "Missing installed Linux CLI: $app" >&2; exit 2; }
[[ -f "$game" ]] || { echo "Missing game: $game" >&2; exit 2; }
# Resolve before cd below; keep file names containing spaces intact.
game=$(realpath -- "$game")
profile="$app/mkw-profile/config/eden/sdl2-config.ini"
if [[ ! -e "$profile" ]]; then
    python3 - "$app/profile/config/eden/sdl2-config.ini" "$profile" <<'PROFILE'
import pathlib, re, sys
source, target = map(pathlib.Path, sys.argv[1:])
text = source.read_text()
section = re.search(r"(?ms)^\[Renderer\]\s*\n(.*?)(?=^\[|\Z)", text)
if section is None:
    raise SystemExit("The installed SDL profile has no [Renderer] section; initialize it first")
body = section[1]
for key, value in [("resolution_setup", "1"), (r"resolution_setup\default", "false")]:
    pattern = r"(?m)^" + re.escape(key) + r"=.*$"
    line = key + "=" + value
    if re.search(pattern, body):
        body = re.sub(pattern, lambda _: line, body)
    else:
        body += "\n" + line + "\n"
text = text[:section.start(1)] + body + text[section.end(1):]
target.parent.mkdir(parents=True, exist_ok=True)
# Never overwrite a profile initialized or edited by another invocation.
try:
    with target.open("x") as output:
        output.write(text)
except FileExistsError:
    pass
PROFILE
fi
export XDG_CONFIG_HOME="$app/mkw-profile/config"
export XDG_CACHE_HOME="$app/profile/cache"
export XDG_DATA_HOME="$app/profile/data"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"
export DISPLAY="${DISPLAY:-:1}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-gamescope-0}"
export SDL_VIDEO_DRIVER=x11
export LD_LIBRARY_PATH="$app/squashfs-root/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export EDEN_NX2_MEMORY_LAYOUT_GB=12
export EDEN_NX2_SPARSE_UNBACKED=1
export EDEN_NX2_TC_EXPECTED_MIB=1024
export EDEN_NX2_TC_CRITICAL_MIB=1536
cd "$app"
exec "$app/squashfs-root/usr/bin/eden-cli" "$@" "$game"
