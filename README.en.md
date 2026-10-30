# AWJimage

[中文](README.md)

AWJimage is a batch image converter for Windows and Linux, with a desktop interface and CLI. Current source version: **1.3.0 (unreleased development)**; published downloads remain those listed on Releases.

## Formats

| Use | Formats |
|---|---|
| Input | AVIF, HEIC/HEIF, JPEG, PNG, WebP, JXL, BMP, GIF, TIFF and camera RAW; JXR on Windows |
| Output | AVIF, PNG, WebP, JXL, JPG |
| Animation | GIF, APNG, WebP and AVIF retain frame timing, loops and transparency when converted to AVIF |
| HDR | HDR images and supported Gain Maps; JPEG output can include an HDR Gain Map |

Defaults: AVIF, quality 70, speed 5. JPG uses 8-bit YCbCr and follows source sampling in auto mode, like AVIF: known 4:2:0, 4:2:2 and 4:4:4 are preserved; RGB, RGBA and unknown sources use 4:4:4. Grayscale JPG sources use three-component 4:4:4 in auto mode; explicit 4:0:0 is unsupported. HDR JPG stores an SDR base image and an ISO 21496-1 gain map. Only ISO gain-map metadata is accepted; legacy Adobe, Google and Apple gain-map protocols are no longer supported. Lossless behavior at quality 100 depends on the format. JPEG can be losslessly repackaged as JXL. See [Gain Map and animation details](docs/gain-map-animation.md).

Formats share one target-pixel conversion stage. Compatible YUV layouts are reused directly; matrix, depth, sampling and geometry changes are combined to reduce full-image intermediate buffers. HDR luminance analysis, gain-map reconstruction and animation compositing follow each format's requirements.

## Download and requirements

Download published builds from [GitHub Releases](https://github.com/Dominic485649/AWJimage/releases).

- Windows x64: Windows 10 or newer and an AVX2 CPU. Open `AWJ.exe` for the desktop interface; use `AWJ.com` for the CLI. AWJ.com can convert independently; the GUI and context-menu converter require both programs in the same directory.
- Linux x64: an x86-64-v3 CPU, glibc 2.39 or newer, fontconfig and FreeType; X11 or Wayland for the desktop interface. Run `./AWJ` to open it.
- Regular conversion uses the CPU. Unavailable graphics acceleration falls back to CPU metrics and software UI rendering.
- Keep `LICENSE` and `NOTICE.txt` beside the application. Versions 1.0.12 and earlier require a manual upgrade.

## Quick start

Drop images or folders into the queue, select an output directory and format, then click **Start conversion**. Save settings as presets. The parameter page can install Windows Explorer context menus; Linux supports Nautilus Scripts and Thunar UCA.

The queue shows source sizes and output sizes after successful conversion. Drag a header divider to resize only the column on its left. Column widths stay fixed when the window is resized; use the bottom scrollbar to view overflowing columns. Right-click the header to choose visible columns. Widths and visibility are retained for the current session.

Reimport completed images without clearing the queue. Entries not reimported retain their completed results; new and reimported entries become pending. Deduplicate normalized paths at the last valid import position without changing output-collision policies. Importing remains locked during encoding and cancellation cleanup.

Settings are stored in `AWJ.jsonc` beside the application. User presets live in `preset/*.jsonc`. The queue initially uses the built-in defaults. Selecting a user preset stores its filename in `queue_preset_file` and restores that selection on the next launch. Selecting the built-in defaults clears the saved selection; deleted or invalid presets also fall back to the built-in defaults.

## CLI

Use `AWJ.com` on Windows; replace it with `./AWJ` on Linux.

```powershell
# Batch conversion with default AVIF settings
.\AWJ.com -i .\images -o .\output
# Choose format, quality and speed
.\AWJ.com -i .\image.png -o .\output --format webp -q 90 --speed 5
# Select a thread budget and logical CPUs
.\AWJ.com -i .\images -o .\output --threads 10 --cpu-affinity 0-4,9-13
# List options and presets
.\AWJ.com --help
.\AWJ.com --list-presets
```

`--threads auto` reserves desktop capacity from the allowed logical CPUs. `--threads N` uses a total encoding thread budget of N (1–128) without that reservation; file concurrency and per-image encoder threads share this budget and remain subject to the memory budget. `--cpu-affinity` uses zero-based logical CPU IDs and can only narrow the currently allowed set. Windows IDs refer to the current processor group.

## C ABI SDK (unreleased)

Other applications can submit CPU, shared-memory, FFmpeg or Windows D3D11 frames through the same C headers for memory encoding. There is no public C++ ABI or built-in capture session/player. Implementation uses C++23 and targets Clang 21+; Windows and Linux are separate builds, while HarmonyOS still requires the real SDK/device. See the [SDK documentation](docs/sdk.md) for copying, color, lifetime and platform contracts. Ordinary pipes and GPU readback are not end-to-end zero-copy paths.

## Build

See [CONTRIBUTING.md](CONTRIBUTING.md) for dependencies and platform commands, and the [release guide](docs/release.en.md) for packaging.

## License and community

[AGPL-3.0](LICENSE). Third-party licenses are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and in the release package's `NOTICE.txt`.

[GitHub repository and issues](https://github.com/Dominic485649/AWJimage) · [QQ community](https://qm.qq.com/q/Wt4Ssh1igi)
