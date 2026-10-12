"""Crop the supplied RGBA artwork and export the application icons (Pillow)."""
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[1]
with Image.open(root / "ui/logo-source.png") as source:
    assert source.mode == "RGBA" and source.size == (1774, 887)
    for theme, box, ico in [
        ("dark", (0, 0, 887, 887), "app-dark.ico"),
        ("light", (887, 0, 1774, 887), "app.ico"),
    ]:
        cropped = source.crop(box)
        png = root / f"ui/logo-{theme}.png"
        cropped.save(png)
        cropped.save(root / "src/ui" / ico, sizes=[
            (size, size) for size in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
        ])
        with Image.open(png) as actual:
            assert actual.mode == "RGBA" and actual.tobytes() == cropped.tobytes()
        with Image.open(root / "src/ui" / ico) as actual:
            assert (16, 16) in actual.info["sizes"] and (256, 256) in actual.info["sizes"]
