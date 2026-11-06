#!/usr/bin/env python3
"""eden_offline_pack.py -- build and verify Eden offline texture packs (v1).

Reads a capture directory produced by the EDEN_OFFLINE_CAPTURE hook in
Eglantine (see src/video_core/texture_cache/offline_capture.h):
    <capture>/manifest.jsonl          one JSON object per ASTC upload
    <capture>/textures/<gpuaddr>.astc raw (still compressed) ASTC bytes

and produces a versioned package the loader can use instead of transcoding
at runtime:
    <pack>/manifest.json              package manifest (format v1)
    <pack>/textures/<gpuaddr>_L<level>.bc1|.bc3
    <pack>/shaders/vulkan.bin               (copied in by warm_caches.sh)
    <pack>/shaders/vulkan_pipelines.bin     (copied in by warm_caches.sh)

Transcoding itself is done by eden_texcode (tools/offline-pack/texcode/),
which links Eden's own CPU ASTC decoder (textures/astc.cpp), Eden's CPU BC
encoder (textures/bcn.cpp, stb_dxt) and Eden's BC decoder
(texture_cache/decode_bc.cpp). This tool never re-implements a codec.

Scope of v1 (honest limits, see docs/offline-pack-design.md):
  * Only manifest entries with layout "linear" and layers == 1, depth == 1
    are transcoded. Entries with layout "swizzled"/"swizzled-tail" are
    listed as skipped (offline unswizzle is not implemented yet).
  * One mip level is transcoded at a time; the dump is split into levels
    using the ASTC block size table below.
  * Opaque ASTC (manifest astc_opaque=true) -> BC1 (8 B/block, half the
    size); otherwise -> BC3 (16 B/block). Same rule the Vulkan runtime uses.
  * No NUP input: the game textures live inside the game archives and only
    reach guest memory at runtime, so capture is the only input path.

Subcommands:
  build    --capture-dir D --out P [--texcode T]  build a pack
  verify   --pack P --capture-dir D [--texcode T] re-encode + compare + PSNR
  selftest --workdir W [--texcode T] [--astcenc A]
           synthetic images -> real ASTC (astcenc) -> texcode roundtrip;
           prints sizes, timings and PSNR. Synthetic data only, no game
           textures: the numbers prove the chain works, not game gains.

All sizes/timings printed are measured on this machine (253); nothing is
extrapolated to the PS5 except where explicitly labelled "estimate".
"""

import argparse
import hashlib
import json
import resource
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# ASTC format table.
# Generated from Eglantine src/video_core/surface.h (PIXEL_FORMAT_LIST,
# enum order == numeric value) at submodule 08598c9984:
#   python3 -c "parse PIXEL_FORMAT_ELEM lines, keep ASTC_*"
# Regenerate with that command if surface.h changes; do not hand-edit.
# name -> (format_id, block_w, block_h). All ASTC blocks are 16 bytes.
# ---------------------------------------------------------------------------
ASTC_FORMATS = {
    33: ("ASTC_2D_4X4_UNORM", 4, 4),
    60: ("ASTC_2D_8X8_UNORM", 8, 8),
    61: ("ASTC_2D_8X5_UNORM", 8, 5),
    62: ("ASTC_2D_5X4_UNORM", 5, 4),
    70: ("ASTC_2D_4X4_SRGB", 4, 4),
    71: ("ASTC_2D_8X8_SRGB", 8, 8),
    72: ("ASTC_2D_8X5_SRGB", 8, 5),
    73: ("ASTC_2D_5X4_SRGB", 5, 4),
    74: ("ASTC_2D_5X5_UNORM", 5, 5),
    75: ("ASTC_2D_5X5_SRGB", 5, 5),
    76: ("ASTC_2D_10X8_UNORM", 10, 8),
    77: ("ASTC_2D_10X8_SRGB", 10, 8),
    78: ("ASTC_2D_6X6_UNORM", 6, 6),
    79: ("ASTC_2D_6X6_SRGB", 6, 6),
    80: ("ASTC_2D_10X6_UNORM", 10, 6),
    81: ("ASTC_2D_10X6_SRGB", 10, 6),
    82: ("ASTC_2D_10X5_UNORM", 10, 5),
    83: ("ASTC_2D_10X5_SRGB", 10, 5),
    84: ("ASTC_2D_10X10_UNORM", 10, 10),
    85: ("ASTC_2D_10X10_SRGB", 10, 10),
    86: ("ASTC_2D_12X10_UNORM", 12, 10),
    87: ("ASTC_2D_12X10_SRGB", 12, 10),
    88: ("ASTC_2D_12X12_UNORM", 12, 12),
    89: ("ASTC_2D_12X12_SRGB", 12, 12),
    90: ("ASTC_2D_8X6_UNORM", 8, 6),
    91: ("ASTC_2D_8X6_SRGB", 8, 6),
    92: ("ASTC_2D_6X5_UNORM", 6, 5),
    93: ("ASTC_2D_6X5_SRGB", 6, 5),
}

PACK_VERSION = 1


def level_dims(w, h, level):
    return max(1, w >> level), max(1, h >> level)


def level_astc_bytes(w, h, bw, bh, level):
    lw, lh = level_dims(w, h, level)
    return ((lw + bw - 1) // bw) * ((lh + bh - 1) // bh) * 16


def expected_dump_bytes(entry):
    """Total linear ASTC bytes for a single-layer 2D entry, or None if the
    entry shape is outside v1 scope."""
    fmt = ASTC_FORMATS.get(entry["format"])
    if fmt is None:
        return None
    if entry.get("layers", 1) != 1 or entry.get("depth", 1) != 1:
        return None
    _, bw, bh = fmt
    return sum(level_astc_bytes(entry["width"], entry["height"], bw, bh, lv)
               for lv in range(entry["levels"]))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, timeout=600):
    """Run a command; return (rc, seconds, stdout, stderr)."""
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, time.monotonic() - t0, p.stdout, p.stderr


def child_peak_rss_kib():
    """Peak RSS of waited-for child processes (ru_maxrss, KiB on Linux)."""
    return resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

def cmd_build(args):
    capture = Path(args.capture_dir)
    out = Path(args.out)
    texcode = args.texcode
    manifest_path = capture / "manifest.jsonl"
    if not manifest_path.is_file():
        print(f"no manifest: {manifest_path}", file=sys.stderr)
        return 1
    entries = []
    for line in manifest_path.read_text().splitlines():
        line = line.strip()
        if line:
            entries.append(json.loads(line))
    print(f"capture: {len(entries)} manifest entries from {manifest_path}")

    tex_dir = out / "textures"
    tex_dir.mkdir(parents=True, exist_ok=True)
    pack_entries = []
    n_ok = n_skip = n_err = 0
    total_astc = total_bc = 0
    total_ms = 0.0
    for e in entries:
        addr = e["gpu_addr"]
        fmt = ASTC_FORMATS.get(e["format"])
        rec = dict(e)
        rec["format_name"] = fmt[0] if fmt else f"UNKNOWN_{e['format']}"
        rec["levels_out"] = []
        if fmt is None:
            rec["status"] = "skipped-unknown-format"
            n_skip += 1
            pack_entries.append(rec)
            continue
        if e.get("layout") != "linear":
            rec["status"] = f"skipped-layout-{e.get('layout')}"
            n_skip += 1
            pack_entries.append(rec)
            continue
        if e.get("layers", 1) != 1 or e.get("depth", 1) != 1:
            rec["status"] = "skipped-multilayer-or-3d"
            n_skip += 1
            pack_entries.append(rec)
            continue
        if not e.get("dump"):
            rec["status"] = "skipped-no-dump"
            n_skip += 1
            pack_entries.append(rec)
            continue
        dump = capture / e["dump"]
        if not dump.is_file():
            rec["status"] = "error-dump-missing"
            n_err += 1
            pack_entries.append(rec)
            continue
        data = dump.read_bytes()
        expected = expected_dump_bytes(e)
        if expected is None or len(data) != expected:
            rec["status"] = f"error-size-mismatch got={len(data)} expected={expected}"
            n_err += 1
            pack_entries.append(rec)
            continue
        _, bw, bh = fmt
        codec = "bc1" if e.get("astc_opaque") else "bc3"
        ok = True
        offset = 0
        tmp = Path(tempfile.mkdtemp(prefix="offline-pack-"))
        for lv in range(e["levels"]):
            lw, lh = level_dims(e["width"], e["height"], lv)
            nbytes = level_astc_bytes(e["width"], e["height"], bw, bh, lv)
            level_in = tmp / f"{addr[2:]}-L{lv}.astc"
            level_in.write_bytes(data[offset:offset + nbytes])
            offset += nbytes
            level_out = tex_dir / f"{addr[2:]}_L{lv}.{codec}"
            rc, dt, so, se = run([texcode, "astc-decode", str(level_in),
                                  str(lw), str(lh), str(bw), str(bh),
                                  str(level_in) + ".rgba"])
            if rc != 0:
                print(f"  {addr} L{lv}: astc-decode failed: {se.strip()}", file=sys.stderr)
                ok = False
                break
            rc, dt2, so, se = run([texcode, f"{codec}-encode",
                                   str(level_in) + ".rgba", str(lw), str(lh),
                                   str(level_out)])
            total_ms += (dt + dt2) * 1000.0
            if rc != 0:
                print(f"  {addr} L{lv}: {codec}-encode failed: {se.strip()}", file=sys.stderr)
                ok = False
                break
            rec["levels_out"].append({
                "level": lv, "width": lw, "height": lh,
                "file": f"textures/{level_out.name}",
                "bytes": level_out.stat().st_size,
                "sha256": sha256_file(level_out), "codec": codec,
            })
            total_astc += nbytes
            total_bc += level_out.stat().st_size
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
        rec["status"] = "ok" if ok else "error-transcode"
        if ok:
            n_ok += 1
        else:
            n_err += 1
        pack_entries.append(rec)

    manifest = {
        "version": PACK_VERSION,
        "created_unix": int(time.time()),
        "capture_dir": str(capture),
        "tool": "eden_offline_pack.py (offline-pack prototype v1)",
        "entries": pack_entries,
        "stats": {
            "manifest_entries": len(entries),
            "transcoded": n_ok, "skipped": n_skip, "errors": n_err,
            "astc_bytes_in": total_astc, "bc_bytes_out": total_bc,
            "ratio_bc_over_astc": (total_bc / total_astc) if total_astc else None,
            "transcode_ms_total": round(total_ms, 1),
            "peak_child_rss_kib": child_peak_rss_kib(),
        },
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"pack: {out}  ok={n_ok} skipped={n_skip} errors={n_err}")
    print(f"  astc_in={total_astc} B bc_out={total_bc} B "
          f"ratio={manifest['stats']['ratio_bc_over_astc']} "
          f"transcode_ms={total_ms:.1f}")
    print(f"  manifest: {out / 'manifest.json'}")
    return 0 if n_err == 0 else 2


# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------

def cmd_verify(args):
    pack = Path(args.pack)
    capture = Path(args.capture_dir)
    texcode = args.texcode
    manifest = json.loads((pack / "manifest.json").read_text())
    n_ok = n_bad = n_skip = 0
    for e in manifest["entries"]:
        if e.get("status") != "ok":
            n_skip += 1
            continue
        fmt = ASTC_FORMATS.get(e["format"])
        _, bw, bh = fmt
        dump = capture / e["dump"]
        data = dump.read_bytes()
        offset = 0
        good = True
        tmp = Path(tempfile.mkdtemp(prefix="offline-pack-verify-"))
        for lv in e["levels_out"]:
            nbytes = level_astc_bytes(e["width"], e["height"], bw, bh, lv["level"])
            level_in = tmp / f"{e['gpu_addr'][2:]}-L{lv['level']}.astc"
            level_in.write_bytes(data[offset:offset + nbytes])
            offset += nbytes
            # 1) fresh re-encode must be byte-identical to the packaged file
            rc, _, _, se = run([texcode, "astc-decode", str(level_in),
                                str(lv["width"]), str(lv["height"]),
                                str(bw), str(bh), str(level_in) + ".rgba"])
            if rc != 0:
                print(f"  {e['gpu_addr']} L{lv['level']}: astc-decode failed", file=sys.stderr)
                good = False
                break
            fresh = tmp / f"{e['gpu_addr'][2:]}-L{lv['level']}.{lv['codec']}"
            rc, _, _, se = run([texcode, f"{lv['codec']}-encode",
                                str(level_in) + ".rgba", str(lv["width"]),
                                str(lv["height"]), str(fresh)])
            if rc != 0:
                good = False
                break
            if sha256_file(fresh) != lv["sha256"]:
                print(f"  {e['gpu_addr']} L{lv['level']}: re-encode hash mismatch",
                      file=sys.stderr)
                good = False
                break
            # 2) full round trip (decode->encode->decode) must clear the PSNR bar
            rc, _, so, se = run([texcode, "roundtrip", str(level_in),
                                 str(lv["width"]), str(lv["height"]),
                                 str(bw), str(bh), lv["codec"]])
            psnr = ""
            for ln in so.splitlines():
                if "psnr_rgb" in ln:
                    psnr = ln.strip()
            print(f"  {e['gpu_addr']} L{lv['level']}: {psnr} (roundtrip rc={rc})")
            if rc != 0:
                good = False
                break
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
        if good:
            n_ok += 1
        else:
            n_bad += 1
    print(f"verify: ok={n_ok} bad={n_bad} skipped(non-ok entries)={n_skip}")
    return 0 if n_bad == 0 else 2


# ---------------------------------------------------------------------------
# selftest: synthetic images -> real ASTC (astcenc) -> texcode roundtrip
# ---------------------------------------------------------------------------

def write_tga(path, w, h, kind, seed=1):
    """Uncompressed TGA (24-bit RGB, or 32-bit RGBA for the alpha case),
    bottom-origin rows. TGA is used for every selftest image because
    astcenc 4.7.0 rejects the PPM files this tool used to write
    ("unknown image type"), while TGA loads fine.
    Deterministic; no external deps."""
    import random
    rnd = random.Random(seed)
    px = bytearray()
    has_alpha = kind == "alpha"
    for y in range(h):  # bottom row first (descriptor 0 = bottom origin)
        for x in range(w):
            if kind == "gradient":
                r, g, b = (x * 255 // max(1, w - 1), y * 255 // max(1, h - 1),
                           (x + y) * 255 // max(1, w + h - 2))
                a = 255
            elif kind == "checker":
                v = 255 if ((x // 16) + (y // 16)) % 2 else 0
                r, g, b = v, 255 - v, (x * 7 + y * 13) % 256
                a = 255
            elif kind == "alpha":
                r = x * 255 // max(1, w - 1)
                g = y * 255 // max(1, h - 1)
                b = 128
                a = (x * 255 // max(1, w - 1)) if (x < w // 2) else 255 - (y * 255 // max(1, h - 1))
            else:  # noise: high-frequency, hardest case for BC
                r, g, b = rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)
                a = 255
            if has_alpha:
                px += bytes((b, g, r, a))  # TGA stores BGRA
            else:
                px += bytes((b, g, r))
    hdr = bytearray(18)
    hdr[2] = 2  # uncompressed true-color
    hdr[12] = w & 0xFF
    hdr[13] = (w >> 8) & 0xFF
    hdr[14] = h & 0xFF
    hdr[15] = (h >> 8) & 0xFF
    hdr[16] = 32 if has_alpha else 24
    with open(path, "wb") as f:
        f.write(bytes(hdr))
        f.write(bytes(px))


def read_tga_rgba(path):
    """Read an uncompressed (type 2) or RLE-compressed (type 10) 24/32-bit
    TGA into (w, h, bytes RGBA). astcenc writes RLE TGAs on decode."""
    with open(path, "rb") as f:
        hdr = f.read(18)
        assert len(hdr) == 18
        idlen, cmaptype, imgtype = hdr[0], hdr[1], hdr[2]
        w = hdr[12] | (hdr[13] << 8)
        h = hdr[14] | (hdr[15] << 8)
        bpp = hdr[16]
        top_origin = bool(hdr[17] & 0x20)
        assert imgtype in (2, 10) and bpp in (24, 32), \
            f"unsupported TGA {imgtype}/{bpp}"
        f.read(idlen)
        step = bpp // 8
        raw = bytearray()
        if imgtype == 2:
            raw = f.read(w * h * step)
            assert len(raw) == w * h * step
        else:  # RLE packets
            while len(raw) < w * h * step:
                head = f.read(1)
                assert head, "truncated RLE TGA"
                head = head[0]
                count = (head & 0x7F) + 1
                if head & 0x80:
                    px = f.read(step)
                    assert len(px) == step
                    raw += px * count
                else:
                    raw += f.read(count * step)
            assert len(raw) == w * h * step
    px = bytearray()
    for i in range(w * h):
        b, g, r = raw[i * step], raw[i * step + 1], raw[i * step + 2]
        a = raw[i * step + 3] if step == 4 else 255
        px += bytes((r, g, b, a))
    if not top_origin:
        # rows are bottom-up; flip to top-down
        row = w * 4
        px = b"".join(bytes(px[i:i + row]) for i in range(len(px) - row, -1, -row))
    return w, h, bytes(px)


def psnr_rgba(a, b):
    import math
    assert len(a) == len(b)
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)
    if mse == 0:
        return 99.0
    return 10.0 * math.log10(255.0 * 255.0 / mse)


def cmd_selftest(args):
    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)
    texcode = args.texcode
    astcenc = args.astcenc
    if not Path(astcenc).is_file():
        print(f"selftest needs astcenc, not found: {astcenc}", file=sys.stderr)
        print("fetch it with tools/offline-pack/texcode/setup-deps.sh", file=sys.stderr)
        return 1
    cases = [
        # (kind, w, h, block, bw, bh, opaque) -- opaque=True means every
        # pixel has alpha 1, i.e. the texture qualifies for BC1, exactly the
        # rule the pack builder applies from manifest astc_opaque.
        ("gradient", 256, 256, "4x4", 4, 4, True),
        ("checker", 256, 256, "4x4", 4, 4, True),
        ("noise", 256, 256, "4x4", 4, 4, True),
        ("gradient", 512, 512, "8x8", 8, 8, True),
        ("checker", 512, 256, "6x6", 6, 6, True),
        ("noise", 128, 128, "5x4", 5, 4, True),
        ("alpha", 256, 256, "4x4", 4, 4, False),
    ]
    print("selftest: synthetic images (gradient/checker/noise/alpha) encoded to REAL "
          "ASTC blocks by astcenc, then Eden texcode roundtrip. No game data.")
    all_ok = True
    for kind, w, h, blk, bw, bh, opaque in cases:
        src = work / f"{kind}-{w}x{h}.tga"
        write_tga(src, w, h, kind)
        astc = work / f"{kind}-{w}x{h}-{blk}.astc"
        rc, enc_s, _, se = run([astcenc, "-cl", str(src), str(astc), blk,
                                "-medium", "-silent"])
        if rc != 0:
            print(f"  {kind} {w}x{h} {blk}: astcenc failed rc={rc}: {se[:200]}",
                  file=sys.stderr)
            all_ok = False
            continue
        # astcenc writes a .astc container (16-byte header + blocks); the
        # capture hook dumps the raw block stream, so strip the header.
        raw = astc.read_bytes()
        if raw[:4] != b"\x13\xab\xa1\x5c":
            print(f"  {kind}: unexpected astcenc output magic {raw[:4]!r}", file=sys.stderr)
            all_ok = False
            continue
        blocks = work / f"{kind}-{w}x{h}-{blk}.raw"
        blocks.write_bytes(raw[16:])
        # BC1 is only valid for opaque textures (it has 1-bit alpha); the
        # builder applies it exactly when manifest astc_opaque is true, so
        # the selftest mirrors that rule instead of testing BC1 on alpha.
        # Noise is a stress case: per-pixel random colors cannot be
        # represented by BC's 4-color palette, so its PSNR bar is waived
        # (--no-psnr-bar); the chain still has to run end to end.
        codecs = ("bc3", "bc1") if opaque else ("bc3",)
        for codec in codecs:
            cmd = [texcode, "roundtrip", str(blocks),
                   str(w), str(h), str(bw), str(bh), codec]
            if kind == "noise":
                cmd.append("--no-psnr-bar")
            rc, dt, so, se = run(cmd)
            lines = [ln.strip() for ln in so.splitlines() if ln.strip()]
            status = "OK " if rc == 0 else "FAIL"
            if rc != 0:
                all_ok = False
            tag = "STRESS" if kind == "noise" else "     "
            print(f"  [{status}] {tag} {kind} {w}x{h} astc{blk} -> {codec} "
                  f"({dt * 1000:.1f} ms): " + " | ".join(lines))
    # Independent cross-check: decode the same ASTC file with astcenc itself
    # and with Eden's decoder, and compare. Two independent ASTC decoders
    # must agree; this guards against a systematic error in Eden's decoder
    # that a roundtrip alone could hide (both halves would share the error).
    print("cross-check: astcenc decode vs Eden astc-decode (gradient 256x256 4x4):")
    astc_file = work / "gradient-256x256-4x4.astc"
    ref_tga = work / "gradient-256x256-ref.tga"
    eden_rgba = work / "gradient-256x256-eden.rgba"
    rc, _, _, se = run([astcenc, "-dl", str(astc_file), str(ref_tga), "-silent"])
    if rc != 0:
        print(f"  astcenc decode failed rc={rc}: {se[:200]}", file=sys.stderr)
        all_ok = False
    else:
        raw = astc_file.read_bytes()[16:]
        raw_file = work / "gradient-256x256-4x4.raw"
        raw_file.write_bytes(raw)
        rc, _, _, se = run([texcode, "astc-decode", str(raw_file), "256", "256",
                            "4", "4", str(eden_rgba)])
        if rc != 0:
            print(f"  Eden astc-decode failed rc={rc}", file=sys.stderr)
            all_ok = False
        else:
            w2, h2, ref = read_tga_rgba(ref_tga)
            eden = eden_rgba.read_bytes()
            assert (w2, h2) == (256, 256) and len(eden) == len(ref)
            maxdiff = max(abs(x - y) for x, y in zip(ref, eden))
            print(f"  astcenc-vs-eden: psnr={psnr_rgba(ref, eden):.2f} dB "
                  f"max_abs_diff={maxdiff}")
            if maxdiff > 2:
                print("  WARNING: decoders disagree by more than rounding",
                      file=sys.stderr)
                all_ok = False
    print("selftest:", "ALL OK" if all_ok else "FAILURES (see above)")
    return 0 if all_ok else 2


def main():
    ap = argparse.ArgumentParser(description="Eden offline texture pack tool (v1)")
    ap.add_argument("--texcode", default="tools/offline-pack/texcode/build/eden_texcode",
                    help="path to the eden_texcode helper binary")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("build", help="build a pack from a capture directory")
    p.add_argument("--capture-dir", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_build)

    p = sub.add_parser("verify", help="re-encode and roundtrip-check a pack")
    p.add_argument("--pack", required=True)
    p.add_argument("--capture-dir", required=True)
    p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("selftest", help="synthetic-image roundtrip test")
    p.add_argument("--workdir", required=True)
    p.add_argument("--astcenc", default="",
                    help="path to astcenc binary (default: auto-find)")
    p.set_defaults(fn=cmd_selftest)

    args = ap.parse_args()
    if args.cmd == "selftest" and not args.astcenc:
        for cand in (Path.home() / ".cache" / "eden-offline-pack" / "astcenc-4.7.0"
                     / "bin" / "astcenc-avx2",
                     Path("/tmp/offline-pack-deps/astcenc47/bin/astcenc-avx2"),
                     Path("/tmp/offline-pack-deps/astcenc47/bin/astcenc-sse2")):
            if cand.is_file():
                args.astcenc = str(cand)
                break
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
