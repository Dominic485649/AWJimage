# 发布指南

1. 从同一干净源码提交构建 Windows x64 与原生 Linux x64 Release。完成回归、CLI/UI 检查、CPU 基线和依赖检查；保留 AVX2 / x86-64-v3、IPO/LTO 和第三方许可证。
2. Windows 运行 `release.ps1 -SkipUpdateManifest`。Linux 运行 `scripts/package-linux-release.sh --binary bin/x64/Release/AWJ`；正式包要求 HEAD 为匹配版本 tag，本地候选使用 `--candidate-head <commit>`。Linux 在原生文件系统构建和打包。
3. 使用 PowerShell 7 的 `scripts/package-release.ps1`，指定两平台程序、Linux 包和归档。审查候选时使用 `-SkipManifests`；正式发布时提供渠道、递增 `ArchiveManifestSequence`、密钥环、发布密钥 ID、公钥、仓库外 seed 和到期时间。
4. 固定成员：Windows 为 `AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt`；Linux 为 `AWJ`、`LICENSE`、`NOTICE.txt`。归档使用 LZMA2，验证完整性、成员、可执行位、版本和 SHA-256。
5. 审查通过后发布 tag 和 GitHub Release。复核公开下载，再提交与实际资产完全一致的 `update-archive.json`、`update-keyring.json` 及各自签名。候选不得写入正式更新清单。

签名与轮换流程见 [更新安全](update-security.md)。1.2.1 起停止生成旧更新入口，1.0.12 及更早版本需手动升级。
