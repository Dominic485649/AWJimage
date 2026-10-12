# AWJimage 1.2.1

This update improves the standalone Windows CLI, CPU and memory controls, and desktop layout and themes.

## Optimization

- Unify target-pixel conversion. Prefer native YUV and reuse compatible layouts; combine matrix, depth, sampling, HDR and geometry changes to reduce full-image intermediate buffers. Retain required HDR luminance analysis, gain-map reconstruction and animation compositing.
- The Windows conversion core runs independently in `AWJ.com`. `AWJ.exe` handles the GUI, context menus and updates, and delegates conversion to `AWJ.com` in the same directory.
- Automatic threads reserve desktop capacity from the allowed logical CPUs. Explicit `--threads N` uses the full thread budget. New `--cpu-affinity` accepts logical CPU IDs and ranges.
- Ordinary large images enter the normal encoding queue; Grid uses separate scheduling. Improve memory estimates and reduce intermediate buffers for HDR conversion and JPG encoding.
- Use JPG as the format name. Default to 8-bit YCbCr 4:2:0, with 4:2:2 and 4:4:4 available. Preserve HDR Gain Maps and compatibility with previous options.
- Reuse reference images and candidate buffers in CPU visual-quality searches. Initialize DX11, Vulkan and WIC on demand; unavailable optional graphics features fall back to CPU metrics and software UI rendering.

## Interface

- Narrow the sidebar and center the AWJ title. Fix naming buttons, selectors, checkbox alignment and settings-page spacing.
- Match sidebar and caption colors and restore section cards. Fix rounded scrolling viewports, borders and rendering during continuous resizing.
- Merge changelog preferences into the default-enabled “Only show changelog after updates”, synchronized with the changelog page. Keep update controls and status on one row.
- Replace light/dark icons and add GitHub repository and QQ community links to About.

## Requirements

- Windows x64: Windows 10 or newer and an AVX2 CPU. Keep `AWJ.exe` and `AWJ.com` in the same directory for GUI and context-menu conversion.
- Linux x64: an x86-64-v3 CPU, glibc 2.39 or newer, fontconfig and FreeType; X11 or Wayland for the desktop interface.
- Keep `LICENSE` and `NOTICE.txt` after extraction. Versions 1.0.12 and earlier require a manual upgrade.

See the [README](https://github.com/Dominic485649/AWJimage/blob/master/README.en.md) for usage and the [CHANGELOG](https://github.com/Dominic485649/AWJimage/blob/master/CHANGELOG.en.md) for the full history.
