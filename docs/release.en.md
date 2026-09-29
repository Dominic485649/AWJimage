# AWJ release procedure

## 1.1.0 release

The stable release is published: [release notes](release-notes-1.1.0.md), [validation record](validation-1.1.0.md), [GitHub Release](https://github.com/Dominic485649/AWJimage/releases/tag/1.1.0).

1. Build Windows x64 and native Linux x64 Release from the same clean tag. Run platform CTest, Studio/CLI smoke tests, and CPU baseline checks. Windows requires AVX2; Linux requires x86-64-v3; both enable IPO/LTO. Do not build or package Linux under `/mnt`.
2. On Linux run `bash scripts/package-linux-release.sh --binary bin/x64/Release/AWJ --output-dir build/release-linux/1.1.0`, then copy its archive and three-file package into the Windows repository `build/` directory. Omit `--candidate-head` for a release: the script requires HEAD to match the tag.
3. With `pwsh7`, run `scripts/package-release.ps1` with both platform binaries and the Linux package, `-Channel stable`, an increasing `-ArchiveManifestSequence`, an external signing seed, valid keyring, public key, key ID, and manifest expiry. Omit `-SkipManifests` for a release. The script checks fixed members, archive round trips, versions, hashes, and signatures. The Windows archive contains only `AWJ.exe`, `AWJ.com`, `LICENSE`, `NOTICE.txt`; Linux contains only `AWJ`, `LICENSE`, `NOTICE.txt`.
4. Verify the tag, source commits, archive hashes, and member hashes before uploading and publishing the GitHub Release. Check the public downloads before committing the signed manifest from **the same packaging run**; do not repackage and reuse old hashes. Run real updater installation end-to-end only on local Windows.
