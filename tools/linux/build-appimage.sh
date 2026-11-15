#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
[[ $(uname -sm) == 'Linux x86_64' ]] || { echo 'Requires Linux x86_64' >&2; exit 2; }
# Keep the tracked emulator source separate from generated files and package caches.
work="$root/build/linux"
mkdir -p "$work" "$root/dist"
bash tools/sync-eden-source.sh "$work/source"
python3 "$root/tools/linux/checks/wait64_abi_check.py" "$work/source"
cmake -S "$work/source" -B "$work/cmake" -G Ninja \
    -DCMAKE_PROJECT_yuzu_INCLUDE="$root/tools/linux/checks/inject.cmake" \
    -DEGLANTINE_NUP_CHECK_SOURCE="$root/tools/linux/checks/nup_check.cpp" \
    -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_LTO=OFF \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O2 -g -DNDEBUG -fno-omit-frame-pointer' \
    -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_CMD=ON \
    -DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON \
    -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF -DYUZU_USE_BUNDLED_SDL3=ON \
    -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
    -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF -DENABLE_WERROR=OFF \
    "$@"
cmake --build "$work/cmake" --target yuzu yuzu-cmd eglantine-linux-nup-check --parallel "${EDEN_BUILD_JOBS:-2}"
"$work/cmake/bin/eglantine-linux-nup-check" "$root/fixtures/nup" | tee "$work/nup-check.log"
# Keep full symbols outside the compressed AppImage for GDB/perf.
mkdir -p "$work/symbols" "$work/package" "$work/tools"
for exe in lichii eden-cli; do
    objcopy --only-keep-debug "$work/cmake/bin/$exe" "$work/symbols/$exe.debug"
    cp "$work/cmake/bin/$exe" "$work/package/$exe"
    strip --strip-debug "$work/package/$exe"
    objcopy --add-gnu-debuglink="$work/symbols/$exe.debug" "$work/package/$exe"
done
fetch() {
    local name=$1 sha=$2
    if ! echo "$sha  $work/tools/$name-x86_64.AppImage" | sha256sum --check --status; then
        curl -fL --retry 3 "https://github.com/linuxdeploy/$name/releases/download/continuous/$name-x86_64.AppImage" -o "$work/tools/$name-x86_64.AppImage"
    fi
    echo "$sha  $work/tools/$name-x86_64.AppImage" | sha256sum --check
    chmod +x "$work/tools/$name-x86_64.AppImage"
}
fetch linuxdeploy df908b19f1ed1cdafa79508d415aa12d6c56ce4d56d4149aa8c09286718706e5
fetch linuxdeploy-plugin-qt cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617
cp "$work/source/dist/lichii.png" "$work/package/io.github.zortos293.lichii.png"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=${QMAKE:-/usr/bin/qmake6}
export EXTRA_PLATFORM_PLUGINS='libqwayland-egl.so;libqwayland-generic.so'
export OUTPUT="$root/dist/EglantinePS5-Linux-x86_64.AppImage"
export LDAI_OUTPUT="$OUTPUT"
# linuxdeploy can retain an executable already present in its AppDir. Always
# recreate this managed staging tree so incremental builds package the new ELF.
python3 - "$work/AppDir" <<'PY'
from pathlib import Path
import shutil, sys
appdir = Path(sys.argv[1])
assert appdir.name == 'AppDir' and appdir.parent.name == 'linux' and appdir.parent.parent.name == 'build'
if appdir.is_symlink():
    appdir.unlink()
elif appdir.exists():
    shutil.rmtree(appdir)
PY
mkdir -p "$work/AppDir/usr/bin"
cp "$work/cmake/bin/nx2_switches.txt" "$work/AppDir/usr/bin/"
"$work/tools/linuxdeploy-x86_64.AppImage" --appdir "$work/AppDir" \
    --executable "$work/package/lichii" --executable "$work/package/eden-cli" \
    --desktop-file "$work/source/dist/io.github.zortos293.lichii.desktop" \
    --icon-file "$work/package/io.github.zortos293.lichii.png" --plugin qt --output appimage
tar -C "$work/symbols" -czf "$root/dist/EglantinePS5-Linux-x86_64.debug.tar.gz" .
python3 - "$root" <<'PY'
import json,pathlib,re,subprocess,sys
r=pathlib.Path(sys.argv[1]); src=r/'build/linux/source'
def build_id(path):
    notes=subprocess.check_output(['readelf','-n',str(path)],text=True,env={'LC_ALL':'C','PATH':'/usr/bin:/bin'})
    match=re.search(r'Build ID:\s+([0-9a-f]+)',notes)
    if not match:
        raise RuntimeError(f'No ELF build ID in {path}')
    return match[1]
identities={}
for exe in ('lichii','eden-cli'):
    compiled=build_id(r/'build/linux/cmake/bin'/exe)
    deployed=build_id(r/'build/linux/AppDir/usr/bin'/exe)
    if compiled != deployed:
        raise RuntimeError(f'Packaged {exe} does not match the compiled ELF')
    identities[exe]=compiled
receipt={'port_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
         'source_stamp':(src/'.unpacked-from').read_text().strip(),
         'elf_build_ids':identities,
         'build_type':'RelWithDebInfo', 'frontend':'Linux Qt/SDL, not the PS5 frontend'}
(r/'dist/EglantinePS5-Linux-build.json').write_text(json.dumps(receipt,indent=2)+'\n')
PY
(cd dist && sha256sum EglantinePS5-Linux-x86_64.AppImage EglantinePS5-Linux-x86_64.debug.tar.gz > EglantinePS5-Linux-SHA256SUMS)
