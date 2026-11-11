# Release guide

## User review and applications

Each version requires explicit user confirmation that the optimized MSVC Windows review build is usable and explicit approval of that version's Release notes. Automated acceptance, plan approval and approvals for an earlier version do not replace either review. Linux and SDK preparation may be brought forward where useful, but the required assets and technical gates remain mandatory.

Build Windows x64 and native Linux x64 Release from the same clean source commit and matching version tag. Preserve AVX2 / x86-64-v3, IPO/LTO, runtime policy and full third-party licenses. Record targeted tests and artifact checks, identifying the source and boundaries of any reused evidence; do not rerun unrelated full suites by default.

- Windows applications and SDKs use actual MSVC `cl.exe` and matching Microsoft tools. `release.ps1 -SkipUpdateManifest -Generator Ninja -CompilerDirectory <MSVC-directory> -BuildTests` supports isolated build, runtime and dependency paths, retaining `/GL` and final `/LTCG`. Do not use clang-cl or relabel old binaries as a replacement.
- Linux uses `scripts/package-linux-release.sh --binary <native-binary> --build-directory <native-build>`. Build and package on the native WSL filesystem with Clang 23, libc++, Release IPO/LTO and x86-64-v3; reuse only valid caches belonging to that source directory.
- `scripts/package-release.ps1` verifies both platform binaries, their build evidence and the validation report before creating a new stage. Official publication requires a channel, increasing manifest sequence, trusted keyring, delegated release key ID, public key, external seed file and expiry.
- Exact application members remain `AWJ.exe`, `AWJ.com`, `LICENSE`, `NOTICE.txt` on Windows, and `AWJ`, `LICENSE`, `NOTICE.txt` on Linux. Validate local archive round trips, permissions, versions and SHA-256.

Local candidates use explicit `-LocalCandidate` / `--local-candidate` modes. `-SkipManifests` is restricted to local candidates; it cannot bypass formal signing or clean-source gates. Deliver into `bin/<VERSION>/` and preserve existing packages and the user-managed `bin/x64/`. When an existing candidate must remain at its original path, `-PreserveVersionCandidates` permits new formal members only and refuses name collisions.

## SDK and corresponding source (1.3.0+)

Use the explicit `windows-x64-msvc`, `linux-x64-clang23` and `hmos-arm64-api26-clang23` profiles for 1.3.1 and later. Only 1.3.0 retains the historical Clang 21 profile. Install only the `AWJimageSDK` component. On each native host, run `scripts/package-sdk-release.cmake` with `TARGET_PROFILE`, `WORK_ROOT`, `INSTALL_DIRECTORY`, a new `OUTPUT_DIRECTORY` and actual `BUILD_EVIDENCE`. Windows/Linux also require a real CTest `TEST_REPORT`; HMOS 1.3.5+ supplies the actual build and link inputs required by the cross-build contract below. Windows provides `MSVC_DIRECTORY` for production compiler identity and `LLVM_DIRECTORY` only for binary inspection.

Build records bind the same clean tag, commit, normalized source fingerprint, real compiler and effective configuration, native codecs, library and applicable report hashes. Windows/Linux packaging checks the local archive round trip and exact C exports, then builds and runs installed C99/C++11/C++17 consumers and the relocated stream tool. Failed, skipped or missing JUnit cases remain errors. HMOS uses its explicit linking-based validation tier; desktop execution records cannot stand in for it.

`scripts/package-source-release.ps1 -OutputDirectory <new-project-build-directory>` archives the formal source commit and checks the version, SDK, build scripts and license members.

Pass all generated `asset-evidence.json` files to the application packager using `-AdditionalAssetEvidencePaths`. Version 1.3.0 requires Windows/Linux SDKs and corresponding source; 1.3.1 and later require Windows/Linux/HMOS SDK roles and corresponding source. Unknown, duplicate or missing roles are rejected. The application archives, three SDK archives, source and SHA256SUMS form seven release assets; application-update member whitelists stay unchanged.

### HMOS cross-build releases from 1.3.5

On 2026-11-11 the user authorized future HMOS releases without on-device validation as a prerequisite. Version 1.3.5 and later, with the exact `hmos-arm64-api26-clang23` profile, automatically use `cross-build` acceptance in the formal SDK entry. No per-version preview exception is needed; Windows/Linux remain `target-verified`.

- Preserve the clean exact tag, real Clang 23/Native 26 inputs and effective Release/IPO configuration, original static runtime, five codecs, seven actual target executables, three archive-extracted cross-linked consumers, exact C exports, full licenses, member whitelist and archive round-trip hashes.
- The role remains `sdk:hmos-arm64-api26`, with `mode=formal`, `status=cross-linked` and `validation_tier=cross-build`. Target execution and loader are explicitly `not-run`. Passed build, link and archive checks do not claim device execution.
- The build entry produces `hmos-build.json`; archive-extracted consumer linking produces `hmos-cross-link-consumers.json`. SDK-BUILD and the external receipt bind these ordinary-file attachments, their hashes and the same source/library. Re-extract and verify the final archive without rewriting earlier reports to match different output.
- Package `VALIDATION.txt` explains the tier in both languages. This path does not require or accept JUnit, runner or loader reports pretending that target execution passed. Source, effective target configuration, Native/resource inputs and the actual compilation/linking process remain traceable.
- The three SDKs and corresponding source retain their four roles. The seven public assets, signing, expiry, sequence, anti-replay and authenticated non-download upload verification remain unchanged.

### Historical HMOS rules before 1.3.5

The following preserves the historical acceptance and per-version exceptions. Its on-device prerequisites and statements that other versions do not inherit a preview describe those earlier versions, not the new policy above.

Formal HMOS acceptance requires actual target SDK tests, three installed consumers and loader evidence bound to the source, library, Native SDK, link map and raw logs. Host tests, ordinary Linux QEMU and cross-linking do not substitute for that gate.

The user separately authorized **1.3.1, 1.3.3 and 1.3.4** to deliver Windows/Linux applications and SDKs formally while providing the HMOS ARM64/API26 SDK as a cross-build preview. These are explicit per-version exceptions, not a future-version default. The independent `scripts/package-hmos-preview.cmake` entry and explicit `-HmosSdkPreview` preserve the clean exact tag, real Clang 23 Release/IPO, original Native inputs, five codecs, seven linked target programs, three archive-extracted cross-linked consumers and export audit. Package notices, receipts and Release notes identify `cross-build-preview`, `target_execution=not-run` and `target_loader=not-run`; no synthetic JUnit or runtime-passed evidence is accepted. All other source, optimization, license, signing and anti-replay gates remain in place.

## Publication and update compatibility

After all reviews and technical gates pass, push the exact source tag and prepare a GitHub Draft Release. Verify the title, channel, complete asset set, source and licenses before publishing. After upload, compare local final hashes with authenticated platform asset names, sizes, states and available digests; do not download uploaded assets or stream their contents for anonymous verification. Unperformed download or anonymous-access checks remain not-run. Publish signed update metadata matching those exact application assets. The version tag remains on the build source commit; later metadata-only commits do not move it.

The development branch is `main`, with historical versions retained as tags. The old `master` compatibility endpoint has been retired; do not recreate dual-branch synchronization. Signing and anti-replay state follow the existing trusted-keyring process on `main`.

See [update security](update-security.en.md) for signing and rotation. Versions 1.0.12 and earlier require a manual upgrade.
