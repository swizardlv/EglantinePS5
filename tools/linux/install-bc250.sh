#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root/dist"
host=${BC250_HOST:-bazzite@192.168.0.167}
app_name=${BC250_APP_NAME:-EglantinePS5}
[[ $app_name =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo 'BC250_APP_NAME must be a directory name' >&2; exit 2; }
ssh_args=()
[[ -z ${BC250_SSH_CONTROL_PATH:-} ]] || ssh_args+=(-o "ControlPath=$BC250_SSH_CONTROL_PATH")
sha256sum --check EglantinePS5-Linux-SHA256SUMS
remote="Applications/$app_name"
ssh "${ssh_args[@]}" "$host" "mkdir -p $remote"
scp "${ssh_args[@]}" EglantinePS5-Linux-x86_64.AppImage EglantinePS5-Linux-x86_64.debug.tar.gz \
    EglantinePS5-Linux-SHA256SUMS EglantinePS5-Linux-build.json "$host:$remote/"
ssh "${ssh_args[@]}" "$host" bash -s -- "$app_name" <<'REMOTE'
set -euo pipefail
app_name=$1
cd "$HOME/Applications/$app_name"
sha256sum --check EglantinePS5-Linux-SHA256SUMS
chmod +x EglantinePS5-Linux-x86_64.AppImage
# Extract into a new managed directory so removed libraries cannot survive an upgrade.
stage=$(mktemp -d "$PWD/.extract.XXXXXX")
(cd "$stage" && ../EglantinePS5-Linux-x86_64.AppImage --appimage-extract > extract.log)
mkdir -p "$stage/squashfs-root/usr/bin/.debug"
tar -xzf EglantinePS5-Linux-x86_64.debug.tar.gz -C "$stage/squashfs-root/usr/bin/.debug"
if [[ -e squashfs-root ]]; then mv squashfs-root "$stage/previous-squashfs-root"; fi
mv "$stage/squashfs-root" squashfs-root
mkdir -p profile/{config,cache,data} evidence
mkdir -p profile/data/lichii/{sdmc,load,dump,tas,nand,keys,log,play_time}
cat > launch <<'LAUNCH'
#!/usr/bin/env bash
set -euo pipefail
app=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export XDG_CONFIG_HOME="$app/profile/config"
export XDG_CACHE_HOME=${EDEN_LINUX_CACHE_HOME:-$app/profile/cache}
export XDG_DATA_HOME="$app/profile/data"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
export DISPLAY=${DISPLAY:-:0}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-gamescope-0}
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}
cd "$app"
exec ./EglantinePS5-Linux-x86_64.AppImage "$@"
LAUNCH
chmod +x launch
mkdir -p ~/.local/share/applications
entry=eglantineps5-linux
label='EglantinePS5 Linux (test build)'
if [[ $app_name != EglantinePS5 ]]; then entry+="-$app_name"; label="EglantinePS5 Linux ($app_name)"; fi
cat > "$HOME/.local/share/applications/$entry.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$label
Exec="$HOME/Applications/$app_name/launch" %f
Icon=$HOME/Applications/$app_name/squashfs-root/io.github.zortos293.lichii.png
Categories=Game;Emulator;
Terminal=false
DESKTOP
XDG_CONFIG_HOME="$PWD/profile/config" XDG_CACHE_HOME="$PWD/profile/cache" XDG_DATA_HOME="$PWD/profile/data" \
    LD_LIBRARY_PATH="$PWD/squashfs-root/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    ./squashfs-root/usr/bin/eden-cli --version > evidence/cli-version.txt 2>&1
cp "$stage/extract.log" evidence/appimage-extract.log
python3 - "$stage" <<'CLEANUP'
import pathlib, shutil, sys
stage = pathlib.Path(sys.argv[1]).resolve()
assert stage.parent == pathlib.Path.cwd() and stage.name.startswith('.extract.')
shutil.rmtree(stage)
CLEANUP
date -u +%FT%TZ > installed-at.txt
REMOTE
