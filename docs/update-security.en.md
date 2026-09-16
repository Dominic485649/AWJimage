# Update signing and key rotation

This is the AWJ 1.0.6+ release procedure. Archives, manifests, keyrings, and temporary release files stay in repository `build/`, `bin/`, or the versioned manifest files at repository root. Private keys never belong in the repository or a release asset.

## Private-key custody

The current legacy Ed25519 seed is kept locally at:

```text
C:\Users\ROG\Documents\AWJimage-secrets\update-ed25519-seed.hex
```

That location is outside the repository. `.gitignore` also ignores `**/*update-ed25519-seed*.hex` and `**/*update-ed25519*.seed`, but ignore rules are not custody: never copy a seed into the repository, `build/`, `bin/`, release assets, logs, CI output, or chat. Keep the legacy, both recovery-root, and every release seed in independent encrypted offline backups. Treat loss or suspected disclosure as a rotation event.

## Trust chain and replay defense

The client verifies raw UTF-8 bytes before parsing JSON. From 1.0.13, the public canonical names are `update-keyring.json` / `update-keyring.json.sig`; packaging also keeps byte-identical `update-keyring-v1.json` / `update-keyring-v1.json.sig` compatibility aliases. At least two different compiled roots out of three must sign the keyring, and its signature envelope contains detached Ed25519 signatures with `key_id`. The client falls back to the legacy pair only when the canonical document or signature returns 404; other network failures, signature/parse failures, and replay-defense failures remain fail-closed. The keyring has its own increasing `sequence`, `issued_at`, and `expires_at`, and delegates release keys with a `key_id`, public key, validity window, and `revoked` flag.

Both v1 and v2 manifests carry signed `key_id`, `issued_at`, and `expires_at`. Their lifetime is capped at 180 days; expired or implausibly future-dated documents are rejected to prevent a valid signature being replayed indefinitely. The client persists each of the v1, v2, and keyring last-verified sequence plus raw SHA-256 in executable-adjacent `.awj-update-security-state.json`, using an inter-process exclusive lock, same-directory temporary write, flush, and atomic replace. Lower sequences, different bytes at the same sequence, and a damaged state file fail closed across restart; legacy `AWJ.jsonc` counters are only migration floors.

No app-local mechanism can resist an attacker with equal local write access that can delete/replace both the application and its state file. Network authority still keeps the URL allow-list, verify-before-parse order, archive/member size and SHA-256 checks, safe extraction, helper re-verification, same-volume staging, health check, and rollback.

## Release and rotation

1. Create or update the raw keyring. Every release key needs a unique lowercase `key_id`; add a new key before use and keep the old key until all manifests it signed expire.
2. Sign the keyring with at least two safe roots. The helper reads the compiled root public keys from CMake and rejects a seed that does not match:

   ```powershell
   .\scripts\sign-update-keyring.ps1 `
     -KeyringPath .\update-keyring.json `
     -RootSeedFiles @{
       'root-legacy-2026' = 'C:\Users\ROG\Documents\AWJimage-secrets\update-ed25519-seed.hex'
       'root-recovery-a-2026' = 'C:\Users\ROG\Documents\AWJimage-secrets\update-ed25519-root-recovery-a-2026.hex'
     } `
     -SignerPath .\bin\x64\Release\awj_update_manifest_sign.exe
   ```

3. Commit and make the signed keyring plus `.sig` publicly downloadable, confirm its sequence is higher than any previously issued value, then sign manifests with the matching release seed. `scripts/package-release.ps1` verifies the keyring envelope with compiled roots before parsing it, requires `-ManifestKeyId` and `-ManifestExpiresAtUtc`, and checks that the ID names a non-revoked keyring key matching `-UpdatePublicKeyHex`. When the existing manifest was signed by the preceding key, pass `-ExistingManifestPublicKeyHex` explicitly.
4. If a release seed is exposed, have two uncompromised roots publish a higher-sequence keyring that marks it `revoked: true`, adds a new release key, and then sign manifests only with the new key. Do not wait for the old manifest to expire.
5. If a root seed is lost or exposed, first use the other two roots to rotate release keys. Deliver a normally signed client with a replacement compiled root set before retiring the old root. Roots are bootstrap trust anchors and cannot be changed by a single release key.

GitHub Immutable Releases is enabled. Before publishing, verify the tag, name, assets, hashes, and prerelease status because immutable releases cannot be amended afterward.
