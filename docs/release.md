# 发布与本地版本交付

## 用户审查与工作顺序

项目规范以 [CLAUDE.md](../CLAUDE.md) 为准。每个新版本默认先完成开启优化的 MSVC Windows 软件构建，交付 `bin/<VERSION>/` 供用户实际审查；用户明确确认可用后，再准备本版本 Linux 产物、相关 SDK 和 Release 文案。用户不负责 Linux 软件审查，但正式发布仍须提供该版本要求的 Linux 产物。

发布需要两个独立的用户确认：**Windows 软件可用**，以及**明确同意本次 Release 文案**。实施计划批准、自动验收或过去版本的批准不能代替；文案批准且技术门禁全部通过后，才正式发布并更新签名入口。

上述构建顺序可调整：为验证 Linux 能否正确使用，可以提前构建；SDK 专项更新可以提前构建并由 AI 验收 SDK，必要的跨平台验证和文案准备也可前置，不必等待 Windows 审查。调整顺序不自动取消两次用户审查或现有技术门禁。

## 版本目录约定

- 新版本只投放到 `bin/<VERSION>/`，按 `Windows/`、`Linux/` 放置扁平平台包，并附 `AWJ_Win.7z`、`AWJ_Linux.7z` 和 `SHA256SUMS`。
- `bin/x64/` 由用户手动管理。发行、验证和清理工具都必须拒绝把它作为写入、移动或删除目标。
- 构建／测试中间产物在项目 `build/` 的独立工作目录，通过 `AWJ_RUNTIME_OUTPUT_ROOT` 显式隔离。已有有效编译缓存和依赖可以复用，不要求每次冷构建。
- 平台归档只收固定成员。Windows 为 `AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt`；Linux 为 `AWJ`、`LICENSE`、`NOTICE.txt`。本地验证、用户数据备份和签名工具不进入归档。

## 本地候选

1. Windows 使用 `release.ps1 -LocalCandidate -SkipUpdateManifest`，可指定本项目内的独立 build、runtime、vcpkg installed 和源缓存路径。脚本记录源码指纹，并在编译前后核对源码未变化。
2. Linux 必须在原生文件系统编译和打包。现有 WSL 工作区可就地增量修复，稳定后统一源码差异；不通过 Windows 共享路径或 `/mnt` 编译。
3. Linux 打包器使用 `--local-candidate --build-directory <原生构建目录>`，保持原有 Clang 23／libc++／Release／IPO／x86-64-v3，再做一次增量构建并绑定二进制 SHA256 与前后源码指纹。记录真实 HEAD 与 dirty 状态；旧 `--candidate-head` 仍要求干净源码，不将其误用作 dirty 工作区许可。
4. Windows 审查候选可以先单独交付，不要求为用户审查提前构建 Linux。组装完整跨平台候选时，两平台必须具有相同源码指纹、正确版本和有效 patch 锁；完成对应的两平台测试、HDR 数学、ISO 协议、采样、原生 UI 和原始 JXR 验收后，使用 `scripts/package-release.ps1 -LocalCandidate -VerificationReportPath <验收报告>` 验证报告及两平台构建证据，再创建全新 staging。
5. 校验版本、许可、成员白名单、归档往返 SHA256、Linux 可执行位和运行自检，通过后投放 `bin/<VERSION>/`。拒绝先删除输入或覆盖未知的已存在版本目录。
6. 对投放后的真实产物完成自动化验收，保存 `verification/acceptance.json`。未通过时保留旧版本，不把构建或压缩退出成功当作全部验收。

本地候选不推送、不自动打 tag、不公开发布，不签或改写正式更新清单。源码指纹算法为 `sha256/git-normalized-source-v1`，只覆盖源码白名单，不读取或哈希私钥、运行配置、bin/build 或本地素材。

## 旧版本与无效缓存

每次新版本验收后，先生成项目限定的清理清单，再备份旧版本用户配置、预设和防回放状态，逐文件校验。备份按来源版本分开，不自动激活或合并旧安全状态，不降低 sequence／revocation floor。

只将可确认属于本项目、可重建、无当前引用的缓存列为候选。根目录、`bin/x64/`、仓库外路径、链接越界、活跃进程引用、dirty 工作副本、有效工具链／依赖、原始 artwork、样本及未知素材均受保护。文件夹名字含旧版本不等于缓存失效。

删除动作与清单／备份分离。实际清理必须同时通过路径和归属重检，并获得执行环境允许。权限拦截时只生成清单与备份，报告未删除项目，不能绕过或声称清理成功。

## 正式发布

正式 Windows／Linux 从同一干净源码提交构建，保留 AVX2／x86-64-v3、IPO/LTO、既定运行库和完整许可证。Linux 正式包要求匹配版本 tag；本地候选不能通过 `SkipManifests` 混入正式入口。

正式 `scripts/package-release.ps1` 仍需渠道、递增 manifest sequence、受信密钥环、发布委派 key ID、公钥、仓库外 seed 与到期时间。验证签名及归档后，才由明确授权的发布步骤创建 tag／GitHub Release，复核公开下载，再提交与实际资产匹配的更新清单及签名。

Windows 应用与 SDK 均使用真正的 MSVC `cl.exe`，不以 clang-cl 或 MSVC ABI 兼容替代。应用入口 `release.ps1 -Generator Ninja -BuildTests -SkipUpdateManifest` 自动定位 MSVC；默认 VS 2026 生成器同样保留。SDK 使用 `scripts/build-sdk.ps1`。两者启用完整 Release 优化、`/GL` 和最终 `/LTCG`；应用保持 AVX2，SDK 保留 x64 部署基线。更换编译器须隔离旧 Clang 的 BMI/对象/构建树；只复用身份和编译条件匹配的源码及依赖。正式入口继续要求干净 tag、静态运行库与实际 MSVC/优化证据。

### SDK 与对应源码（1.3.0 起）

- 1.3.1 使用严格独立 profile：Windows SDK 为 `windows-x64-msvc`，Linux 为 `linux-x64-clang23`，HMOS 为 `hmos-arm64-api26-clang23`。各平台在原生 host 运行 `scripts/package-sdk-release.cmake`，显式提供 `TARGET_PROFILE`，并传入 `WORK_ROOT`、`INSTALL_DIRECTORY`、全新的 `OUTPUT_DIRECTORY`、实际 `BUILD_EVIDENCE` 和 CTest `TEST_REPORT`。Windows 的 `MSVC_DIRECTORY` 指定可信 cl.exe/link.exe 目录；`LLVM_DIRECTORY` 在 Windows 只提供 LLVM23 二进制检查工具，在 Linux/HMOS 同时提供 Clang23 编译器。仅 1.3.0 保留旧 Clang21 兼容 profile。
- 构建记录必须包含同一干净 tag 的提交、源码指纹、编译器版本、完整 native codecs 状态、实际库 SHA-256 与对应 JUnit SHA-256。打包器在归档回解位置核对 C 导出，构建 C99/C++11/C++17 消费者并按目标执行，执行迁移后的 `awj-stream`，核对成员、符号链接与内容摘要。
- `scripts/package-source-release.ps1 -OutputDirectory <本项目build下的新目录>` 从正式提交生成对应源码归档，并校验版本及 SDK、构建脚本和许可成员。
- 将 SDK 与源码的 `asset-evidence.json` 一起传入正式应用打包器的 `-AdditionalAssetEvidencePaths`。1.3.0 固定双平台 SDK 与源码三个角色；1.3.1 起固定 Windows/Linux/HMOS 三份 SDK 与源码四个角色，未知、重复、缺失及大小写变体均拒绝。统一生成七项资产、SHA256SUMS 与 acceptance.json，应用更新包的成员白名单不变。
- HMOS 正式打包另提供 `TARGET_CMAKE_TOOLCHAIN_FILE`、`TARGET_NATIVE_SDK_DIRECTORY`、`TARGET_LINK_MAP`，在进程环境设置匹配的 `AWJ_OHOS_NATIVE_ROOT`、`AWJ_OHOS_RESOURCE_DIR`、`AWJ_OHOS_LLVM_ROOT`；仅从可信 CLI 路径调用 compiler/runner。`TARGET_RUNNER` 与 `TARGET_EXECUTION_EVIDENCE` 二选一，证据必须含真实目标 SDK 测试、三个安装消费者及 loader 原始记录，不接受宿主通过或单独的 passed 字段。
- HMOS 使用无版本实际文件 `libawj_sdk.so`，仅 CPU C 头及完整五编码器；按能力检查，不强求桌面 SHM/stream。所有源、库、工具链、Native 静态运行库、link map 与目标报告均绑定摘要。缺少目标运行门禁时完成其他本地版本化交付，但不发布承诺三平台支持的正式版本。

### main 与旧版更新入口

开发主线为 `main`。保留 `master` 的 `update-archive.json`、`update-keyring.json` 及各自签名作为旧客户端兼容入口，两处分发相同有效字节和 sequence。GitHub 的 raw 文件 URL 不会因分支改名重定向，因此应在验证旧、新入口后切换默认主线，并持续维护兼容元数据。

签名和轮换见 [更新安全](update-security.md)。候选不得写入正式清单，不静默轮换信任根；1.0.12 及更早版本需手动升级。
