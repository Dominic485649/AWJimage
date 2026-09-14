# AWJ 1.0.4–1.0.9 发版

所有构建、暂存、归档与旧版测试样本必须位于仓库 `build/` 或 `bin/`。不在 `D:/` 根目录创建任何文件。Windows 的 1.0.3 和 1.0.6 测试样本分别只保留在 `bin/old-versions/1.0.3/` 与 `bin/old-versions/1.0.6/`；除非另行指定，不保留其他后续旧版。

1.0.4/1.0.5 的冻结发布必须检出 `1.0.4` tag 后使用其随 tag 固化的脚本；不要将 1.0.6 的 keyring/过期策略回写到这两个桥接发布。

## 1.0.4 prerelease

1. 在 `codex/awj-1.0.4` 上完成 Windows Release CTest、Studio/CLI smoke，以及普通 WSL Linux Release 构建、CTest、CLI、ELF 和归档检查。不要在 WSL 或虚拟机执行自动更新端到端测试。
2. 直接合并到 `master`，从干净的 1.0.4 提交打 `1.0.4` tag；Windows 和 Linux 二进制必须来自该 tag 的锁定依赖。
3. 用仓库外 Ed25519 seed 编译签名工具和客户端公钥，然后只运行一次打包脚本，生成、测试归档和待发布的签名 manifest：

   ```powershell
   .\scripts\package-release.ps1 `
     -WindowsExePath .\bin\x64\Release\AWJ.exe `
     -WindowsComPath .\bin\x64\Release\AWJ.com `
     -LinuxBinaryPath .\bin\x64\Release\AWJ `
     -ArchiveManifestSequence 1 `
     -PublishedAtUtc 'YYYY-MM-DDTHH:MM:SSZ' `
     -UpdatePublicKeyHex $env:AWJ_UPDATE_PUBLIC_KEY_HEX `
     -UpdateSigningSeedFile $env:AWJ_UPDATE_SIGNING_SEED_FILE
   ```

   该步骤仅写入 `build/release/1.0.4/`，执行 `7z t`、全新解压、逐文件 SHA-256 和 Windows CLI `--help`。归档包含二进制、校验文件、`LICENSE`、`THIRD_PARTY_NOTICES.txt`、`THIRD_PARTY_LICENSES/` 和 `BUILD_INFO.txt`。Linux `AWJ --help` 与 `readelf -d` 在 WSL 普通验证中执行。
4. 先创建 draft prerelease，再只上传该次打包产生的 `build/release/1.0.4/assets/AWJ_Win.7z` 与 `AWJ_Linux.7z`；确认资产完整后发布。Immutable Release 发布后不能改动资产或 tag。先核对公开下载、大小、哈希和 Release prerelease 状态；不要为签 manifest 再运行一次打包，以免归档成员时间变化导致哈希失配。
5. 资产可下载后，只提交步骤 3 已生成的 v2 manifest：

   ```powershell
   git add update-manifest-v2.json update-manifest-v2.json.sig
   git commit -m 'release: add 1.0.4 archive update manifest'
   git push origin master
   ```

   `update-manifest-v2.json` 单独维护 sequence，只含归档资产。每个成员都绑定归档 URL、大小和 SHA-256；客户端不回退到裸资产。

## 1.0.5 bridge prerelease

从 1.0.4 的锁定源码创建 1.0.5，只改版本、更新日志和发布说明。完成同一组构建和归档验证后，发布四个自定义资产：`AWJ_Win.7z`、`AWJ_Linux.7z`、`AWJ.exe`、`AWJ.com`。Release 正文必须明确：功能与 1.0.4 相同，仅用于 1.0.3 自动更新桥接测试；普通用户应下载 1.0.4。

在上传 1.0.5 的 Immutable Release 前，使用 `-BridgeRelease` 只运行一次打包脚本以同时生成资产和 v1 manifest；先创建 draft、上传同一次生成的四个资产、确认完整后发布，资产公开可下载时仅提交已生成的 v1 manifest：

```powershell
.\scripts\package-release.ps1 `
  -WindowsExePath .\bin\x64\Release\AWJ.exe `
  -WindowsComPath .\bin\x64\Release\AWJ.com `
  -LinuxBinaryPath .\bin\x64\Release\AWJ `
  -BridgeRelease `
  -LegacyManifestSequence 8 `
  -PublishedAtUtc 'YYYY-MM-DDTHH:MM:SSZ' `
  -UpdatePublicKeyHex $env:AWJ_UPDATE_PUBLIC_KEY_HEX `
  -UpdateSigningSeedFile $env:AWJ_UPDATE_SIGNING_SEED_FILE
```

它不会把 1.0.5 添加为 v2 候选。Windows 本机从 `bin/old-versions/1.0.3/` 的隔离副本执行 1.0.3→1.0.5 下载、校验、安装、健康检查和回滚测试；通过后停止。1.0.6 已获得单独授权，按下节执行，仍不在 VM 或 WSL 中执行更新端到端测试。

## 1.0.6 security prerelease

1. 先完成 1.0.4/1.0.5 冻结资产、公开下载和本机桥接测试；1.0.6 不得改写两者 tag 或资产。
2. seed 固定保存在仓库外 `C:\Users\ROG\Documents\AWJimage-secrets\update-ed25519-seed.hex`。不要读取、打印、提交或复制 seed；legacy、recovery root 和 release seed 都要有独立离线加密备份。
3. 用两把 root 签名 `update-keyring-v1.json`，确认 `sequence` 严格递增、`expires_at` 不超过 180 天，并在公开更新 manifest 之前提交其 `.sig`。详见 [自动更新签名与密钥轮换](update-security.md)。
4. 从干净的 1.0.6 tag 先在原生 Linux 工作树生成 Linux 归档；不得在 `/mnt/...` 编译或打包 Linux 资产：

   ```bash
   bash scripts/package-linux-release.sh \
     --binary bin/x64/Release/AWJ \
     --output-dir build/release-linux/1.0.6
   ```

   该脚本会 `7z t`、全新解压、核对全部文件哈希并启动解压后的 `AWJ --help`，因此可执行位是发布门槛。再在 Windows 端只运行一次 `scripts/package-release.ps1`，传入原生目录的 `-LinuxPackagePath` 和 `-LinuxArchivePath`（例如 `\\wsl.localhost\<发行版>\home\...`），以及 `-ManifestKeyId`、`-ManifestExpiresAtUtc`、匹配的 release 公钥/seed 和旧 manifest 公钥（如不同）。Windows 脚本生成 `AWJ_Win.7z`，重新解压并逐文件复核原生 `AWJ_Linux.7z`，再生成待发布的签名 v2 manifest。v1 仍只保留给 1.0.5 桥接。上传该次生成的资产；公开下载验证后只提交已生成的 manifest，绝不为签 manifest 重跑打包。脚本会拒绝未撤销 keyring key 以外的签名者。
5. GitHub Immutable Releases 已启用。必须先创建 draft、上传并核对完整资产后才发布；发布后核对 tag、prerelease、资产名、大小和哈希。不可用“上传后修正”替代发布前验证。

## 1.0.7 归档自动更新测试 prerelease

1. 从锁定的 1.0.6 源码创建 1.0.7，只改版本、更新日志和发布说明；功能代码与 1.0.6 必须一致。
2. 从干净的 1.0.7 tag 在 Windows 构建 Windows 资产、在原生 Linux 工作树构建和打包 Linux 资产；不得在 `/mnt/...` 跨系统编译或打包。
3. 以严格递增的 v2 sequence 发布且只上传 `AWJ_Win.7z`、`AWJ_Linux.7z`，公开下载、哈希和签名验证后只提交该次生成的 v2 manifest。不要上传裸 `AWJ.exe` 或 `AWJ.com`。
4. Release 正文必须明确：功能与 1.0.6 相同，仅用于 1.0.6→1.0.7 自动更新测试；普通用户继续下载 1.0.6。需要本机测试时只从 `bin/old-versions/1.0.6/` 的隔离副本执行，不在 VM 或 WSL 中执行更新端到端测试。

## 1.0.9 更新器/发行包修复 prerelease

1. Windows 公共发行归档 `AWJ_Win.7z` 的结构固定且必须精确为以下六个顶层文件，不允许任何子目录或额外成员：

   ```text
   AWJ.exe
   AWJ.com
   AWJ.ShellExtension.dll
   AWJ.ContextMenu.msix
   LICENSE
   NOTICE.txt
   ```

   `AWJ.ContextMenu.msix` 是 Windows 11 资源管理器现代右键菜单所需的 sparse package；它不包含第二份程序或 DLL，只提供 package identity 并引用同目录的外部安装内容。发布前必须使用目标用户计算机信任的证书签名 MSIX。`NOTICE.txt` 合并构建信息、`THIRD_PARTY_NOTICES.txt` 的第三方 notices，以及 `licenses/` 下发行所需的完整第三方许可证文本。包内不再放置 `.sha256`、`BUILD_INFO.txt`、`THIRD_PARTY_NOTICES.txt` 或 `THIRD_PARTY_LICENSES/`；归档本身及每个成员的大小和 SHA-256 仍由签名 v2 manifest 绑定，因此不依赖包内 checksum sidecar。
2. `AWJ_Linux.7z` 同步固定为扁平的 `AWJ`、`LICENSE`、`NOTICE.txt` 三文件结构。`scripts/package-linux-release.sh` 和 `scripts/package-release.ps1` 都必须先精确校验成员集合，再执行 7z 完整性、全新解压、逐文件哈希和 smoke test。
3. 为兼容 1.0.4–1.0.8 旧归档，更新器允许并跳过“由签名文件路径必然隐含”的安全目录 entry；目录本身不加入 manifest。任何额外目录、路径穿越、链接、特殊成员、重复/大小写冲突成员或未签名文件仍 fail-closed。
4. 1.0.9 必须从干净的 `1.0.9` tag 构建 Windows 与原生 Linux 资产，使用严格递增的 v2 sequence 和仓库外 release seed 签名；GitHub Release 只上传 `AWJ_Win.7z` 与 `AWJ_Linux.7z` 并标记 prerelease。公开下载验证后再提交该次已经生成的 `update-manifest-v2.json` 与签名，不为重新签名而重跑打包。
5. Windows 本机从 `bin/old-versions/1.0.6/` 的隔离副本执行 1.0.6→1.0.9 更新端到端验证；同时保留目录 entry 单元回归，确保旧 1.0.8 归档的结构不会再次触发误判。
