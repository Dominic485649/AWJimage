# AWJimage

[中文](README.md)

AWJimage is a batch image converter for Windows and Linux, with a desktop interface and CLI. Current development version: **1.3.7** (local candidate), including the application and C ABI SDK.

## Formats

| Use | Formats |
|---|---|
| Input | AVIF, HEIC/HEIF, JPEG, PNG, WebP, JXL, BMP, GIF, TIFF and camera RAW; JXR on Windows |
| Output | AVIF, PNG, WebP, JXL, JPG |
| Animation | GIF, APNG, WebP and AVIF retain animation timing, loops and transparency when converted to AVIF or WebP; single frames use still encoding |
| HDR | Single-layer SDR/HDR stays single-layer; real paired renditions retain gain maps; HDR JPEG can generate an SDR base, while SDR-only output automatically rolls off highlights |

Defaults: AVIF, quality 70, speed 5. JPG uses 8-bit YCbCr and follows source sampling in auto mode, like AVIF: known 4:2:0, 4:2:2 and 4:4:4 are preserved; RGB, RGBA and unknown sources use 4:4:4. Grayscale JPG sources use three-component 4:4:4 in auto mode; explicit 4:0:0 is unsupported. HDR JPG stores an SDR base image and an ISO 21496-1 gain map. Only ISO gain-map metadata is accepted; legacy Adobe, Google and Apple gain-map protocols are no longer supported. Lossless behavior at quality 100 depends on the format. JPEG can be losslessly repackaged as JXL. See [Gain Map and animation details](docs/gain-map-animation.md).

Wide gamut, high bit depth and display brightening do not add an HDR layer to SDR input. JXL/AVIF retain an HDR base and a map reconstructing SDR only for an existing SDR/HDR pair; ordinary single-layer input stays single-layer. JPEG is the exception: when HDR has no existing SDR rendition, highlight roll-off generates an SDR base and a gain map preserves HDR. WebP automatically rolls HDR down to SDR, preferring an existing SDR rendition; PNG preserves the selected single HDR/SDR rendition. Even `--jpeg-hdr hdr` keeps SDR-only input as ordinary SDR instead of relabeling it HDR.

JXL automatically selects 10/12/16-bit output from the selected rendition's effective precision, retaining 16-bit sources and honoring explicit 8/10/12/16-bit settings. Lossy primary images use XYB; lossless primary images and numeric gain maps retain the original mode. Quality 100 means lossless primary pixels at the target bit depth: reducing 16-bit input to 10/12-bit first is not sample-exact preservation of the original, and regenerated paired renditions are not declared sample-exact as a whole. Reversible JPEG bitstream repackaging requires explicit 8-bit output; automatic depth uses pixel encoding.

Formats share one target-pixel conversion stage, with Spline36 for geometric resampling. Size modes include automatic, unlimited, maximum-factor scaling, minimum-factor scaling and a fixed canvas. Scaling combines optional width, height, long-edge, short-edge and percentage values; positive fractional percentages and values above 100% are supported. Fixed mode accepts dimensions or a percentage: entering dimensions disables percentage input and places the full aspect-preserved image in a centered canvas with transparent or black padding; entering only a percentage disables the four dimension inputs and scales proportionally. HDR luminance analysis, gain-map reconstruction and animation compositing follow each format's requirements.

## Download and requirements

Download published builds from [GitHub Releases](https://github.com/Dominic485649/AWJimage/releases).

- Windows x64: Windows 10 or newer and an AVX2 CPU. Open `AWJ.exe` for the desktop interface; use `AWJ.com` for the CLI. AWJ.com can convert independently; the GUI and context-menu converter require both programs in the same directory.
- Linux x64: an x86-64-v3 CPU, glibc 2.39 or newer, fontconfig and FreeType; X11 or Wayland for the desktop interface. Run `./AWJ` to open it.
- Conversion and visual-quality metrics (luma, GMSD and MS-SSIM) use the CPU. The interface continues to support hardware-accelerated and software rendering.
- Keep `LICENSE` and `NOTICE.txt` beside the application. Versions 1.0.12 and earlier require a manual upgrade.

## Quick start

Drop images or folders into the queue, select an output directory and format, then click **Start conversion**. Save settings as presets. The parameter page can install Windows Explorer context menus; Linux supports Nautilus Scripts and Thunar UCA.

The queue shows source sizes and output sizes after successful conversion. Drag a header divider to resize only the column on its left. Column widths stay fixed when the window is resized; use the bottom scrollbar to view overflowing columns. Right-click the header to choose visible columns. Widths and visibility are retained for the current session.

Reimport completed images without clearing the queue. Entries not reimported retain their completed results; new and reimported entries become pending. Deduplicate normalized paths at the last valid import position without changing output-collision policies. Importing remains locked during encoding and cancellation cleanup.

Settings are stored in `AWJ.jsonc` beside the application. User presets live in `preset/*.jsonc`. The queue initially uses the built-in defaults. Selecting a user preset stores its filename in `queue_preset_file` and restores that selection on the next launch. Selecting the built-in defaults clears the saved selection; deleted or invalid presets also fall back to the built-in defaults.

### CLI arguments in presets

Starting with 1.3.7, newly saved user presets always include these two keys before `formats`. Edit the JSONC file to use them. Built-in defaults are not stored as preset files and do not have this feature:

```jsonc
{
  "schema": 1,
  "name": "Custom encoding",
  "description": "",
  "shell_menu": false,
  "cli_args_high_priority": ["--quality", "90"],
  "cli_args_low_priority": ["--timeout-encode", "30"],
  "formats": {}
}
```

Each string is one CLI argument token. Separate options from values; a path or template containing spaces remains one string, without an executable name or shell quoting. Both keys default to `[]`. Priority, highest first: **`cli_args_high_priority` → current UI / explicit CLI arguments → visible settings in `formats` → `cli_args_low_priority` → application defaults**. This example forces quality 90 and supplies a 30-minute encoding timeout not exposed by the interface. Normal CLI format and mutual-exclusion rules still apply.

Desktop queues on both platforms, CLI preset loading and saved user-preset context-menu conversion share this rule. Arguments affect execution without changing visible controls or `formats`; the high layer may explicitly override run logging, source recycling and other conversion switches. A high-priority `--input` cannot replace an existing UI file queue; use the CLI for that override. Presets do not run PowerShell, Bash or external programs, and cannot embed help, version, recursive preset selection or internal `--studio-*` controls. Each array accepts at most 256 tokens, with 4096 bytes per token and 16 KiB total.

Valid older presets are migrated and written back on load: missing root fields, formats and format fields are filled; a missing name uses the filename; extra fields and their subtrees are preserved as `//` comments with their original paths. Existing comments remain inactive and are neither repeatedly wrapped nor re-enabled. JSONC accepts `//` and ordinary `/* ... */` comments, but not nested block comments. Invalid values, malformed files and migration write failures produce an error without overwriting the original file.

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

`--threads auto` reserves desktop capacity from the allowed logical CPUs. `--threads N` uses a total compute-thread budget of N (1–128) without that reservation. File concurrency and per-image processing share the effective budget T and remain subject to memory admission. Batches redistribute integer remainders and idle tail capacity at safe decode, resize/conversion and encode boundaries; ordinary images and AVIF Grid share admission without changing priority. Synchronous codec calls cannot grow midway through a call. Single-image serial paths in JPEGli/libpng, WebP's internal threading switch, I/O and finite memory can still keep measured busy cores below T: configured threads are not measured CPU utilization. `--cpu-affinity` uses zero-based logical CPU IDs and can only narrow the currently allowed set. Windows IDs refer to the current processor group.

Use `--avif-progressive` for automatic two-layer still AVIF, or configure 2–4 layers with per-layer dimensions and quality through the CLI. It remains off by default, without new UI controls. See [progressive AVIF and layer options](docs/avif-progressive.md).

Memory planning separates decode, preparation, encoding, metrics and output peaks, then refines reservations using decoded pixels, alpha and sampling. Uncertain inputs may refine exclusively within a finite budget; refinement does not implicitly lift that budget. CSV diagnostics preserve initial estimates, refinements and reservations. Reservations are not allocator hard limits.

## Memory protection

With protection enabled, the automatic working-set budget is `min(80% of total memory, 50% of currently available memory)`. A manual memory value specifies working-set admission. Single-file inputs and primary image/encoded buffers are protected at 80% of the execution environment's physical memory.

Settings expose only **Lift the working-set memory budget**; input/buffer protection follows the same control rather than appearing separately. Enabling it overrides automatic and manual budgets and removes artificial memory-byte limits for the current image operation. Manual values for regular and context-menu parameters are retained and become effective again when the control is cleared. The unlock is session-only and never saved to configuration or presets; use `--unlock-memory-limit` in the CLI. The compatibility option `--unlock-max-input-file-bytes` remains input/buffer-only and does not implicitly lift the working-set budget.

Lifting protection does not create unlimited physical memory: the OS can still run out of memory, and actual allocation failures remain errors. Format representation, structural counts, integer-overflow and cancellation checks stay in force. Non-configurable limits in the pinned codecs also remain: the libavif decoding API accepts at most 268,435,456 pixels; LibRaw has a separate compile-time input-file size limit, 2 GiB−1 for regular RAW and either 2 GiB−1 or 4 GiB−1 for DNG depending on its SDK build configuration. These are not session-configurable AWJ memory budgets. The SDK continues to honor caller-provided context/frame/memory contracts and does not inherit the application's UI unlock state.

## Queue logs and source files

**Write run log** produces both `log/awj.log` and `log/summery.csv` under the output directory; CSV no longer has a separate queue option. CLI `--log` and its compatibility alias `--summary` enable both; `--no-log`/`--no-summary` disable both, with the last switch taking effect.

The default collision policy is **Append number**: existing outputs produce `name(1).ext`, `name(2).ext`, and so on. Overwrite, skip, time and random suffix policies remain available. Double-click a queue image to open its source before encoding, or its actual output after successful encoding.

**Delete source files** is off by default. When enabled, each source is moved to the system recycle bin immediately after its output has been successfully written. Failed, canceled or skipped items retain their sources. If recycling is unavailable or fails, the source is retained with a warning; there is no permanent-deletion fallback. Linux uses system GIO and retains the source if it is unavailable. Link paths, hard links or other input dependencies that cannot be safely verified also retain their sources with a warning. CLI `--delete-source`/`--no-delete-source` control the same behavior.

## C ABI SDK

Windows / Linux dynamic SDKs provide one set of C headers for C / C++ integration. Submit CPU frames, shared-memory frames, optional FFmpeg frames or Windows D3D11 textures and receive encoded results in memory. Packages include CMake files, `awj-stream` and integration examples. Windows SDK builds use MSVC and Linux SDK builds use Clang 23, with C99 / C++11 / C++17 consumer examples.

A complete CPU C ABI SDK is also provided for HMOS ARM64/API26. Starting with 1.3.5, HMOS uses formal cross-build release acceptance, retaining target-program and consumer linking, export, license and archive checks. On-device loading and execution are not verified.

Download `AWJ_SDK_Win_x64.zip`, `AWJ_SDK_Linux_x64.tar.gz` or `AWJ_SDK_HMOS_arm64_api26.tar.gz` from the same Release. See the [SDK documentation](docs/sdk.md) for integration, copy diagnostics, color and platform requirements.

## Build

See [CONTRIBUTING.md](CONTRIBUTING.md) for dependencies and platform commands, and the [release guide](docs/release.en.md) for packaging.

## License and community

[AGPL-3.0](LICENSE). Third-party licenses are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) and in the release package's `NOTICE.txt`.

[GitHub repository and issues](https://github.com/Dominic485649/AWJimage) · [QQ community](https://qm.qq.com/q/Wt4Ssh1igi)
