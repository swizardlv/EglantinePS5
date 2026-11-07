#!/usr/bin/env bash
# Build the nxnative PoC skeleton title (PPSA98722).
#
# Runs INSIDE the eglantineps5-builder container (clang-18 lives there):
#   docker exec gallant_banach bash -c \
#     'cd /data/code/EglantinePS5-nxnative-poc && bash nxnative-poc/skeleton/build-skeleton.sh'
#
# What it reuses (read-only, never modified):
#   - /data/code/mihawk-vulkan-review: PS5 payload SDK, RADV release archive
#     (libvulkan_radeon.ps5.a), radv-link.sh recipe, CRT sources, stubs,
#     sce_sys/icon0.png.
#   - /data/code/mihawk-mesa-review: Vulkan headers.
#   - This worktree's tools/build-native-tool.sh: builds build/host/ps5-native-tool.
#   - /data/code/EglantinePS5/build/release/PPSA98515/sce_module/libc.prx:
#     the byte-identical libc reproducer, copied and verified against
#     /data/code/mihawk-vulkan-review/runtime/libc.prx.sha256 (no rebuild
#     into the shared checkout).
#
# Output (staging, NOT a deploy): nxnative-poc/skeleton/build/PPSA98722/
#   eboot.bin, sce_sys/param.json, sce_sys/icon0.png, sce_module/libc.prx
# Deploy is a separate step (nxnative-poc/skeleton/deploy-steps.md):
# copy the staging dir to the USB disk as PPSA98722/. Never network-deploy.
#
# TITLE_ID: PPSA98722 (user-confirmed 2026-11-07 08:10). Never PPSA98515.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
skel="$root/nxnative-poc/skeleton"
vulkan=/data/code/mihawk-vulkan-review
mesa=/data/code/mihawk-mesa-review
sdk_root="$vulkan/.deps/native/ps5-payload-sdk"
archive="$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
native="$vulkan/tooling/native"
work="$skel/build/work"
app="$skel/build/PPSA98722"
param="$skel/param.json"

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
[[ $title_id == "PPSA98722" ]] || { echo "refusing: param.json titleId is $title_id, not PPSA98722" >&2; exit 2; }

for file in "$archive" "$sdk_root/bin/prospero-lld" "$param" \
            "$native/app_crt.cpp" "$native/app_cpp_runtime.cpp" "$native/app-symbols.map" \
            "$vulkan/tooling/psbc/ps5-pie-unwind.ld" "$vulkan/tools/radv-link.sh" \
            "$mesa/include/vulkan/vulkan.h" "$skel/skeleton.c"; do
    [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done
command -v clang-18 >/dev/null || { echo "clang-18 not found: run inside eglantineps5-builder" >&2; exit 2; }

# ps5-native-tool (Eglantine's own builder; built in this worktree).
tool="$root/build/host/ps5-native-tool"
if [[ ! -x $tool ]]; then
    bash "$root/tools/build-native-tool.sh"
fi
[[ -x $tool ]] || { echo "ps5-native-tool did not build" >&2; exit 2; }

# libc.prx: copy the already-built byte-identical reproducer, verify checksum.
libc_src=/data/code/EglantinePS5/build/release/PPSA98515/sce_module/libc.prx
manifest="$vulkan/runtime/libc.prx.sha256"
[[ -f $libc_src ]] || { echo "missing $libc_src (build the mainline release first?)" >&2; exit 2; }
[[ -f $manifest ]] || { echo "missing $manifest" >&2; exit 2; }
mkdir -p "$work"
cp "$libc_src" "$work/libc.prx"
( cd "$work" && sha256sum --check --strict --quiet "$manifest" ) || {
    echo "libc.prx checksum does not match $manifest" >&2; exit 2; }

module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

mkdir -p "$work/obj" "$work/stubs" "$work/gen"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$vulkan/tooling/prospero-clang18" "$@"; }

cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
    -I "$mesa/include" -c "$skel/skeleton.c" -o "$work/obj/skeleton.o"
cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk_root" "$archive" || exit 2
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/skeleton.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic "$fself_magic"
cp "$param" "$app/sce_sys/param.json"
[[ -f $vulkan/sce_sys/icon0.png ]] && cp "$vulkan/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
cp "$work/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/sce_module/libc.prx" > /dev/null
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
echo "== Built $app"
find "$app" -type f -exec ls -l {} \;
sha256sum "$app/eboot.bin" "$app/sce_sys/param.json" "$app/sce_module/libc.prx"
echo "BUILD_EXIT:0"
