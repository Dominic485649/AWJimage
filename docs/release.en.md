# Release guide

## Applications

Build Windows x64 and native Linux x64 Release from the same clean source commit and matching version tag. Preserve AVX2 / x86-64-v3, IPO/LTO, runtime policy and full third-party licenses. Record targeted tests, CLI/UI checks, HDR math, ISO gain maps, sampling and original HDR JXR validation against the actual artifacts, identifying any reused evidence.

- Windows uses `release.ps1 -SkipUpdateManifest`. Existing Clang 23 caches can use `-Generator Ninja -CompilerDirectory <LLVM-directory> -BuildTests` with isolated build, runtime and dependency paths.
- Linux uses `scripts/package-linux-release.sh --binary <native-binary> --build-directory <native-build>`. Build and package on the native Linux filesystem.
- `scripts/package-release.ps1` verifies both platform binaries, their build evidence and the validation report before creating a new stage. Official publication requires a channel, increasing manifest sequence, trusted keyring, delegated release key ID, public key, external seed file and expiry.
- Exact application members remain `AWJ.exe`, `AWJ.com`, `LICENSE`, `NOTICE.txt` on Windows, and `AWJ`, `LICENSE`, `NOTICE.txt` on Linux. Validate archive round trips, permissions, versions and SHA-256.

Local candidates use the explicit `-LocalCandidate` / `--local-candidate` modes. `-SkipManifests` is restricted to local candidates; it cannot bypass formal signing or clean-source gates. Deliver into a new `bin/<VERSION>/` directory and preserve existing packages and the user-managed `bin/x64/`.

## SDK and corresponding source (1.3.0+)

Build the SDK independently with Clang 21 and install only the `AWJimageSDK` component. On each native platform, run `scripts/package-sdk-release.cmake` with `WORK_ROOT`, `INSTALL_DIRECTORY`, a new `OUTPUT_DIRECTORY`, actual `BUILD_EVIDENCE` and CTest `TEST_REPORT`. Windows can specify `LLVM_DIRECTORY` for export inspection.

The build record binds the clean tag, commit, normalized source fingerprint, compiler version, native-codec configuration, actual library hash and JUnit hash. Packaging verifies the archive round trip, links and exact C exports, then builds and runs installed C99/C++11 consumers and the relocated stream tool.

`scripts/package-source-release.ps1 -OutputDirectory <new-project-build-directory>` archives the formal source commit and checks the version, SDK, build scripts and license members.

Pass the three generated `asset-evidence.json` files to the application packager using `-AdditionalAssetEvidencePaths`. Formal 1.3.0+ publication requires both SDK archives and corresponding source. They join the final SHA256SUMS and acceptance receipt while application-update member whitelists stay unchanged.

## Publication and update compatibility

After all gates pass, push the exact source tag and prepare a GitHub Draft Release. Verify the title, channel, complete asset set, uploaded bytes, source and licenses before publishing the immutable Release. Recheck public downloads, then publish signed update metadata matching those exact application assets. The version tag remains on the build source commit; later metadata-only commits do not move it.

Use `main` as the development branch and retain `master` for older clients' `update-archive.json`, `update-keyring.json` and their signatures. Both branches serve identical valid bytes and sequences. GitHub raw-file URLs do not redirect on branch renames; validate both endpoints before switching the default branch and continue maintaining compatibility metadata.

See [update security](update-security.en.md) for signing and rotation. Versions 1.0.12 and earlier require a manual upgrade.
