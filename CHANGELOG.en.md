# Changelog

## 1.0.11 - 2026-09-05

- Prerelease: Windows Explorer external file/folder drops now use native OLE `IDropTarget` + `CF_HDROP`. File selection, folder selection, Explorer drops, and command-line inputs converge on one C++ import pipeline, while Slint components/theme and the C++ import/path-picker/platform bridges are split by responsibility without changing queue deduplication, ordering, collision, or encoding semantics.
- Studio visual-quality guidance now says `Recommended: leave blank`. Blank input continues to use the regular quality parameter directly; when visual-quality search is explicitly needed, starting around 50 is recommended, with clear notice that repeated GMSD / MS-SSIM search encodes are significantly more expensive. The 1–100 range, blank default, search algorithm, and encoding behavior are unchanged.
- Normalized the Windows classic shell context menu to an HKCU-only, no-elevation static cascade with one vendor-qualified command/order source, perceived-type fallbacks, legacy/optional-command cleanup, transactional rollback, and strict drift validation. The final cascade uses the `ExtendedSubCommandsKey` REG_SZ shared-tree pointer layout empirically verified on the current Windows Shell, with the compatibility/documentation gap versus current Microsoft Learn explicitly retained.
- Embedded CHANGELOG generation now uses UTF-8-safe chunked literals to avoid MSVC C2026 oversized-string failures, with byte-exact reconstruction tests for both languages and synthetic multibyte UTF-8 boundary coverage.
- Fixed the Windows 1.0.9→1.0.10 updater rollback caused by health readiness depending on mutable `pending_update_version` cache state. Readiness is now bound to the helper-verified target version matching the running build version, while signed keyring/manifest checks, asset/member hashes, transaction/session validation, the 30-second event wait, 3-second stay-alive gate, and rollback remain intact. 1.0.11 remains a prerelease and preserves the existing updater compatibility floor so deployed 1.0.9/1.0.10 clients can directly select it.
- Release validation for this version continues to respect the safety boundary: F6, screenshot/screen-capture, capture, and raw-WGC functional tests were not executed. Directory Opus is outside this work item's acceptance scope.

## 1.0.10 - 2026-09-01

- Prerelease: reworked Studio tooltip and output-format interactions so tooltip lifetime no longer repeatedly drives repainting. Queue output order is now AVIF, AVIF.png, WebP, JXL, JPGLI, PNG, with an explicit UI-to-backend format mapping that preserves existing backend format indices. PNG is editable in parameter settings while retaining fixed lossless semantics; queue action buttons remain equal-width in one- or two-row layouts, and the visual-quality UI now documents 50 as the recommended value.
- Backported the Slint/Winit AccessKit component-lifetime guard. If a window component has already been destroyed, an accessibility-tree rebuild now returns an empty TreeUpdate instead of unwrapping a missing component, fixing the identified Windows FAST_FAIL_FATAL_APP_EXIT crash path. Per the task safety constraint, F6 and all screenshot/capture-function tests were not executed for this release.
- Fixed the applied-bit-depth regression for FP16/scRGB 16-bit sources encoded to AVIF through AOM, keeping that path at the intended 12-bit depth and adding regression coverage.

## 1.0.9 - 2026-08-30

- Prerelease: fixed the 7z updater rejecting legitimate directory entries. The extractor now skips only safe directories that are necessarily implied by signed manifest member paths, while continuing to reject extra directories, traversal, links, special entries, duplicates, and unsigned files. Regression coverage includes accepted and rejected real directory entries, restoring compatibility with older archives including 1.0.8.
- The Windows release archive is now a strict flat four-file package: `AWJ.exe`, `AWJ.com`, `LICENSE`, and `NOTICE.txt`. In-archive `.sha256` files, `BUILD_INFO.txt`, `THIRD_PARTY_NOTICES.txt`, and `THIRD_PARTY_LICENSES/` are removed; the signed v2 manifest continues to bind archive/member size and SHA-256.
- `NOTICE.txt` consolidates build information, third-party notices, and required full third-party license texts. The Linux archive is likewise flattened to exactly `AWJ`, `LICENSE`, and `NOTICE.txt`, with exact-member validation in the packagers.

## 1.0.8 - 2026-08-30

- Prerelease: AVIF now has explicit color-representation modes. The default `yuv` always uses a non-Identity matrix; `source` follows the source YUV/RGB Identity representation; `rgb` forces RGB(A)/GBR(A) Identity + 4:4:4 and selects AOM when the encoder is automatic. CLI, presets, Studio, and encoding diagnostics use the same contract.
- AVIF can optionally append `.png` to produce a complete `.avif.png` filename while the payload remains AVIF. Collision numbering, same-directory `-converted` naming, and Studio worker manifests treat the double extension as one unit.
- Fixed lossless AVIF color semantics and bitstream-passthrough eligibility: eligible YUV 420/422/444 AVIF files can preserve the original bitstream; YUV no longer switches implicitly to Identity at q100/4:4:4, invalid or unknown matrices on BT.2020 sources fall back to BT.2020 NCL, and other sources fall back to BT.709.
- Studio Parameters adds AVIF color-representation and `.avif.png` compatibility controls; presets persist the new field, queue drag/drop and multi-target scans use deterministic ordering, and queue context actions, failed-only filtering, and prerelease changelog markers are tightened.

## 1.0.7 - 2026-08-24

- Prerelease: Functionally identical to 1.0.6 and provided only for the 1.0.6→1.0.7 archive auto-update test; normal users should continue to download 1.0.6.

## 1.0.6 - 2026-08-24

- Prerelease: the updater now has a two-root-authorized release keyring, key IDs, signed `issued_at` / `expires_at`, revocation, and a maximum 180-day lifetime. v1, v2, and keyring sequence plus raw SHA-256 persist through an inter-process lock and atomic state file; rollback, changed bytes at one sequence, and damaged state fail closed, while the helper re-verifies the keyring and v2 manifest.
- Preset loading now accepts legacy `[null]` / `[integer]` `visual_quality` values and rewrites them as proper scalars on the next save, so `测试.jsonc` can again appear in Parameters and Queue. CLI adds `--list-presets` to dynamically show executable-adjacent valid presets and invalid-file diagnostics.
- Windows CLI adds `--stdin-wgc-rgba16f <width>x<height>` for one exact-length piped `DXGI_FORMAT_R16G16B16A16_FLOAT` linear-scRGB frame; it uses a secure temporary file through the existing HDR path and rejects short/long data, missing dimensions, and unknown bare RAW.
- Added a Windows 11-style rounded application icon; the three file-time choices follow “Write runtime log” and wrap on narrow windows. The changelog scrolls downward normally again, and the three automatic-update choices in Settings are horizontal.
- Fixed the sidebar “New version available” hover card: it is now opaque and clipped as a compact prompt, while the full release notes remain on the Changelog page so long summaries cannot bleed through the card or cover navigation.
- The release tooling now fixes LF raw bytes for signed keyring/v2 manifests and handles an existing one-entry manifest safely. `AWJ_Linux.7z` is packaged natively on Linux and verifies its executable bit; Windows only verifies its content and hashes.

## 1.0.5 - 2026-08-24

- Prerelease: Functionally identical to 1.0.4. This build only supplies the raw `AWJ.exe` / `AWJ.com` needed for the Windows 1.0.3→1.0.5 updater bridge test; normal users should download the 1.0.4 archive assets.

## 1.0.4 - 2026-08-21

- Prerelease: fixed changelog cards whose fixed body height clipped and overlapped text; the page now embeds the complete history from the earliest release, retaining the Chinese original for legacy entries without an English translation. The sidebar title remains centered while the version marker and rule move 16 px left.
- The encoding queue now independently selects AVIF, WebP, JXL, JPGLI, or PNG plus an optional user preset, and snapshots that choice for the whole batch at start. Ordinary queues default to AVIF, keep metadata, and do not preserve Windows creation/modification/access times. The Parameters format switch only edits one of five parameter groups; built-in defaults are not written to root config.
- Input and output paths accept one complete pair of surrounding double quotes. Native Slint drops accept files and directories regardless of the file/folder selector; output rejects multiple targets. Right-clicking a queue item no longer changes the detail selection.
- Added executable-adjacent `preset/*.jsonc` user presets with five complete format parameter sets, Unicode-safe names, strict schema validation, default fallback for missing fields, and a full-window name/description save overlay.
- `AWJ.jsonc` now white-lists settings, context-menu parameters, and update-security state; ordinary queues, paths, window state, and ordinary parameters are not persisted. Visual-quality fallback output defaults on; AVIF's default speed is 5 and an empty speed restores that value.
- Windows CLI now exposes creation/modification/access timestamp preservation. Times are captured before input read and written only after the output commits; a write failure warns without discarding a valid image. Linux CLI neither lists nor accepts those options.
- HDR-to-SDR now uses libplacebo's CPU-only spline tone mapping and perceptual gamut mapping. PQ, HLG, and FP16 scRGB follow explicit HDR semantics and target 100-nit BT.709/sRGB SDR while retaining non-color metadata.
- The updater now uses a signed v2 archive manifest: Windows/Linux download `AWJ_Win.7z` / `AWJ_Linux.7z`, verify every required member's size and SHA-256, and reject traversal, links, extra members, and archive bombs. Windows and writable Linux installs atomically replace, health-check, and roll back.
- Updated Slint, the vcpkg baseline, and release locks; added the CPU-only libplacebo overlay, libarchive 7z extraction, and their third-party notices.

## 1.0.3 - 2026-08-19

- Prerelease: fixed the lifetime of the current user's Windows proxy configuration during update requests, keeping manual proxy and PAC/auto-detected results valid for the whole request and safely falling back to direct routing when discovery is unavailable.
- Prerelease: fixed update-channel focus after switching, keeping one blue focus ring the same width as the combo box instead of leaving a black border behind.

## 1.0.2 - 2026-08-10

- Prerelease: when automatic proxy discovery (WPAD/PAC) is unavailable, fall back to WinHTTP's default route instead of rejecting a direct connection; an explicitly configured manual proxy still takes precedence.
- Prerelease: Windows update requests now honor the current user's manual proxy, PAC, and auto-detect settings; when the user profile is unavailable, WinHTTP's default proxy remains in effect and proxy addresses are never logged.
- Prerelease: the update-channel combo keeps focus while automatic checking disables interaction, so focus no longer jumps to the “Show changelog” checkbox or paints a black focus border.
- Prerelease: fixed Windows updates when LocalAppData staging and the installed application are on different volumes; the helper now copies to a same-volume temporary and atomically replaces the target while preserving rollback.
- Prerelease: fixed release-script parsing when continuing an existing manifest so sequence/version validation does not treat the field label as a version component.
- Prerelease: isolated all test executables under `bin/x64/tests/<configuration>` so cross-platform builds and release cleanup cannot overwrite test artifacts.
- Prerelease: changed the changelog page to show the complete version-sorted release history from the signed manifest; removed manual checking so update checks remain automatic.
- Prerelease: embedded the bilingual major-version history in the changelog page and merged it with verified manifest entries; removed explanatory and empty-state text.
- Prerelease: atomically persist the latest verified manifest bytes and signature with the existing config, then re-verify them at startup so the complete history remains available offline between automatic checks.

## 1.0.1 - 2026-08-09

- Added the signed update foundation. The client accepts only a static `update-manifest.json` whose original deterministic UTF-8 bytes verify against the embedded Ed25519 public key. A monotonic sequence rejects replay, versions use strict integer MAJOR.MINOR.PATCH comparison, and stable/prerelease filtering is explicit.
- Added a clickable version label, persistent update dot, and a first-class changelog page beside Queue, Parameters, and Settings. Settings now expose the update channel, changelog visibility, automatic-check status, and the last successful check time. Hovering, opening the page, navigating, restarting, or a failed check never clears the dot.
- Windows checks and downloads run on a cancellable worker with WinHTTP timeouts, response limits, and an exact host allowlist. Installation re-fetches and re-verifies the manifest, then verifies each asset's declared size and SHA-256. Updates are blocked while encoding; unwritable installs open the Release page without requesting elevation.
- The Windows helper derives the install directory from its parent process handle and can replace only sibling `AWJ.exe` and `AWJ.com`. It backs up both files, journals the transaction, replaces the pair, launches the new build, and waits for a health signal. Missing health, early exit, or any replacement failure restores the full old pair; the next launch can also recover an interrupted transaction.
- Unified update-state and normal Studio config commits on the UI thread. Windows config writes now use a same-directory temporary file, `FlushFileBuffers`, and atomic replacement. Integer config storage is 64-bit, avoiding the Unix timestamp overflow in 2038.
- Corrected AVIF q100/visual-quality 100, CICP, and clamped-grid help and documentation to match source-aware automatic chroma, HDR metadata precedence, and smaller edge cells. Benchmark result paths, titles, and profiles now derive their version from `VERSION`.
- Corrected public Windows/Linux build and packaging instructions. Archives include `LICENSE`, `THIRD_PARTY_NOTICES.txt`, and `BUILD_INFO.txt` and are followed by an archive-list check. Portable Linux presets are separated from maintainer-only compiler-wrapper presets.
- Corrected the pinned `zenravif 0.1.3` AGPL-3.0-only/commercial dual-license notice and documented the updater's libsodium and nlohmann JSON dependencies. The release script now requires bilingual changelog entries, a deterministic manifest, an explicit sequence, a signing seed, and a matching embedded public key.

## 1.0.0 - 2026-07-30

- Added instant Chinese/English Studio switching with translations bundled into the executable and persisted on Windows.
- Reworked parameter-help layout and narrow-window wrapping, fixed C++ module dependency declarations, and hardened GUI crash diagnostics and worker exception boundaries.
- Updated the dependency baseline and pinned codec/UI sources, fixed the upstream `svt-av1-hdr` chroma scaling assignment during configure, and aligned benchmark assumptions with source-aware AVIF chroma.
- Corrected automatic memory budgeting, removed the obsolete context-menu preset selector, and expanded regression coverage for defaults and UI behavior.
