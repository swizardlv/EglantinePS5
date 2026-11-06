#!/usr/bin/env python3
"""Apply the offline-capture hook to Eglantine's texture_cache.h.

Exact-string patch with assertion counts: every anchor must occur exactly
once, otherwise the script aborts without writing. Run from the
EglantinePS5-offline-pack worktree root; the target file is
Eglantine/src/video_core/texture_cache/texture_cache.h.

The hook is placed in TextureCache<P>::UploadImageContents (the single funnel
every guest->host texture upload passes through). Four branches, one call
each; all calls are guarded by IsPixelFormatASTC() and are no-ops (one bool
check) when EDEN_OFFLINE_CAPTURE is unset, so the normal path is unchanged.
"""

import sys
from pathlib import Path

TARGET = Path("Eglantine/src/video_core/texture_cache/texture_cache.h")

HOOK_ARGS = """image.gpu_addr, static_cast<u32>(image.info.format), image.info.size.width,
                image.info.size.height, image.info.size.depth,
                static_cast<u32>(image.info.resources.levels),
                static_cast<u32>(image.info.resources.layers),
                static_cast<u32>(image.info.type), image.guest_size_bytes,
                image.host_level_offset, image.info.astc_opaque,"""

PATCHES = [
    (
        "include offline_capture.h",
        '#include "video_core/textures/decoders.h"\n',
        '#include "video_core/textures/decoders.h"\n'
        '#include "video_core/texture_cache/offline_capture.h"\n',
    ),
    (
        "hook: accelerated upload, trimmed levels (swizzled-tail)",
        """        for (auto& upload : uploads) {
            upload.buffer_offset -= level_offset;
        }
        runtime.AccelerateImageUpload(image, staging, uploads, 0, 0);""",
        """        for (auto& upload : uploads) {
            upload.buffer_offset -= level_offset;
        }
        if (IsPixelFormatASTC(image.info.format)) {
            // OFFLINE_CAPTURE (EDEN_OFFLINE_CAPTURE=1): record the ASTC upload
            // for offline transcoding. Staging still holds the swizzled guest
            // bytes (levels >= host_level_offset only), so the offline tool
            // must unswizzle them first (not implemented in v1).
            OfflineCapture::RecordAstcUpload(
                """ + HOOK_ARGS + """
                "swizzled-tail",
                std::span<const std::uint8_t>{
                    mapped_span.data(),
                    static_cast<size_t>(image.info.resources.layers) * tail});
        }
        runtime.AccelerateImageUpload(image, staging, uploads, 0, 0);""",
    ),
    (
        "hook: accelerated upload (swizzled)",
        """        gpu_memory->ReadBlock(gpu_addr, mapped_span.data(), mapped_span.size_bytes(),
                              VideoCommon::CacheType::NoTextureCache);
        const auto uploads = FullUploadSwizzles(image.info);""",
        """        gpu_memory->ReadBlock(gpu_addr, mapped_span.data(), mapped_span.size_bytes(),
                              VideoCommon::CacheType::NoTextureCache);
        if (IsPixelFormatASTC(image.info.format)) {
            // OFFLINE_CAPTURE: staging holds the swizzled guest bytes; the
            // offline tool must unswizzle them first (not implemented in v1).
            OfflineCapture::RecordAstcUpload(
                """ + HOOK_ARGS + """
                "swizzled",
                std::span<const std::uint8_t>{mapped_span.data(), mapped_span.size()});
        }
        const auto uploads = FullUploadSwizzles(image.info);""",
    ),
    (
        "hook: converted upload (linear)",
        """        auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, unswizzle_data_buffer);
        ConvertImage(unswizzle_data_buffer, image.info, mapped_span, copies);""",
        """        auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, unswizzle_data_buffer);
        if (IsPixelFormatASTC(image.info.format)) {
            // OFFLINE_CAPTURE: unswizzle_data_buffer holds the linear ASTC
            // block stream, ready for the offline transcoder.
            OfflineCapture::RecordAstcUpload(
                """ + HOOK_ARGS + """
                "linear",
                std::span<const std::uint8_t>{unswizzle_data_buffer.data(),
                                              image.unswizzled_size_bytes});
        }
        ConvertImage(unswizzle_data_buffer, image.info, mapped_span, copies);""",
    ),
    (
        "hook: plain upload (linear)",
        """        const auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, mapped_span);
        image.UploadMemory(staging, copies);""",
        """        const auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, mapped_span);
        if (IsPixelFormatASTC(image.info.format)) {
            // OFFLINE_CAPTURE: mapped_span holds the linear ASTC block stream.
            OfflineCapture::RecordAstcUpload(
                """ + HOOK_ARGS + """
                "linear",
                std::span<const std::uint8_t>{mapped_span.data(), mapped_span.size()});
        }
        image.UploadMemory(staging, copies);""",
    ),
]


def main() -> int:
    text = TARGET.read_text()
    for name, old, new in PATCHES:
        count = text.count(old)
        if count != 1:
            print(f"ABORT: anchor for [{name}] occurs {count}x (expected 1)")
            return 1
        text = text.replace(old, new)
        print(f"patched: {name}")
    TARGET.write_text(text)
    print(f"wrote {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
