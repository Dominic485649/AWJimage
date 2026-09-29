# AWJ 发版流程

## 1.1.0 发布

当前 `VERSION` 为 1.1.0。发行说明见 [release-notes-1.1.0.md](release-notes-1.1.0.md)，验证范围见 [validation-1.1.0.md](validation-1.1.0.md)。正式资产必须来自同一干净源码提交；签名 manifest 的 sequence、资产哈希和公开下载均须复核。

1. 本地候选先冻结两平台**代码内容一致**的源码快照，再分别构建 Windows x64 Release 与原生 Linux x64 Release，并完成各平台 CTest、Studio/CLI 冒烟和 CPU 基线检查；正式发布必须改用同一干净提交。Windows 需要 AVX2，Linux 需要 x86-64-v3；两者 Release 必须启用 IPO/LTO。Linux 不在 `/mnt` 下编译或打包。
2. Linux 在原生文件系统中运行 `bash scripts/package-linux-release.sh --binary bin/x64/Release/AWJ --output-dir build/release-linux/1.1.0 --candidate-head <40位提交SHA>`。脚本拒绝脏工作树，检查精确的 `AWJ`、`LICENSE`、`NOTICE.txt` 三文件、7z 完整性、解压哈希、可执行位和 CLI。Windows 端将该包与解压目录复制到仓库 `build/` 作为输入。
3. Windows 使用 `pwsh7` 运行 `scripts/package-release.ps1 -WindowsExePath <AWJ.exe> -WindowsComPath <AWJ.com> -LinuxPackagePath <Linux三文件目录> -LinuxArchivePath <AWJ_Linux.7z> -Version 1.1.0 -SkipManifests`。脚本生成 `build/release/1.1.0/assets/AWJ_Win.7z` 与 `AWJ_Linux.7z`，检查两包精确成员、7z 完整性、解压逐文件 SHA-256 与 Windows CLI。Windows 包严格只含 `AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt`；用户配置、预设、日志、测试程序和签名工具均不入包。
4. 记录两个归档和各成员的实际大小、SHA-256，核对 `AWJimage 1.1.0` 版本、`NOTICE.txt` 构建提交和双平台源码一致。保留本地验证记录；不把旧候选或测试中的用户配置混入发布包。
5. 正式发布时，从经审核的干净 tag 重新构建双平台最终资产，使用仓库外 seed 与有效 keyring、递增 sequence 运行 `package-release.ps1`（不带 `-SkipManifests`）；先审核完整资产、签名和哈希，再创建/发布 GitHub Release。公开下载复核后再提交**同一次打包生成**的签名 manifest。不要重跑打包来单独签名，避免归档哈希变化。更新器真实端到端验证只在本机 Windows 进行。
