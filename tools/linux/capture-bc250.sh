#!/usr/bin/env bash
# Run ON THE BC250 after installation. Saves command output, system state, and logs.
# usage: capture-bc250.sh run|appimage|mangohud|gdb|perf|strace /absolute/path/game.nsp [CLI arguments...]
set -euo pipefail
mode=${1:?usage: capture-bc250.sh run|appimage|mangohud|gdb|perf|strace GAME [ARGS...]}; shift
game=${1:?game path required}; shift
[[ -f $game ]] || { echo "Game does not exist: $game" >&2; exit 2; }
app=${EDEN_LINUX_APP_DIR:-$HOME/Applications/EglantinePS5}
debug_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
output="$debug_dir/evidence/$(date -u +%Y%m%dT%H%M%SZ)-$mode"
mkdir -p "$output"
exec > >(tee "$output/session.txt") 2>&1
printf 'Game: %s\nMode: %s\n' "$game" "$mode"
uname -a
sha256sum "$app/EglantinePS5-Linux-x86_64.AppImage"
cp "$app/EglantinePS5-Linux-build.json" "$output/"
vulkaninfo --summary > "$output/vulkan.txt" 2>&1 || true
export XDG_CONFIG_HOME="$app/profile/config"
export XDG_CACHE_HOME=${EDEN_LINUX_CACHE_HOME:-$app/profile/cache}
export XDG_DATA_HOME="$app/profile/data"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
export DISPLAY=${DISPLAY:-:0}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-gamescope-0}
# The extracted executable gives GDB/perf a stable ELF with a matching .debug file.
export LD_LIBRARY_PATH="$app/squashfs-root/usr/lib:${LD_LIBRARY_PATH:-}"
cli=${EDEN_LINUX_CLI:-$app/squashfs-root/usr/bin/eden-cli}
[[ -x $cli ]] || { echo "Missing CLI: $cli" >&2; exit 2; }
printf 'CLI: %s\n' "$cli"
sha256sum "$cli"
if [[ -f $(dirname -- "$cli")/candidate.json ]]; then
    cp "$(dirname -- "$cli")/candidate.json" "$output/candidate.json"
fi
cd "$app"
runner=()
if [[ ${CAPTURE_SECONDS:-0} != 0 ]]; then
    [[ $CAPTURE_SECONDS =~ ^[1-9][0-9]*$ ]] || { echo 'CAPTURE_SECONDS must be a positive integer'; exit 2; }
    runner=(timeout --foreground --signal=INT --kill-after=10s "${CAPTURE_SECONDS}s")
fi
set +e
case $mode in
run) /usr/bin/time -v "${runner[@]}" "$cli" "$@" "$game" ;;
appimage) /usr/bin/time -v "${runner[@]}" env -u LD_LIBRARY_PATH "$app/launch" -f "$@" "$game" ;;
mangohud) MANGOHUD_CONFIG="autostart_log=1,log_duration=60,output_folder=$output" mangohud "$cli" "$@" "$game" ;;
gdb) gdb -batch -ex 'set pagination off' -ex run -ex 'thread apply all bt full' \
    -ex 'info registers' -ex 'info sharedlibrary' --args "$cli" "$@" "$game" ;;
perf) distrobox enter --name eglantine-debug -- perf record -e cpu-clock -F 99 \
    --call-graph dwarf -o "$output/perf.data" -- "$cli" "$@" "$game" ;;
strace) distrobox enter --name eglantine-debug -- strace -ff -tt -T \
    -o "$output/strace" "$cli" "$@" "$game" ;;
*) echo "Unknown mode: $mode"; exit 2 ;;
esac
status=$?
printf '%s\n' "$status" > "$output/exit-status.txt"
[[ ! -d $XDG_DATA_HOME/lichii/log ]] || cp -a "$XDG_DATA_HOME/lichii/log" "$output/"
echo "Evidence: $output"
exit "$status"
