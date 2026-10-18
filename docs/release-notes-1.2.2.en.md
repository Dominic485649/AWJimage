# AWJimage 1.2.2

This update fixes blurry text inside rounded regions of the Windows interface, improves small-text clarity at high DPI, and strengthens native-rendering and scaling regressions. It also fixes PNG conversion from high-bit-depth AVIF inputs and improves Linux build compatibility. Codec dependency versions and platform requirements remain unchanged.

## Fixes

- Fix blurry queue counters and other text in rounded regions at scaling factors such as 150% and in some 200% layouts. Slint/FemtoVG resampled rounded offscreen layers at fractional physical coordinates. Align the texture, clipping path and coverage rectangle to the physical pixel grid together.
- Preserve the original clipping bounds so aligned content cannot extend beyond the parent layer's cached area. Keep font families, sizes, weights, corner radii and application DPI awareness unchanged.
- The issue was reproducible without MacType. Patched native glyph checks also passed with MacType modules loaded. This fix does not require disabling MacType or changing its configuration.
- Fix PNG conversion failures for native high-bit-depth AVIF inputs. Normalize PNG storage depth to 8/16-bit and convert 10/12-bit source precision to 16-bit PNG through the existing pixel converter instead of requesting an unsupported PNG depth.

## Maintenance and validation

- Reapply the idempotent Slint patches during configuration so cached dependency sources receive the fix. Slint remains pinned to 1.18.1; codec dependency versions are unchanged.
- Include the public standard headers required by native codec modules and let CMake select the Linux update module, fixing standard-module build compatibility with Clang/libc++.
- Add real Winit/FemtoVG glyph and clipping-boundary regressions covering Chinese and Latin text, rectangular clips, rounded clips, nested rounded clips and parent-cache boundaries. Register desktop/OpenGL checks only when explicitly enabled.
- Fix scale-test coverage that repeatedly tried to change one window's constant scale factor. Capture the production UI component through a native event loop and check the actual scale, frame dimensions, light/dark themes and navigation.
- Native Windows validation covers glyph checks at 100%, 125%, 150% and 200%, plus production UI component captures at 150% and 200% with HarmonyOS Sans SC and Microsoft YaHei UI.

These automated checks use per-process Slint content scaling without changing Windows display settings. Actual system-DPI changes and moving windows between monitors are outside this validation scope.

## Requirements

- Windows x64: Windows 10 or newer and an AVX2 CPU. Keep `AWJ.exe` and `AWJ.com` in the same directory for GUI and context-menu conversion.
- Linux x64: an x86-64-v3 CPU, glibc 2.39 or newer, fontconfig and FreeType; X11 or Wayland for the desktop interface.
- Keep `LICENSE` and `NOTICE.txt` after extraction. Versions 1.0.12 and earlier require a manual upgrade.

See the [README](https://github.com/Dominic485649/AWJimage/blob/1.2.2/README.en.md) for usage and the [CHANGELOG](https://github.com/Dominic485649/AWJimage/blob/1.2.2/CHANGELOG.en.md) for the full history.
