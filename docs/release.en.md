# AWJ 1.0.4–1.0.9 release

Keep every build, staging directory, archive, and old-version sample under repository `build/` or `bin/`; never create release files at the `D:/` root. Keep the Windows 1.0.3 and 1.0.6 samples only in `bin/old-versions/1.0.3/` and `bin/old-versions/1.0.6/`; retain no other later old version unless explicitly requested.

Frozen 1.0.4/1.0.5 publication must check out the `1.0.4` tag and use the scripts frozen with that tag; do not backport the 1.0.6 keyring/expiry policy into either bridge release.

## 1.0.4 prerelease

Run Windows Release CTest plus Studio/CLI smoke, then ordinary WSL Linux Release build, CTest, CLI, ELF, and archive checks. Do not run updater end-to-end tests in WSL or a VM. Merge directly to `master`, tag a clean 1.0.4 commit, and build both platforms from that tag's locked dependencies.

Run `scripts/package-release.ps1` exactly once with the three platform binary paths, `-ArchiveManifestSequence`, UTC time, and the external Ed25519 seed/public key. It writes only `build/release/1.0.4/`, runs `7z t`, extracts into a new directory, verifies every file SHA-256, runs Windows CLI `--help`, and stages the signed v2 manifest. Archives contain the binary, checksum file, `LICENSE`, `THIRD_PARTY_NOTICES.txt`, `THIRD_PARTY_LICENSES/`, and `BUILD_INFO.txt`; run Linux `AWJ --help` and `readelf -d` in the normal WSL validation. Create a draft prerelease, upload exactly the resulting `AWJ_Win.7z` and `AWJ_Linux.7z`, verify it is complete, then publish it. Immutable Releases prevent later asset or tag changes. Verify public downloads, then commit and push only the already-generated signed `update-manifest-v2.json` and `.sig`. Do not rerun packaging solely to sign the manifest: archive member timestamps could otherwise change its hash.

The v2 manifest has its own sequence and only archive assets. Each required member binds to the archive URL, size, and SHA-256; clients do not fall back to raw assets.

## 1.0.5 bridge prerelease

Create 1.0.5 from the locked 1.0.4 source, changing only version, changelog, and release notes. Validate the same build and archives, then publish exactly `AWJ_Win.7z`, `AWJ_Linux.7z`, `AWJ.exe`, and `AWJ.com`. Its notes must state that functionality is identical to 1.0.4, it exists only for the 1.0.3 updater bridge test, and normal users should download 1.0.4.

Before creating the immutable 1.0.5 Release, run the packager once with `-BridgeRelease` and a strictly higher `-LegacyManifestSequence`, create a draft, upload the four assets it produced, verify it is complete, then publish. After public-download validation, commit only the already-generated legacy v1 1.0.5 entry; it is not a v2 candidate. On Windows, use an isolated copy of `bin/old-versions/1.0.3/` to validate the local 1.0.3→1.0.5 download, verification, install, health check, and rollback. Stop after it passes. 1.0.6 has separate authorization and follows the next section; updater E2E remains prohibited in a VM or WSL.

## 1.0.6 security prerelease

1. Finish frozen 1.0.4/1.0.5 assets, public-download validation, and the local bridge test first; 1.0.6 must not rewrite either tag or asset.
2. Keep the seed outside the repository at `C:\Users\ROG\Documents\AWJimage-secrets\update-ed25519-seed.hex`. Do not read, print, commit, or copy it; legacy, recovery-root, and release seeds all need independent encrypted offline backups.
3. Have two roots sign `update-keyring-v1.json`, confirm its sequence increases strictly and its `expires_at` is at most 180 days out, and commit the `.sig` before publishing an update manifest. See [update signing and key rotation](update-security.en.md).
4. From a clean 1.0.6 tag, first create the Linux archive in a native Linux worktree; never compile or package a Linux asset under `/mnt/...`:

   ```bash
   bash scripts/package-linux-release.sh \
     --binary bin/x64/Release/AWJ \
     --output-dir build/release-linux/1.0.6
   ```

   The script runs `7z t`, freshly extracts the archive, verifies every file hash, and launches extracted `AWJ --help`, so the executable bit is a release gate. Then run `scripts/package-release.ps1` exactly once on Windows with the native `-LinuxPackagePath` and `-LinuxArchivePath` (for example `\\wsl.localhost\<distro>\home\...`), explicit `-ManifestKeyId`, `-ManifestExpiresAtUtc`, the matching release public key/seed, and the preceding manifest public key when it differs. Windows creates `AWJ_Win.7z`, freshly extracts and hash-checks the native `AWJ_Linux.7z`, then stages the signed v2 manifest. v1 remains only for the 1.0.5 bridge. Upload the resulting assets; after public-download validation, commit only that already-generated manifest rather than rerunning packaging. The script rejects a signer outside the non-revoked keyring entry.
5. GitHub Immutable Releases is enabled. Create a draft, attach and verify the complete asset set, then publish; verify tag, prerelease state, asset names, sizes, and hashes afterward. Publishing first and correcting later is not an option.

## 1.0.7 archive update-test prerelease

1. Create 1.0.7 from locked 1.0.6 source, changing only version, changelog, and release notes; its functional code must remain identical to 1.0.6.
2. Build Windows assets on Windows and build/package Linux assets in a native Linux worktree from a clean 1.0.7 tag. Never compile or package across systems under `/mnt/...`.
3. Publish a strictly higher v2 sequence with exactly `AWJ_Win.7z` and `AWJ_Linux.7z`; after public-download, hash, and signature validation, commit only the generated v2 manifest. Do not upload raw `AWJ.exe` or `AWJ.com`.
4. The release notes must say that it is functionally identical to 1.0.6 and exists only for the 1.0.6→1.0.7 update test; normal users should continue to download 1.0.6. If a local test is run, use only an isolated copy from `bin/old-versions/1.0.6/`, never a VM or WSL updater E2E.

## 1.0.9 updater/package-fix prerelease

The public Windows archive `AWJ_Win.7z` has a fixed, exact, flat six-file contract with no subdirectories or extra members:

   ```text
   AWJ.exe
   AWJ.com
   AWJ.ShellExtension.dll
   AWJ.ContextMenu.msix
   LICENSE
   NOTICE.txt
   ```

   `AWJ.ContextMenu.msix` is the sparse package required by the Windows 11 File Explorer modern context menu. It contains no second copy of the application or DLL; it supplies package identity and points at the external installation directory. The MSIX must be signed with a certificate trusted by target machines before publication. `NOTICE.txt` consolidates build information, the third-party notices from `THIRD_PARTY_NOTICES.txt`, and the full third-party license texts required for distribution from `licenses/`. Do not place `.sha256` sidecars, `BUILD_INFO.txt`, `THIRD_PARTY_NOTICES.txt`, or `THIRD_PARTY_LICENSES/` inside the archive. The signed v2 manifest still binds the archive and each member by size and SHA-256, so in-package checksum sidecars are unnecessary.
2. `AWJ_Linux.7z` is likewise fixed to the flat three-file set `AWJ`, `LICENSE`, and `NOTICE.txt`. Both `scripts/package-linux-release.sh` and `scripts/package-release.ps1` must verify the exact member set before 7z integrity, fresh extraction, per-file hashing, and smoke tests.
3. For backward compatibility with 1.0.4–1.0.8 archives, the updater may skip only safe directory entries necessarily implied by signed file-member paths; directories themselves are not manifest members. Extra directories, traversal, links, special entries, duplicate/case-colliding members, and unsigned files remain fail-closed.
4. Build 1.0.9 Windows and native-Linux assets from a clean `1.0.9` tag, sign a strictly increasing v2 sequence with the external release seed, and upload only `AWJ_Win.7z` and `AWJ_Linux.7z` to a prerelease. Commit the already-generated `update-manifest-v2.json` and signature only after public-download verification; never rerun packaging merely to re-sign.
5. On native Windows, use an isolated copy of `bin/old-versions/1.0.6/` for the 1.0.6→1.0.9 updater E2E test, and retain the directory-entry regression test so the old 1.0.8 archive layout cannot regress again.
