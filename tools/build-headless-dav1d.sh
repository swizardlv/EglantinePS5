#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build dav1d (CPU AV1 decoder) for the PS5, as a static library.
# FFmpeg's native AV1 decoder needs a hardware accelerator, which the PS5
# lacks for AV1, so AV1 video decodes via libdav1d instead.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(cat "$root/.local/headless-cache")
[[ "$(cat "$scratch/owner")" == "$root" ]]
export PS5_PAYLOAD_SDK="$scratch/sdk"
source="$root/.deps/dav1d-1.5.1"
prefix="$scratch/dav1d"
build_dir="$scratch/dav1d-build"
test -f "$source/meson.build"

# Meson cross file for the PS5 (generated: the SDK path lives in the scratch dir).
cat > "$scratch/ps5-meson-cross.ini" <<EOF
[binaries]
c = ['bash', '$root/tools/ffmpeg-ps5-cc.sh']
ar = 'llvm-ar-18'
ranlib = 'llvm-ranlib-18'
strip = 'llvm-strip-18'
nasm = 'nasm'
[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

# Static library only; no tools, examples, tests or docs.
meson setup "$build_dir" "$source" \
    --cross-file "$scratch/ps5-meson-cross.ini" \
    --prefix="$prefix" \
    --default-library=static \
    --buildtype=release \
    -Denable_asm=true \
    -Denable_tools=false \
    -Denable_examples=false \
    -Denable_tests=false \
    -Denable_docs=false
ninja -C "$build_dir"
ninja -C "$build_dir" install
test -f "$prefix/lib/libdav1d.a"
test -f "$prefix/include/dav1d/dav1d.h"
echo "dav1d built: $prefix/lib/libdav1d.a"
