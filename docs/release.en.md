# Release guide

1. Build Windows x64 and native Linux x64 Release from the same clean source commit. Run regressions, CLI/UI checks, CPU baseline and dependency checks. Preserve AVX2 / x86-64-v3, IPO/LTO and third-party licenses.
2. Windows: `release.ps1 -SkipUpdateManifest`. Linux: `scripts/package-linux-release.sh --binary bin/x64/Release/AWJ`. Official Linux packages require the matching version tag; local candidates use `--candidate-head <commit>`. Build and package on the native Linux filesystem.
3. Run `scripts/package-release.ps1` in PowerShell 7 with both platform binaries, the Linux package and archive. Use `-SkipManifests` for review candidates. Official releases require a channel, increasing `ArchiveManifestSequence`, keyring, release key ID, public key, an external seed file and an expiry date.
4. Exact members: Windows contains `AWJ.exe`, `AWJ.com`, `LICENSE`, `NOTICE.txt`; Linux contains `AWJ`, `LICENSE`, `NOTICE.txt`. Use LZMA2 and verify archive integrity, members, executable permissions, versions and SHA-256.
5. Publish the tag and GitHub Release after review. Verify public downloads, then commit `update-archive.json`, `update-keyring.json` and their signatures matching the uploaded assets. Keep review candidates out of official update documents.

See [update security](update-security.en.md) for signing and rotation. Legacy endpoints are retired in 1.2.1; versions 1.0.12 and earlier require a manual upgrade.
