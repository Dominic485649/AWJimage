# AWJimage 1.3.0 — 内存管道与 C ABI SDK

**新增 Windows / Linux C ABI SDK**，支持其他软件直接提交图像帧，在内存中完成 PNG、JPEGli、WebP、AVIF、JPEG XL 编码。同时改进队列重复导入体验，统一 SDR / HDR 与 Gain Map 输出策略。

## 图像编码 SDK

- **统一 C ABI**：C / C++ 项目通过同一套 C 头接入。
- **内存帧处理**：支持 RGB / RGBA、浮点 HDR 与 YUV，提供只读借用、资源生命周期管理和复制量诊断，编码结果可直接返回内存。
- **管道与共享内存**：提供 Windows / Linux 本机共享内存通道、二进制帧协议和 `awj-stream` 工具。
- **平台适配**：支持 Windows D3D11 纹理导入与可选 FFmpeg 帧桥接。
- **集成配套**：提供动态库、公共 C 头、CMake package 和 C99 / C++11 示例；SDK 源码已通过 Clang 21 构建验证。

接入说明：[SDK 文档](https://github.com/Dominic485649/AWJimage/blob/1.3.0/docs/sdk.md) · [集成示例](https://github.com/Dominic485649/AWJimage/tree/1.3.0/examples/basic)

## 队列重复导入

- 编码成功的图片可再次导入，状态恢复为“等待编码”。
- 队列保留其他完成项及结果，支持与新图片混合排队。
- 同一路径自动去重，按最后一次有效导入的位置排列，例如 `[C,A,C,B]` 保留 `[A,C,B]`。
- Windows / Linux 使用一致规则，沿用现有预设和输出文件冲突策略。

## SDR / HDR 与 Gain Map

- SDR 输入保存为单层 SDR；单层 HDR 转 JXL / AVIF / PNG 时保留 HDR。
- 原图具有 SDR / HDR 两端时，JXL / AVIF 使用 **HDR 底图＋重建原 SDR 的反向 Gain Map**；JPEG 复用原 SDR 底图并保存 HDR Gain Map。
- 单层 HDR 转 JPG auto/hdr 时，自动高光滚降生成 SDR 底图，再通过 Gain Map 保留 HDR。
- WebP / JPG sdr 优先复用已有 SDR，其余 HDR 输入自动高光滚降为 SDR。
- HDR / HDR 双呈现选择较高 HDR；文件转换与 SDK 内存编码共用呈现策略。

## 下载

| 文件 | 用途 |
|---|---|
| `AWJ_Win.7z` | Windows x64 应用，`AWJ.exe` 与 `AWJ.com` 放在同一目录 |
| `AWJ_Linux.7z` | Linux x86_64 应用 |
| `AWJ_SDK_Win_x64.zip` | Windows SDK：DLL、导入库、C 头、CMake、工具与示例 |
| `AWJ_SDK_Linux_x64.tar.gz` | Linux SDK：动态库、C 头、CMake、工具与示例 |
| `AWJimage-1.3.0-source.zip` | 对应源码、依赖锁定、补丁、构建脚本和示例 |
| `SHA256SUMS` | 归档 SHA-256 校验清单 |

普通用户选择应用包；开发者选择对应平台 SDK。应用通过内置更新器升级，SDK 由集成方管理。

## 运行要求与许可

- 应用：Windows x64 **AVX2** / Linux **x86-64-v3**。Linux 预编译包基于 Ubuntu 24.04 / glibc；SDK 运行要求见对应包说明。
- 应用与 SDK 采用 **AGPL-3.0**，第三方组件许可随包提供。
- 1.0.12 及更早版本请手动下载升级；应用内更新采用签名验证与防回放校验。

<details>
<summary>English</summary>

## AWJimage 1.3.0 — In-memory pipelines and the C ABI SDK

**New Windows / Linux C ABI SDKs** let applications submit image frames and receive PNG, JPEGli, WebP, AVIF or JPEG XL results in memory. This release also improves queue re-import and unifies SDR / HDR rendition selection.

### Image encoding SDK

- A unified C ABI and public C headers for C / C++ integration.
- RGB / RGBA, floating-point HDR and YUV input, with read-only borrowing, resource lifetime management, copy diagnostics and in-memory output.
- Local shared-memory channels on Windows / Linux, a framed binary protocol and the `awj-stream` tool.
- Windows D3D11 texture import and an optional FFmpeg frame bridge.
- Dynamic libraries, CMake package files and C99 / C++11 examples. SDK source has been built with Clang 21.

### Queue re-import

Re-imported completed images become pending. Other completed items retain their results, alongside new images. Duplicate paths use the last valid occurrence and its position: `[C,A,C,B]` becomes `[A,C,B]`. Windows and Linux share the rules and retain existing preset and output-conflict settings.

### SDR / HDR and gain maps

- SDR input stays single-layer SDR. HDR-only input stays HDR in JXL / AVIF / PNG.
- Genuine SDR/HDR pairs use an HDR base plus a reverse gain map in JXL / AVIF; JPEG reuses the original SDR base with an HDR gain map.
- HDR-only input produces a tone-mapped SDR base plus an HDR gain map in JPG auto/hdr mode.
- WebP and JPG sdr reuse an existing SDR endpoint or tone-map HDR to SDR.
- HDR/HDR pairs select the higher-HDR rendition. File conversion and SDK encoding share the rendition policy.

### Downloads

Choose `AWJ_Win.7z` or `AWJ_Linux.7z` for the application, and `AWJ_SDK_Win_x64.zip` or `AWJ_SDK_Linux_x64.tar.gz` for SDK integration. Source is provided in `AWJimage-1.3.0-source.zip`; verify archives with `SHA256SUMS`. Applications use the built-in updater; integrators manage SDK upgrades.

Application CPU requirements are AVX2 on Windows and x86-64-v3 on Linux. Linux binaries are based on Ubuntu 24.04 / glibc; SDK requirements accompany each package.

The application and SDK use **AGPL-3.0**, with third-party licenses included. Versions 1.0.12 and earlier use a manual upgrade; the built-in updater verifies signatures and replay protection.

</details>
