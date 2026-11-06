#!/usr/bin/env bash
# warm_caches.sh -- put pre-warmed shader/pipeline caches into an offline pack.
#
# What it does (and does NOT do):
#  * It copies the DISK shader caches Eden already maintains into
#    <pack>/shaders/. It does NOT warm anything itself: warming means running
#    the game once so Eden's own loader writes the files (see below).
#  * vulkan.bin            precompiled pipeline cache (guest shader SPIR-V +
#                          pipeline keys). Format: magic "yuzuvkch" (8 bytes)
#                          + u32 cache version + entries. Written by
#                          PipelineCache::LoadDiskResources / SerializePipeline
#                          (src/video_core/renderer_vulkan/vk_pipeline_cache.cpp,
#                          CACHE_VERSION = 19 at line 71; LoadPipelines in
#                          src/video_core/shader_environment.cpp:862).
#                          A version mismatch makes the loader DELETE the
#                          file, so the pack must ship version-19 files from
#                          a matching Eden build.
#  * vulkan_pipelines.bin  Vulkan driver pipeline cache (opaque driver blob,
#                          only usable on the same driver/GPU).
#
# Where the files live:
#  * Linux: ${XDG_CACHE_HOME:-~/.cache}/eden/shader/<title_id_16hex>/
#    (Common::FS::EdenPath::ShaderDir = <cache>/eden/shader; the per-title
#    subdir is fmt "{:016x}" of the title id, vk_pipeline_cache.cpp:650).
#    Exception: if a "user/" dir sits next to the Eden executable, portable
#    mode wins and the dir is <exe>/user/cache/shader/<title_id>/.
#  * PS5: the release package has no user/ dir, so Eden falls through to the
#    XDG paths under the PS5-side HOME, which is NOT known from source alone.
#    Locate it once from a real PS5 run (find the eden log's shader dir, or
#    search the PS5 home for vulkan.bin), then pass --shader-dir explicitly.
#
# Warming procedure (the part that actually creates the files):
#  1. Run the game on the PS5 until the area you want pre-warmed has been
#     visited (title screen at minimum; ideally a playthrough of the first
#     area). Every pipeline built lands in the in-memory cache.
#  2. Quit the game cleanly. The caches are flushed on exit:
#     ~PipelineCache serializes vulkan_pipelines.bin, and the serialization
#     thread writes vulkan.bin (flush every 128 pipelines / 30 s, and on
#     shutdown). A crash or hard power-off may lose the tail.
#  3. Pull the two files off the PS5 (FTP) and run this script with
#     --shader-dir <dir-with-the-files> --pack <pack-dir>.
#
# Usage:
#   warm_caches.sh --title-id 0100xxxxxxxxxxxx --pack <pack-dir>
#   warm_caches.sh --shader-dir <dir> --pack <pack-dir> [--title-id ...]
#   warm_caches.sh --check --title-id ...   # report only, copy nothing
set -euo pipefail

TITLE_ID=""
SHADER_DIR=""
PACK=""
CHECK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --title-id) TITLE_ID="$2"; shift 2 ;;
        --shader-dir) SHADER_DIR="$2"; shift 2 ;;
        --pack) PACK="$2"; shift 2 ;;
        --check) CHECK=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$SHADER_DIR" ]]; then
    [[ -n "$TITLE_ID" ]] || { echo "--title-id or --shader-dir is required" >&2; exit 2; }
    # Lowercase the title id; Eden formats it as {:016x}.
    TITLE_ID="$(printf '%s' "$TITLE_ID" | tr 'A-F' 'a-f')"
    CANDIDATES=(
        "${XDG_CACHE_HOME:-$HOME/.cache}/eden/shader/$TITLE_ID"
        "$HOME/.cache/eden/shader/$TITLE_ID"
    )
    for c in "${CANDIDATES[@]}"; do
        if [[ -d "$c" ]]; then SHADER_DIR="$c"; break; fi
    done
    [[ -n "$SHADER_DIR" ]] || {
        echo "no shader dir found for title $TITLE_ID; tried:" >&2
        printf '  %s\n' "${CANDIDATES[@]}" >&2
        echo "If the files came from the PS5, pass --shader-dir <dir>." >&2
        exit 1
    }
fi

echo "shader dir: $SHADER_DIR"
ok=1
for f in vulkan.bin vulkan_pipelines.bin; do
    p="$SHADER_DIR/$f"
    if [[ ! -f "$p" ]]; then
        echo "  MISSING: $f"
        ok=0
        continue
    fi
    sz="$(stat -c %s "$p")"
    extra=""
    if [[ "$f" == "vulkan.bin" ]]; then
        magic="$(head -c 8 "$p")"
        ver="$(python3 -c "import struct; print(struct.unpack('<I', open('$p','rb').read()[8:12])[0])")"
        extra="magic='$magic' version=$ver"
        if [[ "$magic" != "yuzuvkch" ]]; then
            echo "  BAD MAGIC: $f ($extra)" >&2; ok=0
        elif [[ "$ver" != "19" ]]; then
            echo "  WARNING: $f version $ver != 19 (this build's CACHE_VERSION); the loader will DELETE it" >&2
        fi
    fi
    echo "  $f: $sz bytes $extra"
done
[[ "$ok" == "1" ]] || { echo "not all cache files are present/valid" >&2; exit 1; }

if [[ "$CHECK" == "1" ]]; then
    echo "check OK (nothing copied)"
    exit 0
fi

[[ -n "$PACK" ]] || { echo "--pack is required to copy" >&2; exit 2; }
mkdir -p "$PACK/shaders"
cp "$SHADER_DIR/vulkan.bin" "$PACK/shaders/vulkan.bin"
cp "$SHADER_DIR/vulkan_pipelines.bin" "$PACK/shaders/vulkan_pipelines.bin"
echo "copied to $PACK/shaders/"
ls -la "$PACK/shaders/"
echo "NOTE: the loader change that makes Eden READ these files from the pack"
echo "(instead of its own shader dir) is not implemented yet -- see"
echo "docs/offline-pack-design.md section 4. Until then these files are"
echo "payload for the future loader, and usable today by copying them into"
echo "the PS5 Eden shader dir by hand."
