# Update signing and key rotation

This is the AWJ 1.0.6+ release procedure. Archives, manifests, keyrings, and temporary release files stay in repository `build/`, `bin/`, or the versioned manifest files at repository root. Private keys never belong in the repository or a release asset.

## Private-key custody

Store signing seeds in a protected external directory; supply their paths through environment variables or arguments:

```text
<external-key-directory>\update-ed25519-seed.hex
```

That location is outside the repository. `.gitignore` also ignores `**/*update-ed25519-seed*.hex` and `**/*update-ed25519*.seed`, but ignore rules are not custody: never copy a seed into the repository, `build/`, `bin/`, release assets, logs, CI output, or chat. Keep the legacy, both recovery-root, and every release seed in independent encrypted offline backups. Treat loss or suspected disclosure as a rotation event.

## Trust chain and replay protection

The only network endpoints are `update-keyring.json` / `.sig` and `update-archive.json` / `.sig`. Verify the original UTF-8 bytes before JSON parsing. Missing files, network errors, invalid signatures, parsing errors and replay failures reject the update. Version 1.2.1 retires legacy endpoints; 1.0.12 and earlier require a manual upgrade. Existing transaction recovery remains supported.

At least two of the three compiled root keys sign a keyring that delegates time-bounded, revocable release keys. Both documents carry increasing sequences and validity windows of at most 180 days. Expired and implausibly future documents are rejected.

The application stores accepted sequence and raw SHA-256 values in `.awj-update-security-state.json`, using a cross-process lock, flushed temporary file and atomic replacement. Lower sequences, different content at the same sequence and damaged state fail closed. Retain old-state migration, URL allowlists, member hashes, safe extraction, helper verification, health checks and rollback. This cannot protect against a local attacker who can replace application files and replay state.

## Release and rotation

1. Create or update the raw keyring. Every release key needs a unique lowercase `key_id`; add a new key before use and keep the old key until all manifests it signed expire.
2. Sign the keyring with at least two safe roots. The helper reads the compiled root public keys from CMake and rejects a seed that does not match:

   ```powershell
   .\scripts\sign-update-keyring.ps1 `
     -KeyringPath .\update-keyring.json `
     -RootSeedFiles @{
       'root-legacy-2026' = '<external-key-directory>\update-ed25519-seed.hex'
       'root-recovery-a-2026' = '<external-key-directory>\update-ed25519-root-recovery-a-2026.hex'
     } `
     -SignerPath .\bin\x64\Release\awj_update_manifest_sign.exe
   ```

3. Commit and make the signed keyring plus `.sig` publicly downloadable, confirm its sequence is higher than any previously issued value, then sign manifests with the matching release seed. `scripts/package-release.ps1` verifies the keyring envelope with compiled roots before parsing it, requires `-ManifestKeyId` and `-ManifestExpiresAtUtc`, and checks that the ID names a non-revoked keyring key matching `-UpdatePublicKeyHex`. When the existing manifest was signed by the preceding key, pass `-ExistingManifestPublicKeyHex` explicitly.
4. If a release seed is exposed, have two uncompromised roots publish a higher-sequence keyring that marks it `revoked: true`, adds a new release key, and then sign manifests only with the new key. Do not wait for the old manifest to expire.
5. If a root seed is lost or exposed, first use the other two roots to rotate release keys. Deliver a normally signed client with a replacement compiled root set before retiring the old root. Roots are bootstrap trust anchors and cannot be changed by a single release key.

GitHub Immutable Releases is enabled. Before publishing, verify the tag, name, assets, hashes, and prerelease status because immutable releases cannot be amended afterward.
