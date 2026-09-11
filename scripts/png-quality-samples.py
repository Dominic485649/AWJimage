"""Generate deterministic PNG calibration samples; keep downloaded fixtures in build/.

Run from the repository root, then pass the output directory to
awj_png_codec_tests --benchmark INPUTS FRESH_OUTPUTS.
Requires the development environment's existing NumPy and Pillow.
"""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import sys
import zlib

import numpy as np
from PIL import Image


def chunk(name, data):
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


def write_png(path, pixels, hdr=False):
    depth = 16 if pixels.dtype == np.uint16 else 8
    rows = pixels.astype(">u2" if depth == 16 else "u1").reshape(pixels.shape[0], -1)
    raw = b"".join(b"\0" + row.tobytes() for row in rows)
    header = struct.pack(">IIBBBBB", pixels.shape[1], pixels.shape[0], depth, 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                     + (chunk(b"cICP", bytes([9, 16, 0, 1])) if hdr else b"")
                     + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=False)
    deps = Path(sys.argv[2])
    sources = {}
    for name, source in {
        "photo-paris": deps / "libavif-src/tests/data/paris_icc_exif_xmp.png",
        "photo-weld-16": deps / "libavif-src/tests/data/weld_16bit.png",
        "screenshot": deps / "slint-src/tools/viewer/android/fastlane/metadata/android/en-US/images/phoneScreenshots/1.png",
    }.items():
        shutil.copyfile(source, output / (name + ".png"))
        sources[name] = str(source)
    y, x = np.mgrid[:512, :1024]
    rng = np.random.default_rng(20260909)
    for depth in (8, 16):
        full = (1 << depth) - 1
        dtype = np.uint16 if depth == 16 else np.uint8
        ramp = x / 1023
        for kind in ("gradient", "dark", "cg", "transparent-edge", "noise-low", "noise-high", "hdr-pq"):
            if kind == "hdr-pq" and depth == 8:
                continue
            rgb = np.stack([ramp, y / 511, (ramp + y / 511) / 2], axis=-1)
            alpha = np.ones_like(ramp)
            if kind == "dark":
                rgb *= 0.06
            elif kind == "cg":
                rgb = np.stack([(x // 64 % 2) * 0.83, (y // 64 % 2) * 0.62, ramp ** 0.45], axis=-1)
            elif kind == "transparent-edge":
                alpha = np.clip((220 - np.hypot(x - 512, y - 256)) / 16, 0, 1)
            elif kind.startswith("noise"):
                rgb += rng.normal(0, 0.006 if kind == "noise-low" else 0.06, rgb.shape)
            pixels = np.rint(np.clip(np.dstack([rgb, alpha]), 0, 1) * full).astype(dtype)
            name = f"{kind}-{depth}"
            write_png(output / (name + ".png"), pixels, kind == "hdr-pq")
            sources[name] = "deterministic synthetic code-value calibration, seed 20260909"
    manifest = {path.name: {"source": sources[path.stem], "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for path in sorted(output.glob("*.png"))}
    (output / "sources.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
