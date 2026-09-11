"""Create fresh, deterministic PNG inputs for the resource benchmark."""
from pathlib import Path
import hashlib
import json
import os
import runpy
import sys

import numpy as np
from PIL import Image


def main():
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=False)
    for group in ("large", "mixed", "grid", "stress"):
        (root / group).mkdir()
    for i in range(4):
        for group, width, height in (("large", 4096, 4096), ("mixed", 2048, 1536)):
            x = np.arange(width, dtype=np.uint32)[None, :]
            y = np.arange(height, dtype=np.uint32)[:, None]
            pixels = np.empty((height, width, 3), dtype=np.uint8)
            pixels[:, :, 0] = (x // 16 + y // 16 + i * 23) % 256
            pixels[:, :, 1] = (x // 8 + y // 32 + i * 41) % 256
            pixels[:, :, 2] = (x // 32 + y // 8 + i * 61) % 256
            Image.fromarray(pixels).save(root / group / f"sample-{i}.png", compress_level=1)
    for i in range(2):
        os.link(root / "large" / f"sample-{i}.png", root / "mixed" / f"large-{i}.png")
        width = 65537 + i * 2
        pixels = np.empty((64, width, 3), dtype=np.uint8)
        pixels[:, :, 0] = np.arange(width, dtype=np.uint32)[None, :] % 256
        pixels[:, :, 1] = np.arange(64, dtype=np.uint8)[:, None] * 4
        pixels[:, :, 2] = 96 + i * 32
        Image.fromarray(pixels).save(root / "grid" / f"wide-{width}.png", compress_level=1)
    write_png = runpy.run_path(str(Path(__file__).with_name("png-quality-samples.py")))["write_png"]
    rng = np.random.default_rng(20260910)
    for depth in (8, 16):
        # Full RGBA noise exercises high-depth and nonopaque alpha buffers.
        pixels = rng.integers(0, 1 << depth, (4096, 4096, 4), dtype=np.uint8 if depth == 8 else np.uint16)
        write_png(root / "stress" / f"rgba-noise-{depth}.png", pixels)
    manifest = {}
    for path in sorted(root.glob("*/*.png")):
        with Image.open(path) as image:
            manifest[str(path.relative_to(root))] = {"dimensions": image.size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    (root / "samples.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
