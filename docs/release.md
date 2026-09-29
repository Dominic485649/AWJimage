# AWJ 发版流程

## 1.1.0 发布

正式版已发布：[发行说明](release-notes-1.1.0.md)、[验证记录](validation-1.1.0.md)、[GitHub Release](https://github.com/Dominic485649/AWJimage/releases/tag/1.1.0)。

1. 从同一干净 tag 构建 Windows x64 与原生 Linux x64 Release，完成各平台 CTest、Studio/CLI 冒烟及 CPU 基线检查。Windows 最低 AVX2，Linux 最低 x86-64-v3；两者均启用 IPO/LTO。Linux 不在 `/mnt` 下编译或打包。
2. Linux 运行 `bash scripts/package-linux-release.sh --binary bin/x64/Release/AWJ --output-dir build/release-linux/1.1.0`，然后将归档和三文件包复制到 Windows 仓库 `build/`。正式版不带 `--candidate-head`；脚本要求 HEAD 为对应 tag。
3. Windows 用 `pwsh7` 运行 `scripts/package-release.ps1`，指定两平台二进制与 Linux 包、`-Channel stable`、递增的 `-ArchiveManifestSequence`、仓库外的签名 seed、有效 keyring、公钥、密钥 ID 及 manifest 到期时间。正式版不带 `-SkipManifests`。脚本检查固定成员、归档往返、版本、哈希与签名；Windows 包仅含 `AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt`，Linux 包仅含 `AWJ`、`LICENSE`、`NOTICE.txt`。
4. 核对 tag、双平台源码提交、归档和成员哈希，再上传并发布 GitHub Release。公开下载逐项复核后，提交**同一次打包生成**的签名清单；不要重新打包后沿用旧哈希。真实更新安装端到端验证只在本机 Windows 进行。
