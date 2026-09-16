# AWJimage

Chinese: [README.md](README.md)

The source version is **1.0.13**. See the [local validation record (Chinese)](docs/validation-1.0.13.md). Published versions and downloads are on [GitHub Releases](https://github.com/Dominic485649/AWJimage/releases).

AWJimage is a C++23 / Slint batch image converter. Windows and Linux now share the same mainline. The conversion path is native-only:

- AVIF: libavif/AOM and automatic AOM Grid; Windows and Linux Release builds link them statically.
- PNG: libpng; q100 bypasses quantization, q1–99 quantize RGB precision.
- WebP: libwebp
- JXL: libjxl
- JPGLI: google/jpegli; produces JPEG-compatible bitstreams with the default `.jpg` extension.
- HEIC/HEIF input: native libheif 1.23.4 + libde265 1.1.2 decoding; only HEVC decode support is built, with HEIC encoders and dynamic plugins disabled.

The built-in ImageMagick/MagickWand backend has been removed. Magick and ffmpeg can only return later as explicit external integrations.

Linux keeps one ELF `AWJ` for both Slint UI and CLI. Visual-quality GPU metrics use Vulkan and fall back to CPU on failure, tiny images, or resource limits. HEIC/HEIF uses the cross-platform native libheif/libde265 decoder; WIC, JXR, `AWJ.com`, and Windows registry shell integration remain Windows-only. Linux hides WIC fallback UI and provides user-level Nautilus Scripts plus Thunar UCA actions without sudo.

## Release archives

| Archive | Exact contents |
|---|---|
| `AWJ_Win.7z` | `AWJ.exe`, `AWJ.com`, `LICENSE`, `NOTICE.txt` |
| `AWJ_Linux.7z` | `AWJ`, `LICENSE`, `NOTICE.txt` |

Both archives are flat and use maximum LZMA2 compression, one thread, and no automatic filter (`-m0=lzma2 -mx=9 -mmt=1 -mf=off`). Linux packaging and executable-bit verification run on a native Linux filesystem. Release notes provide actual sizes and SHA-256 hashes; `NOTICE.txt` combines build information, third-party notices, and full license texts.

## Build

Windows (MSVC):

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ AWJ-com
```

Linux — the portable presets bind no private toolchain; the compiler and vcpkg both come from the environment:

```bash
export VCPKG_ROOT=/path/to/vcpkg
export CC=gcc-16 CXX=g++-16      # only if the default cc/c++ lacks C++23 modules
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release --target AWJ
```

Prerequisites:

- A C++23-modules-capable compiler (GCC 16+ or Clang 20+), plus Ninja and CMake 3.30+
- vcpkg with `VCPKG_ROOT` pointing at the checkout; dependencies come from the `vcpkg.json` manifest
- Linux system build tools: `autoconf`, `autoconf-archive`, `automake`, and `libtool` (required when vcpkg builds libsodium)
- Rust toolchain (`cargo` on `PATH`) to build Slint.
- Vulkan via `find_package(Vulkan REQUIRED)`, satisfied by vcpkg `vulkan-headers` / `vulkan-loader`
- DXC to compile the visual_quality shader to SPIR-V at build time; vcpkg `directx-dxc` is preferred, otherwise `dxc` from `PATH`

Use `linux-x64-debug` for Debug. The `linux-gcc-x64-*` and `linux-clang-x64-*` presets are maintainer-only: they hardcode `$HOME/.local/gcc-16.1-deb/wrappers/*-awj` and `$HOME/.local/vcpkg`. Use the portable presets everywhere else.

Maintainer flow (WSL local tree, preferred `/home/dominic/Code/Cpp/AWJimage`; avoid building on `/mnt/d`):

```bash
cd /home/dominic/Code/Cpp/AWJimage
cmake --preset linux-gcc-x64-release
cmake --build --preset linux-gcc-x64-release --target AWJ
```

Both portable and maintainer Release presets use `-O3`, IPO/LTO, `-march=x86-64-v3`, section GC/strip, and statically link `libstdc++` / `libgcc`. Only `AWJ` is produced; there is no `AWJ.com`.

Windows release script:

```powershell
# Local/development build without publishing an update manifest
.\release.ps1 -SkipUpdateManifest
```

> Since 1.0.4, `release.ps1` is Windows-build-only and rejects manifest writes. The cross-platform archive, v2/v1 signing, and GitHub publication procedure is [docs/release.en.md](docs/release.en.md).

From 1.0.9, the public `AWJ_Win.7z` package is an exact flat four-file archive containing only `AWJ.exe`, `AWJ.com`, `LICENSE`, and `NOTICE.txt`. `.sha256` sidecars, `BUILD_INFO.txt`, `THIRD_PARTY_NOTICES.txt`, and license subdirectories are not packaged. `NOTICE.txt` consolidates build information, third-party notices, and required full license texts, while the signed v2 manifest continues to bind the archive and every member by size and SHA-256. The local maintainer output may additionally retain `awj_update_manifest_sign.exe`; that tool is never part of the public archive.

Versioning is controlled by the root `VERSION` file. A real release must use an Ed25519 seed stored outside the repository and compile the matching public key into the client. Never commit the seed, copy it into the output directory, or print it in logs. The actual 1.0.4+ archive, signing, and GitHub publication steps are maintained in [docs/release.en.md](docs/release.en.md).

## Automatic updates

From 1.0.6, clients use a keyring verified by at least two of three compiled roots to choose a non-revoked release key, then verify v1/v2 manifests carrying `key_id`, monotonic sequence, `issued_at`, and `expires_at`. From 1.0.13 the updater prefers `update-keyring.json` / `update-archive.json` plus their signatures, and falls back to legacy names only when the new pair is missing; signature, corruption, and replay failures never downgrade. Each document type persists its last verified sequence and raw SHA-256 beside the executable with an inter-process lock, flush, and atomic replace. Windows and writable Linux installs still stage on the same volume, atomically replace, health-check, and roll back; unwritable Linux installs open the Release page without silent elevation.

Legacy schema-1 `update-manifest.json` remains only for the local Windows 1.0.3→1.0.5 bridge; 1.0.5 is not a v2 candidate. See [update signing and key rotation](docs/update-security.en.md) for custody, expiry, revocation, and rotation.

## CLI

Run `AWJ` with no arguments for Studio; CLI arguments select command-line conversion. Windows may use the sibling `AWJ.com` shim, while Linux runs the single `AWJ` ELF.

```powershell
AWJ -i '"D:\example.jxr"' -o '"D:\output"' --format avif
AWJ -i input.png --format webp --preset "My preset" --quality 85
AWJ -i input.png --preset-file .\preset\my-preset.jsonc --format jxl
AWJ -i input.png --preserve-creation-time --preserve-modification-time
```

`-i` / `-o` trim surrounding whitespace and one complete pair of double quotes; empty paths, unmatched quotes, and embedded quotes fail clearly. `--list-presets` dynamically lists valid executable-adjacent `preset/*.jsonc` files and reports invalid ones; `-p/--preset <name>` loads one by name and `--preset-file <path>` loads a specified JSONC. Without a user preset, AWJ uses current built-in defaults; explicit CLI options always override it regardless of argument order. AVIF default speed is 5.

Windows CLI also offers `--preserve-creation-time`, `--preserve-modification-time`, and `--preserve-access-time`; it writes them only after atomic output commit. A write failure produces an stderr/log warning without discarding valid output. Linux neither lists nor accepts those Windows-only options.

Windows CLI can consume one piped WGC HDR frame: `capture-tool | AWJ --stdin-wgc-rgba16f 3840x2160 -o D:\output --format avif`. It accepts exactly one `DXGI_FORMAT_R16G16B16A16_FLOAT` frame (little-endian RGBA binary16, linear scRGB), requires explicit dimensions and `-o`, and cannot be combined with `-i`; short data, extra-frame bytes, and unknown bare RAW are rejected rather than guessed.

The Studio queue snapshots output format, user preset, metadata removal, and the three Windows timestamp choices at Start. It defaults to AVIF, retaining metadata, and no timestamps. Queue choices are independent from the Parameters editor and context-menu settings; the editor only switches among five format parameter groups, and a user preset must be selected explicitly in the queue.

## visual_quality GPU metrics

`--visual-quality` 1..99 repeatedly encodes candidates, decodes them from memory, and scores visual metrics. Default `--visual-quality-gpu` accelerates the metrics path only:

- Windows: Direct3D 11 compute
- Linux: Vulkan compute

Candidate encode/decode and final encoded-bytes selection remain native CPU pipelines. This is not end-to-end GPU transcoding. Failures fall back to CPU and are recorded as `cpu-fallback`.

## Large-image mode

AVIF inputs over the single-image limits (65536 edge / `2^30` pixels) enter the automatic large-image chain:

AOM Grid preserves the original dimensions. Unavailable encoding, invalid grid plans, and input/runtime memory-limit failures are reported explicitly.

Studio no longer has a separate large-image page; automatic large-image status stays in the main queue. Inputs above 10 MP but still under single-image limits stay in the ordinary queue tail so one large memory estimate cannot throttle every small-file worker. Each stage selects concurrency from CPU, memory and task limits before assigning threads per file. Grid supports smaller right/bottom edge cells for non-divisible dimensions while preserving the original output size.

PNG defaults to q100 without additional pixel quantization; decoded pixels remain identical when no other pixel transform is requested. q1–99 quantizes RGB one row at a time and rescales values to the full range. Storage stays 8-bit or 16-bit, with at least 10 effective bits for 16-bit input, while alpha retains its original precision. Valid per-channel `sBIT` is read and preserved, with quantized RGB precision recorded separately. PNG does not support visual-quality search. Lower quality does not guarantee smaller files; check gradients and dark regions for banding.

`--alpha auto` retains non-opaque alpha automatically. AVIF color and alpha both use the requested quality or visual-quality result; `--chroma auto` preserves the source representation: YUV 4:2:0/4:2:2/4:4:4 sources are kept as-is, RGB/RGBA sources use 4:4:4, and grayscale or unknown sources use 4:2:0. Lossless or 4:4:4 output does not automatically select Identity. Explicit `source/rgb` color representation controls RGB/GBR Identity. q100 permits byte-stream passthrough only for a YUV 4:2:0 AVIF with no requested color, alpha, bit-depth, or metadata rewrite; all other inputs use lossless AOM quantization and those auto rules. CICP precedence is explicit user value, then source value, then fallback, so BT.2020/PQ/HLG HDR sources keep their own CICP and BT.709/sRGB applies only when neither supplies one. Source CICP range is retained by default (PC/full or TV/limited); unknown range uses full. `--alpha off` removes alpha.

CLI session unlock (not written to `AWJ.jsonc`):
- `--unlock-max-input-file-bytes` / `--unlock-20gib-limit` removes the default 20 GiB input/runtime cap for the current process only; huge images may OOM.

Studio auto threading reserves 4 logical threads at >=12, 2 at 5-11, and 1 at 2-4; a single-thread system still uses 1. Automatic memory uses the smaller of 80% of total memory and 50% of currently available memory, falling back to the available source when only one is readable.

## Reproducible benchmark

The current Windows benchmark is CLI-only and uses the 613-file `D:\图片\benchmark\test` directory. It measures AWJ's default AVIF behavior and a strict ffmpeg comparison:

```powershell
pwsh -NoProfile -File .\scripts\benchmark.ps1 `
  -InputRoot 'D:\图片\benchmark\test' `
  -AwjExecutable .\bin\x64\Release\AWJ.exe `
  -FfmpegExecutable 'D:\DevTools\Cli\FFmpeg\ffmpeg.exe' `
  -PowerSchemeGuid 381b4222-f694-41f0-9685-ff5bb260df2e `
  -Mode All
```

The current protocol passes no quality, speed, chroma, bit-depth, or memory option to AWJ, so it verifies the true defaults: AOM, quality 70, speed 5, source-aware automatic chroma, automatic at-least-10-bit output, and automatic retention of non-opaque alpha at q70. The strict comparison fixes ffmpeg to AOM QP 23, 10-bit 4:2:0, all-intra, row-mt, and pixel/byte/path ordering. The runner validates actual `summary.csv` settings before recording Job Object process-tree CPU and peak memory.

The default `All` mode runs both regression and strict comparison; `Regression` runs AWJ only and `Strict` runs the 210 opaque images without ICC/EXIF/XMP against ffmpeg. Non-smoke runs use one warmup and five measured runs per scenario, nearest-rank P95, and a 30-second cooldown. Results go under `build/benchmarks/`; a fastest single run is not a version conclusion.

### 0.10.3 strict-comparison archive

The following values were recorded on 2026-07-16 with the older q80/8-bit protocol. They are historical data, not output reproducible by the current runner. The tested builds were AWJ 0.10.3 MSVC Release, commit `2798db2`, AOM 3.13.3, and ffmpeg `git-2026-07-14-312c830916`, AOM 3.14.1.

### Large-image capability

| Tool | Result | Wall | Process CPU | Sampled peak | Output |
| --- | --- | ---: | ---: | ---: | ---: |
| AWJ | AOM grid encode succeeded | 167.554 s | 892.016 s | 23,707.0 MiB | 164.39 MiB |
| ffmpeg | MJPEG decoder rejected the input before libaom | 0.246 s (time to failure) | n/a | 309.5 MiB | none |

The validated ffmpeg signature is `MJPEG packet ... too big`. Its 0.246 seconds is cached-input failure latency, not encoding throughput, so no large-image speed ratio is reported.

### 613-file results

| Tool | Schedule | Success / failure | Wall median / P95 | Process CPU median / P95 | Sampled peak median / P95 | Throughput | Median output |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| AWJ | 12 files x 1 encoder thread | 612 / 1 | 110.878 / 111.891 s | 940.047 / 956.859 s | 2810.0 / 2820.5 MiB | 5.529 img/s, 14.668 MP/s | 128.98 MiB |
| ffmpeg | 12 processes x 1 encoder thread | 612 / 1 | 107.671 / 109.772 s | 855.688 / 865.672 s | 3242.1 / 3275.8 MiB | 5.693 img/s, 15.104 MP/s | 131.48 MiB |

The five wall-time samples are 111.364, 109.919, 110.883, 111.891, and 107.727 seconds for AWJ, versus 109.772, 108.981, 106.784, 107.671, and 106.767 seconds for ffmpeg. The ffmpeg/AWJ median wall ratio is 0.97x: ffmpeg is about 2.9% faster under this machine and protocol, while AWJ's sampled median peak memory is about 13.3% lower. This is not evidence of a large AWJ speedup.

AWJ's summed `decode / prepare / encode / write` stage median/P95 values are 37.529/38.040, 0.278/0.291, 1077.766/1090.467, and 8.916/9.609 seconds. Stage values sum concurrent per-image durations and can exceed batch wall time; the multi-process ffmpeg run does not expose a comparable stage split.

This is an end-to-end CLI comparison, not a same-libaom-build microbenchmark, and it does not establish equal visual quality. The tools use different AOM versions, build options, and decode/color-conversion front ends. Fixed AWJ-then-ffmpeg order can give AWJ a lower initial temperature and ffmpeg a filesystem-cache advantage. These results describe only this machine and protocol; they do not attribute the difference to AWJ glue code or any AOM parameter.

## Studio and verification

Custom combos, buttons, navigation, and queue menus expose keyboard focus and accessibility metadata. The queue shows pending/running/success/failure counts, failed-only filtering, retry, and complete item details including paths, error text, encoder threads, and stage timings. The font list remains limited to ten visible rows with wheel and scrollbar support and no search/manual entry.

`scripts/cli-worker-smoke.ps1` launches no UI. It validates the real CLI manifest ITEM/DETAIL stream, a failed-item retry manifest, named-event cancellation, and Job Object force termination. A small headless Slint component test covers navigation, keyboard input, font scrolling, light/dark state, 820x560, and 100%/150%/200% scale. 0.10.4 Windows MSVC Release passes 31/31 tests; the 55,614,152-byte Linux GCC 16.1 Release passes 16/16 and has no dynamic `libstdc++`/`libgcc_s` dependency.

## Platform notes

| Feature | Windows | Linux |
| --- | --- | --- |
| UI + CLI single binary | `AWJ.exe` (+ optional `AWJ.com`) | `AWJ` ELF |
| visual_quality GPU | D3D11 | Vulkan |
| WIC / JXR | yes | no |
| Shell integration | registry context menu | Nautilus Scripts + Thunar UCA |
| Preferred compiler | MSVC | GCC 16 side-by-side |

Animated or multi-image WebP/GIF/APNG/JXL/TIFF/AVIF, Windows WIC inputs, and JPEG MPF inputs are flattened to their composed first frame. Extraction failures are reported instead of silently retaining extra frames.

Windows Explorer multi-selection launches are coalesced into one shell window and one queue. The shell window offers graceful cancellation and force termination; closing either UI terminates active work first.

## Docs

- Chinese migration notes: [docs/cpp-port.md](docs/cpp-port.md)
- English migration notes: [docs/cpp-port.en.md](docs/cpp-port.en.md)
- Chinese release procedure: [docs/release.md](docs/release.md)
- English release procedure: [docs/release.en.md](docs/release.en.md)
- Update security: [中文](docs/update-security.md) / [English](docs/update-security.en.md)
- Changelog (Chinese): [CHANGELOG.md](CHANGELOG.md)
