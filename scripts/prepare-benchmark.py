"""Copy a reproducible corpus; source directories are read-only."""
import argparse
import csv
import hashlib
import random
import shutil
from pathlib import Path

from PIL import Image


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pixels(path):
    with Image.open(path) as image:
        if getattr(image, "n_frames", 1) != 1:
            raise ValueError("animated image")
        image.load()
        rgba = image.convert("RGBA")
        return hashlib.sha256(str(rgba.size).encode() + rgba.tobytes()).hexdigest()


def image_suffix(path):
    if path.suffix.lower() == ".jxr":
        return ".jxr"
    with Image.open(path) as image:
        return {"JPEG": ".jpg", "PNG": ".png", "WEBP": ".webp", "AVIF": ".avif",
                "GIF": ".gif", "BMP": ".bmp", "TIFF": ".tif"}.get(image.format, path.suffix.lower())

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("jxr-source", "pixpin-source", "supplement-source", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--seed", default=20261008, type=int)
    args = parser.parse_args()
    sources = [args.jxr_source.resolve(), args.pixpin_source.resolve(), args.supplement_source.resolve()]
    output = args.output.resolve()
    if any(output == source or output.is_relative_to(source) or source.is_relative_to(output) for source in sources):
        raise ValueError("output must be separate from every source")
    inputs = output / "inputs"
    if inputs.exists() and any(inputs.iterdir()):
        raise ValueError("inputs must be empty; refusing to overwrite a corpus")
    selected = [("JXR", path) for path in sorted(sources[0].rglob("*")) if path.is_file() and path.suffix.lower() == ".jxr"]
    pixpin = [path for path in sorted(sources[1].rglob("*")) if path.is_file()]
    if len(selected) != 86 or len(pixpin) != 258:
        raise ValueError(f"expected 86 JXR and 258 PixPin files, got {len(selected)} and {len(pixpin)}")
    selected.extend(("PixPin", path) for path in pixpin)
    hashes = {digest(path) for _, path in selected}
    pixel_hashes = set()
    for _, path in selected:
        try:
            pixel_hashes.add(pixels(path))
        except (OSError, ValueError):
            pass  # WIC validates JXR during conversion.
    candidates = [path for path in sorted(sources[2].rglob("*")) if path.is_file() and path.suffix.lower() in {".png", ".jpg", ".jpeg", ".webp", ".avif"}]
    random.Random(args.seed).shuffle(candidates)
    for path in candidates:
        file_hash = digest(path)
        if file_hash in hashes:
            continue
        try:
            pixel_hash = pixels(path)
        except (OSError, ValueError):
            continue
        if pixel_hash in pixel_hashes:
            continue
        selected.append(("Supplement", path))
        hashes.add(file_hash)
        pixel_hashes.add(pixel_hash)
        if len(selected) == 600:
            break
    if len(selected) != 600:
        raise ValueError("not enough unique decodable static supplemental images")
    inputs.mkdir(parents=True, exist_ok=True)
    rows = []
    for index, (category, source) in enumerate(selected, 1):
        target = inputs / f"{index:04d}{image_suffix(source)}"
        source_hash = digest(source)
        shutil.copyfile(source, target)
        if digest(target) != source_hash:
            raise OSError(f"copy hash mismatch: {target}")
        rows.append({"id": index, "category": category, "input": target.name, "source": str(source), "bytes": target.stat().st_size, "sha256": source_hash, "seed": args.seed})
    with (output / "corpus.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    print(f"Copied and hash-verified {len(rows)} files into {inputs}")


if __name__ == "__main__":
    main()
