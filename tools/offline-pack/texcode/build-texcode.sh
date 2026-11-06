#!/usr/bin/env bash
# build-texcode.sh -- build the eden_texcode helper from Eden's own sources.
#
# The helper links Eden's CPU ASTC decoder (textures/astc.cpp), CPU BC
# encoder (textures/bcn.cpp, stb_dxt) and BC decoder
# (texture_cache/decode_bc.cpp, externals/bc_decoder) plus the small support
# files they need (workers.cpp, thread.cpp). No emulator, no GPU, no Vulkan:
# it is a plain Linux CLI, so it builds with the system g++.
#
# Dependency headers (boost, fmt) are fetched by setup-deps.sh into
# $HOME/.cache/eden-offline-pack/ (override with EDEN_OFFLINE_PACK_CACHE).
# Eden's logging sink is stubbed inside main.cpp (see the comment there).
#
# Usage: tools/offline-pack/texcode/build-texcode.sh
# Output: tools/offline-pack/texcode/build/eden_texcode
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
# ROOT is the EglantinePS5-offline-pack worktree root.
E="$ROOT/Eglantine"
CACHE="${EDEN_OFFLINE_PACK_CACHE:-$HOME/.cache/eden-offline-pack}"

if [[ ! -f "$CACHE/boost/usr/include/boost/container/static_vector.hpp" ]]; then
    echo "missing boost headers; run tools/offline-pack/texcode/setup-deps.sh first" >&2
    exit 1
fi
if [[ ! -f "$CACHE/fmt11/fmt-11.1.4/include/fmt/format.h" ]]; then
    echo "missing fmt headers; run tools/offline-pack/texcode/setup-deps.sh first" >&2
    exit 1
fi

OUT="$ROOT/tools/offline-pack/texcode/build"
mkdir -p "$OUT"

g++ -std=c++20 -O2 -DFMT_HEADER_ONLY \
    -I"$E/src" \
    -I"$E/externals/stb" \
    -I"$E/externals/bc_decoder" \
    -I"$CACHE/boost/usr/include" \
    -I"$CACHE/fmt11/fmt-11.1.4/include" \
    "$E/src/video_core/textures/astc.cpp" \
    "$E/src/video_core/textures/bcn.cpp" \
    "$E/src/video_core/textures/workers.cpp" \
    "$E/src/video_core/texture_cache/decode_bc.cpp" \
    "$E/src/common/thread.cpp" \
    "$E/src/common/error.cpp" \
    "$E/externals/stb/stb_dxt.cpp" \
    "$E/externals/bc_decoder/bc_decoder.cpp" \
    "$ROOT/tools/offline-pack/texcode/main.cpp" \
    -o "$OUT/eden_texcode" -pthread

echo "built: $OUT/eden_texcode"
"$OUT/eden_texcode" 2>&1 | head -2 || true
