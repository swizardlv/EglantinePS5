// eden_texcode -- standalone texture transcoder built from Eden's own code.
//
// It links Eden's CPU ASTC decoder (src/video_core/textures/astc.cpp,
// FasTC-based), Eden's CPU BC encoder (src/video_core/textures/bcn.cpp,
// stb_dxt) and Eden's BC decoder (src/video_core/texture_cache/decode_bc.cpp,
// externals/bc_decoder) -- the same code the emulator itself uses -- so the
// offline pack tool can transcode captured ASTC textures to BC1/BC3 without
// a GPU, and verify the round trip.
//
// Commands (all operate on ONE mip level; the Python tool splits the chain):
//   astc-decode <in.astc> <w> <h> <bw> <bh> <out.rgba>
//   bc3-encode  <in.rgba>  <w> <h> <out.bc3>
//   bc1-encode  <in.rgba>  <w> <h> <out.bc1>
//   bc3-decode  <in.bc3>   <w> <h> <out.rgba>
//   bc1-decode  <in.bc1>   <w> <h> <out.rgba>
//   roundtrip   <in.astc>  <w> <h> <bw> <bh> [bc1|bc3]
//       Decode ASTC -> RGBA, encode to BCn, decode back, print sizes,
//       timings and PSNR / max error vs the ASTC-decoded pixels.
//       Exit 0 only if the round trip is sane (PSNR >= 20 dB, no crash).
//
// Input files are raw block streams with no container header: ASTC is
// 16-byte blocks, BC3 is 16-byte blocks, BC1 is 8-byte blocks, RGBA is
// tightly packed 4 bytes/texel, row-major, top to bottom.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"
#include "video_core/texture_cache/decode_bc.h"
#include "video_core/texture_cache/types.h"

// Standalone stub for Eden's log sink: this tool is a CLI, not the emulator,
// so there is no log filter/file backend. decode_bc.cpp (LOG_WARNING on an
// unimplemented format) and thread.cpp only need the symbol to exist; the
// call is never taken for BC1/BC3.
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*,
                       fmt::string_view format, const fmt::format_args& args) {
    std::fprintf(stderr, "[eden_texcode] %s\n",
                 fmt::vformat(format, args).c_str());
}
} // namespace Common::Log

namespace {

using VideoCommon::BufferImageCopy;
using VideoCore::Surface::PixelFormat;

bool ReadFile(const std::string& path, std::vector<u8>& out) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    in.seekg(0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

bool WriteFile(const std::string& path, const std::span<const u8> data) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

BufferImageCopy CopyFor(u32 width, u32 height, PixelFormat format) {
    BufferImageCopy copy{};
    copy.buffer_offset = 0;
    copy.buffer_size = 0;
    copy.buffer_row_length = width; // tightly packed
    copy.buffer_image_height = height;
    copy.image_subresource = VideoCommon::SubresourceLayers{};
    copy.image_subresource.num_layers = 1;
    copy.image_offset = VideoCommon::Offset3D{};
    copy.image_extent = VideoCommon::Extent3D{width, height, 1};
    (void)format;
    return copy;
}

double Psnr(const std::vector<u8>& a, const std::vector<u8>& b, int channels,
            int channel_offset, int channel_count, double& max_err) {
    double mse = 0.0;
    max_err = 0.0;
    const size_t n = a.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        for (int c = channel_offset; c < channel_offset + channel_count; ++c) {
            const double d = static_cast<double>(a[i * 4 + c]) - static_cast<double>(b[i * 4 + c]);
            mse += d * d;
            max_err = std::max(max_err, std::abs(d));
        }
    }
    mse /= static_cast<double>(n * channel_count);
    (void)channels;
    if (mse == 0.0) {
        return 99.0;
    }
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

int CmdAstcDecode(int argc, char** argv) {
    // astc-decode <in.astc> <w> <h> <bw> <bh> <out.rgba>
    if (argc != 8) {
        std::fprintf(stderr, "usage: astc-decode <in.astc> <w> <h> <bw> <bh> <out.rgba>\n");
        return 2;
    }
    const u32 w = std::stoul(argv[3]);
    const u32 h = std::stoul(argv[4]);
    const u32 bw = std::stoul(argv[5]);
    const u32 bh = std::stoul(argv[6]);
    std::vector<u8> in;
    if (!ReadFile(argv[2], in)) {
        return 1;
    }
    const size_t blocks_x = (w + bw - 1) / bw;
    const size_t blocks_y = (h + bh - 1) / bh;
    if (in.size() != blocks_x * blocks_y * 16) {
        std::fprintf(stderr, "astc-decode: expected %zu bytes, got %zu\n",
                     blocks_x * blocks_y * 16, in.size());
        return 1;
    }
    std::vector<u8> out(static_cast<size_t>(w) * h * 4);
    Tegra::Texture::ASTC::Decompress(in, w, h, 1, bw, bh, out);
    return WriteFile(argv[7], out) ? 0 : 1;
}

int CmdBcEncode(int argc, char** argv, bool bc1) {
    // bcN-encode <in.rgba> <w> <h> <out.bcN>
    if (argc != 6) {
        std::fprintf(stderr, "usage: bc%s-encode <in.rgba> <w> <h> <out.bc%s>\n",
                     bc1 ? "1" : "3", bc1 ? "1" : "3");
        return 2;
    }
    const u32 w = std::stoul(argv[3]);
    const u32 h = std::stoul(argv[4]);
    std::vector<u8> in;
    if (!ReadFile(argv[2], in)) {
        return 1;
    }
    if (in.size() != static_cast<size_t>(w) * h * 4) {
        std::fprintf(stderr, "bc-encode: expected %zu RGBA bytes, got %zu\n",
                     static_cast<size_t>(w) * h * 4, in.size());
        return 1;
    }
    const size_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
    std::vector<u8> out(blocks * (bc1 ? 8 : 16));
    if (bc1) {
        Tegra::Texture::BCN::CompressBC1(in, w, h, 1, out);
    } else {
        Tegra::Texture::BCN::CompressBC3(in, w, h, 1, out);
    }
    return WriteFile(argv[5], out) ? 0 : 1;
}

int CmdBcDecode(int argc, char** argv, bool bc1) {
    // bcN-decode <in.bcN> <w> <h> <out.rgba>
    if (argc != 6) {
        std::fprintf(stderr, "usage: bc%s-decode <in.bc%s> <w> <h> <out.rgba>\n",
                     bc1 ? "1" : "3", bc1 ? "1" : "3");
        return 2;
    }
    const u32 w = std::stoul(argv[3]);
    const u32 h = std::stoul(argv[4]);
    std::vector<u8> in;
    if (!ReadFile(argv[2], in)) {
        return 1;
    }
    const size_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
    if (in.size() != blocks * (bc1 ? 8 : 16)) {
        std::fprintf(stderr, "bc-decode: expected %zu bytes, got %zu\n",
                     blocks * (bc1 ? 8 : 16), in.size());
        return 1;
    }
    std::vector<u8> out(static_cast<size_t>(w) * h * 4);
    BufferImageCopy copy = CopyFor(w, h, bc1 ? PixelFormat::BC1_RGBA_UNORM
                                             : PixelFormat::BC3_UNORM);
    VideoCommon::DecompressBCn(in, out, copy,
                               bc1 ? PixelFormat::BC1_RGBA_UNORM : PixelFormat::BC3_UNORM);
    return WriteFile(argv[5], out) ? 0 : 1;
}

int CmdRoundtrip(int argc, char** argv) {
    // roundtrip <in.astc> <w> <h> <bw> <bh> [bc1|bc3] [--no-psnr-bar]
    if (argc != 7 && argc != 8 && argc != 9) {
        std::fprintf(stderr, "usage: roundtrip <in.astc> <w> <h> <bw> <bh> [bc1|bc3] "
                             "[--no-psnr-bar]\n");
        return 2;
    }
    const u32 w = std::stoul(argv[3]);
    const u32 h = std::stoul(argv[4]);
    const u32 bw = std::stoul(argv[5]);
    const u32 bh = std::stoul(argv[6]);
    const bool bc1 = argc >= 8 && std::strcmp(argv[7], "bc1") == 0;
    if (argc >= 8 && !bc1 && std::strcmp(argv[7], "bc3") != 0) {
        std::fprintf(stderr, "roundtrip: codec must be bc1 or bc3, got %s\n", argv[7]);
        return 2;
    }
    std::vector<u8> astc;
    if (!ReadFile(argv[2], astc)) {
        return 1;
    }
    const size_t blocks_x = (w + bw - 1) / bw;
    const size_t blocks_y = (h + bh - 1) / bh;
    if (astc.size() != blocks_x * blocks_y * 16) {
        std::fprintf(stderr, "roundtrip: expected %zu ASTC bytes, got %zu\n",
                     blocks_x * blocks_y * 16, astc.size());
        return 1;
    }
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto ms = [](auto t0, auto t1) {
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    std::vector<u8> rgba(static_cast<size_t>(w) * h * 4);
    auto t0 = now();
    Tegra::Texture::ASTC::Decompress(astc, w, h, 1, bw, bh, rgba);
    auto t1 = now();

    const size_t bc_blocks = ((w + 3) / 4) * ((h + 3) / 4);
    std::vector<u8> bc(bc_blocks * (bc1 ? 8 : 16));
    if (bc1) {
        Tegra::Texture::BCN::CompressBC1(rgba, w, h, 1, bc);
    } else {
        Tegra::Texture::BCN::CompressBC3(rgba, w, h, 1, bc);
    }
    auto t2 = now();

    std::vector<u8> back(static_cast<size_t>(w) * h * 4);
    BufferImageCopy copy = CopyFor(w, h, bc1 ? PixelFormat::BC1_RGBA_UNORM
                                             : PixelFormat::BC3_UNORM);
    VideoCommon::DecompressBCn(bc, back, copy,
                               bc1 ? PixelFormat::BC1_RGBA_UNORM : PixelFormat::BC3_UNORM);
    auto t3 = now();

    double max_rgb = 0.0, max_a = 0.0;
    const double psnr_rgb = Psnr(rgba, back, 4, 0, 3, max_rgb);
    const double psnr_a = Psnr(rgba, back, 4, 3, 1, max_a);
    std::printf("roundtrip %ux%u astc %ux%u -> %s\n", w, h, bw, bh, bc1 ? "BC1" : "BC3");
    std::printf("  sizes: astc=%zu bc=%zu rgba=%zu bytes\n", astc.size(), bc.size(), rgba.size());
    std::printf("  ms: astc_decode=%.2f bc_encode=%.2f bc_decode=%.2f\n", ms(t0, t1), ms(t1, t2),
                ms(t2, t3));
    std::printf("  psnr_rgb=%.2f dB max_err_rgb=%.0f psnr_alpha=%.2f dB max_err_alpha=%.0f\n",
                psnr_rgb, max_rgb, psnr_a, max_a);
    // Sanity bar: a lossy BC transcode of a real photo-like texture should
    // stay well above 20 dB; below that something in the chain is wrong
    // (wrong block size, wrong row pitch, swapped channels). Pure random
    // noise is the exception: a 4x4 block of 16 random colors cannot be
    // represented by BC's 4-color palette, so --no-psnr-bar exists for that
    // stress case (the chain still has to run end to end).
    if (argc == 9 && std::strcmp(argv[8], "--no-psnr-bar") == 0) {
        return 0;
    }
    return (psnr_rgb >= 20.0) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: eden_texcode <astc-decode|bc1-encode|bc3-encode|"
                             "bc1-decode|bc3-decode|roundtrip> ...\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "astc-decode") {
        return CmdAstcDecode(argc, argv);
    }
    if (cmd == "bc1-encode") {
        return CmdBcEncode(argc, argv, true);
    }
    if (cmd == "bc3-encode") {
        return CmdBcEncode(argc, argv, false);
    }
    if (cmd == "bc1-decode") {
        return CmdBcDecode(argc, argv, true);
    }
    if (cmd == "bc3-decode") {
        return CmdBcDecode(argc, argv, false);
    }
    if (cmd == "roundtrip") {
        return CmdRoundtrip(argc, argv);
    }
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
}
