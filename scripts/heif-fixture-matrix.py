"""Generate synthetic HEVC acceptance inputs; never used by the shipped decoder.

Run with a disposable environment containing pillow-heif and numpy. Records the
generator versions and input hashes so a passing conversion has a traceable input.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import pillow_heif


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    width, height = 160, 128
    y, x = np.indices((height, width), dtype=np.uint32)
    rgb16 = np.stack(((x * 419) % 65536, (y * 521) % 65536,
                     ((x + y) * 233) % 65536), axis=-1).astype("<u2")
    cases = []
    for depth in (8, 10, 12):
        for chroma in ("420", "422", "444"):
            for transfer in ((1,) if depth == 8 else (1, 16, 18)):
                name = f"hevc-{chroma}-{depth}-tr{transfer}.heic"
                mode = "RGB" if depth == 8 else "RGB;16"
                pixels = (rgb16 >> 8).astype("u1") if depth == 8 else rgb16
                image = pillow_heif.from_bytes(mode, (width, height), pixels.tobytes())
                image.save(args.output / name, quality=95, chroma=chroma, bit_depth=depth,
                           save_nclx_profile=True, color_primaries=1 if transfer == 1 else 9,
                           transfer_characteristics=transfer,
                           matrix_coefficients=1 if transfer == 1 else 9, full_range_flag=1,
                           enc_params={"x265:pools": "1", "x265:frame-threads": "1"})
                probe = pillow_heif.open_heif(args.output / name, convert_hdr_to_8bit=False)
                assert probe.info["bit_depth"] == depth, (name, probe.info)
                assert str(probe.info["chroma"]) == chroma, (name, probe.info)
                assert probe.info["nclx_profile"]["transfer_characteristics"] == transfer, (name, probe.info)
                cases.append({"name": name, "width": width, "height": height,
                              "chroma": chroma, "depth": depth, "transfer": transfer,
                              "primaries": 1 if transfer == 1 else 9,
                              "sha256": hashlib.sha256((args.output / name).read_bytes()).hexdigest()})
    (args.output / "fixtures.json").write_text(json.dumps(
        {"pillow_heif": pillow_heif.__version__, "generator": pillow_heif.libheif_info(),
         "cases": cases}, indent=2), encoding="utf-8")
    print(f"Generated {len(cases)} HEVC fixtures in {args.output}")


if __name__ == "__main__":
    main()
