# C++23 迁移说明

## 目标

本项目已经从早期控制台原型收敛为 C++23 / Slint 批量图片转换工具。Windows 与 Linux 已合并到同一主线；平台差异通过 `WIN32` / POSIX 隔离，core/pipeline/codecs 共用。迁移目标不是逐行保留旧实现，而是保留批量转码工作流，并把配置、扫描、调度、native codec、CLI 和 Studio UI 拆成可维护的模块。

## 当前后端

当前转换核心只保留 native codec，不再依赖内置 ImageMagick/MagickWand 后端。

- AVIF：libavif/AOM 与自动 AOM Grid；Windows 与 Linux GCC Release 均启用。
- WebP：libwebp。
- JXL：libjxl。
- JPGLI：google/jpegli，Windows 与 Linux GCC Release 均可用；输出为 JPEG 兼容 bitstream，默认扩展名为 `.jpg`，用户可见格式和诊断显示为 `JPGLI` / `jpegli`。
- PNG/JPEG/TIFF/GIF/BMP/RAW：作为输入解码路径参与批处理和 metadata 透传；JPEG 兼容输入在支持 JPGLI 的构建中优先尝试 Jpegli decoder，失败后按现有策略回退 libjpeg-turbo。WIC、JXR/HD Photo 与 HEIC/HEIF 兜底仅限 Windows；Linux 上 WIC 兜底会被忽略并在界面中隐藏。

Magick 与 ffmpeg 以后只能作为显式配置的外部 exe/runtime 集成方向，不应恢复为默认内部后端或随 Release 输出携带。

## 当前转换流程

1. native decoder registry 按输入格式选择 PNG/JPEG/WebP/TIFF/GIF/JXL/AVIF/RAW 等解码器；Windows 构建额外注册 WIC/JXR。
2. 解码器返回像素、ICC、EXIF/XMP、色彩/HDR 和 alpha 语义。
3. 根据配置执行可选缩放、alpha 策略、色度采样、位深选择和视觉质量搜索；`visual_quality` 指标默认走 GPU-first 路径，Windows 使用 Direct3D 11，Linux 使用 Vulkan，必要时 CPU fallback。
4. AVIF auto 只选择允许参与自动选择的稳定 encoder；实验 encoder 必须显式开启并选择。
5. AVIF 超过单图编码上限时进入大图模式并按资源规划拆分 grid/tile；1000 万像素以上但未超 AOM 65536 边/2^30 像素上限的图片只延后到普通队列尾部。
6. 编码器写回 ICC、EXIF/XMP、色彩和 HDR metadata；不支持的组合返回明确错误。
7. 输出写入先经过临时文件与碰撞策略，再落到目标路径。
8. 日志与 `summary.csv` 记录实际后端、质量、fallback、GPU 指标路径和诊断信息。

## 保留的功能

- 批量扫描文件或目录，支持 `jpg/jpeg/png/webp/bmp/dib/rle/tif/tiff/gif/jxl/avif/awsraw` 与常见 RAW 扩展；`heic/heif/jxr/wdp/hdp` 通过 Windows WIC 路径支持，Linux 首版不暴露这些 WIC 兜底入口。
- 输出名模板 `{name}` / `{index}` / `{ext}` / `{date}` / `{time}` / `{datetime}` / `{unix}` / `{rand}` / `{hash}` / `{hash8}` / `{params}`，CLI 默认 `{name}`。
- 输入为文件夹时保留原始子文件夹结构。
- 输出格式可选 AVIF、WebP、JXL 或 JPGLI。
- 未选择用户预设时使用当前内置默认；Windows 新建用户预设采用 `%LOCALAPPDATA%/AWJimage/preset/*.jsonc` 的五格式完整参数集，已有便携版会继续读取可执行文件同目录 `preset/*.jsonc`，CLI 可用 `--preset <名称>` 或 `--preset-file <路径>` 显式选择。
- AVIF 默认 q70、WebP 默认 q95、JXL 默认 q85、JPGLI 默认 q90，仍支持 `q90` 风格质量参数。
- 质量范围为 q1..q100；JXL q100 对 JPEG 输入在未请求剥离元数据或改写色彩/HDR 时使用原始码流级无损转封装，其他 WebP/JXL q100 为编码器无损；JPGLI q100 表示最高质量 JPEG 兼容编码，不声明像素级无损；AVIF q100 仅对未请求改写色彩、alpha、位深或元数据的既有 YUV420 AVIF 原码流直通，其他输入使用 AOM 无损量化并按 `auto` 的 source-aware 色度规则重编码；源图为 420/422 时仍存在色度子采样，需要显式 444 才能避免。
- AVIF 采样支持 `auto/444/422/420`，`auto` 优先保留源图表示：YUV 源保留 420/422/444，RGB/RGBA 使用 444，灰度或未知源使用 420。默认 YUV 不会因无损或 444 自动改用 Identity；只有显式 `source/rgb` 颜色表示可选择 RGB/GBR Identity。位深留空时按源图和编码器能力选择，显式填写时支持 `8/10/12`。非不透明 alpha 在 `auto` 下保留，颜色与 alpha 都跟随请求质量。
- CICP 优先级为“用户显式值 > 源图值 > 兜底”。BT.2020、PQ、HLG 等 HDR 源的 primaries/transfer/matrix 会原样保留，只有源图和用户都没有提供 CICP 时才回退 BT.709/sRGB；color range 默认保持 PC/full 或 TV/limited，未知时使用 full。
- JXL 不支持手动 chroma sampling，位深留空保持原片，可通过 native libjxl effort/speed 控制压缩成本。
- WebP 固定 8-bit；有损 WebP 为 Y'CbCr 4:2:0，无损 WebP 为 ARGB。
- JPGLI 固定 8-bit RGB JPEG 兼容输出，不提供手动 chroma/alpha 输出入口；ICC、EXIF、XMP 按现有保留/剥离规则处理。
- 并行处理，处理时优先调度小图；超过 1000 万像素但未超 AVIF 单图上限的普通任务排到队列尾部，单任务编码线程数仍按 encoder 线程预算保留。
- 重名输出支持覆盖、跳过、追加时间后缀、追加随机后缀。
- 日志与 `summary.csv` 可选生成。
- JPGLI 输出文件默认 `.jpg`；`summary.csv` 明确记录 `format=JPGLI`、`encoder_id=jpegli`，避免把它误读为普通 JPG 输出入口。
- 单张失败继续处理后续图片。

## 新增内容

- CMake 根工程；Windows 生成 `AWJ.exe` 和命令行 shim `AWJ.com`，Linux 生成单个 ELF `AWJ`。
- Slint 桌面 UI 与 CLI 共用统一 AWJ 入口，主页普通队列支持拖动未开始项目调整顺序。
- UI 支持跟随系统主题，也可以手动切换浅色/深色；Windows 使用注册表 shell/右键菜单，Linux 首版支持 Nautilus/Thunar 用户级自定义动作。
- `run_batch(config, progress_callback, stop_token)` 批处理服务，CLI/UI 共用。
- 大图模式、视觉质量搜索、AVIF encoder registry、资源规划和 GPU 视觉指标路径：Windows Direct3D 11，Linux Vulkan。
- Windows JPGLI/Jpegli native codec、JPEG 兼容输入的 Jpegli-first decoder registry 和 summary 格式诊断列。
- visual_quality shader 在 CMake 构建期预编译并内嵌；Windows 使用 `fxc.exe` 生成 Direct3D bytecode，Linux 使用 vcpkg `directx-dxc` 生成 Vulkan SPIR-V，Release 不需要 shader sidecar。
- 批量编码时只降低转换工作线程 CPU 优先级，避免 UI/CLI 启动阶段在高负载下被系统调度饿住。
- Windows Release 默认保留 `/O2`、函数级裁剪、常量合并、链接器裁剪和 x64-v3/AVX2 代码生成；Linux GCC Release 预设使用 `-O3`、IPO/LTO、`-march=x86-64-v3`、section GC/strip，并静态链接 `libstdc++` / `libgcc`。

## visual_quality GPU 指标路径

当前 GPU 加速范围是 visual_quality 的指标分析，不是端到端 GPU 转码。Windows `awj.visual_metrics_gpu` 使用 Direct3D 11 compute shader，Linux `awj.visual_metrics_gpu_vulkan` 使用 Vulkan compute shader，计算 luma、GMSD、MS-SSIM 和 MS-SSIM downsample；`awj.native_visual_search` 在 1..99 自动搜索中为每张参考图创建可复用 `AcceleratedVisualMetricSession`，复用 reference luma / reference MS-SSIM levels，并让 candidate luma 常驻 GPU 供 GMSD 和 MS-SSIM 连续使用。

候选 encode/decode 仍由 native CPU codec 完成，候选选择、CSV/log 汇总和最终 encoded bytes 返回也仍在 CPU memory pipeline。GPU 小图阈值是刻意的性能保护：低于阈值时 GPU 初始化、上传和 readback 的成本通常高于收益。

`summary.csv` 与日志会记录 `visual_quality_gpu_requested`、`visual_quality_gpu_used`、`visual_quality_gpu_path`、`visual_quality_gpu_fallback_reason`、`visual_quality_gpu_fallback_count`，用于区分 GPU 未请求、小图 CPU、GPU 初始化失败、session 中途失败后 CPU fallback 等情况。`visual_quality_fallback` 只表示“质量未达标时是否输出最接近候选”，不控制 GPU 到 CPU 的兼容性 fallback。

## 简化的部分

- 移除了内置 ImageMagick/MagickWand 和 ffmpeg/ffprobe 后端；当前质量搜索以 native 视觉质量评分为主。
- 质量参数改为各 native encoder 的质量入口，AVIF/WebP/JXL/JPGLI 由统一配置再映射到具体库。
- 不再默认缩放到固定长边，避免用户误以为编码质量差其实是分辨率被改动。
- 不再提交 Scoop/ImageMagick 二进制，仓库只保留外部集成所需的说明和历史脚本。
- Slint 默认静态链接，减少 UI 分发时 DLL 数量。

## 模块划分

- `awj.config`：参数结构、预设、帮助文本、命令行解析。数值解析使用 vcpkg 的 `scnlib`。
- `awj.core`：UTF-8/宽字符转换、图片扫描、输出路径规划、日志、CSV 和少量平台工具。
- `awj.image` / `awj.codec`：共享图像缓冲、metadata、codec capability 和 encode/decode 数据结构。
- `awj.decoder_registry` / `awj.*_codec`：native 格式解码、编码与 metadata 透传；启用 JPGLI 时由 `awj.jpegli_codec` 负责 Jpegli encode/decode 与 JPEG 兼容 metadata marker 处理。
- `awj.avif_registry`：AVIF encoder 能力注册与选择策略。
- `awj.native_backend`：native decoder/encoder 调度、libavif/AOM 静态集成、临时文件和输出写入边界。
- `awj.pipeline`：多线程调度、进度事件和汇总。
- `awj.native_visual_search` / `awj.visual_metrics` / `awj.visual_metrics_gpu` / `awj.visual_metrics_gpu_vulkan`：视觉质量搜索、CPU/GPU 质量指标和 fallback 诊断。
- `src\cli\main.cpp`：CLI 入口。
- `src\cli\shim_main.cpp`：仅限 Windows 的 `AWJ.com` console shim，转发到同目录 `AWJ.exe` 并透传命令行输出与退出码；Linux 没有该目标。
- `src\ui\main.cpp`：Slint UI 入口；Windows 包含 Win32 文件/文件夹选择与 shell 集成，Linux 首版使用 `zenity`/`yad`/`kdialog` 作为文件选择器并可写入 Thunar 用户级右键菜单。

## 进阶改动

- CLI 文本输出使用 C++23 `std::println`，避免继续扩散 `cout`/`printf` 风格输出。
- 批处理由统一 AWJ 可执行程序以 worker 模式执行；Studio 与 CLI 共用同一套参数解析与批处理服务。Windows 命令行入口由 `AWJ.com` shim 转发到 `AWJ.exe`，保留终端等待、stdout/stderr 和退出码；Linux 直接运行单个 ELF `AWJ`，没有 `AWJ.com`。Windows UI worker 继续用 Job Object 管理生命周期。
- Windows Studio worker 使用版本化 ITEM/DETAIL 文本协议回传状态、编码器、线程数和 decode/prepare/encode/write 耗时；队列运行索引与原始队列索引分离，因此“仅重试失败项”仍能把结果映射回原条目。
- C API 与平台 API 交界处统一用 `std::unique_ptr` 自定义 deleter 或局部 RAII 类型表达拥有关系，Windows 覆盖 Win32/COM 的 `Release`、`CoTaskMemFree`、`LocalFree`，通用路径覆盖 libavif/libjxl/libwebp/libraw 等资源释放。
- 参数解析、文件系统、decoder/encoder 入口和重要分配路径使用 `std::expected<T, std::string>` 表达错误。
- 索引型循环优先使用 `std::views::iota`，只有依赖迭代器失效、外部 C API 或更清晰的资源生命周期时才保留传统循环。
- 只在所有权、跨线程时序或格式边界不直观的地方保留中文注释，避免普通业务流程被注释噪音淹没。
- 单文件处理内部有异常兜底，某张图片失败不会终止整个批处理。
- 工程保留 vcpkg/CMake 配置，可继续引入高质量第三方库，但优先保持 native codec 路径清晰可验证。

## 构建与验证约束

日常验证只构建面向平台的发布目标：Windows 构建 AWJ 和 AWJ-com，Linux 构建 AWJ。Release preset 会预编译并内嵌 visual_quality shader；Windows 需要 Windows SDK `fxc.exe`，Linux 使用 vcpkg `directx-dxc` 与 `x64-linux` triplet。Release preset 默认启用 `AWJ_ENABLE_X64_V3=ON`，MSVC Release 添加 `/arch:AVX2`，Linux GCC Release 添加 `-O3`、IPO/LTO、`-march=x86-64-v3`、section GC/strip 和静态 `libstdc++` / `libgcc`。

Windows：

```powershell
cmake --build --preset windows-msvc-x64-release --target AWJ AWJ-com
```

Linux 首版工作路径为 `/home/dominic/Code/Cpp/AWJimage`，推荐 GCC 预设：

```bash
cd /home/dominic/Code/Cpp/AWJimage
cmake --preset linux-gcc-x64-release
cmake --build --preset linux-gcc-x64-release --target AWJ
```

Debug 使用 `linux-gcc-x64-debug`。Release 验证应确认 `bin/x64/Release/AWJ` 是 ELF、目录中没有 `AWJ.com`，`readelf -d` 不包含 `libstdc++.so.6` / `libgcc_s.so.1`，`build.ninja` 中可见 `-O3`、`-flto`、`-march=x86-64-v3`、`-static-libstdc++` 和 `-static-libgcc`。0.10.4 GCC 16.1 Release 实测 55,614,152 bytes（约 53.0 MiB）。

测试可执行文件只在明确需要测试验证时单独构建，不能混入普通构建步骤。0.10.4 的 Windows MSVC Release 为 31/31，Linux GCC Release 为 16/16；Linux 测试配置需显式传 `-DBUILD_TESTING=ON`。Slint component smoke 使用 testing backend，不打开窗口；Windows 取消/强制终止由 `scripts/cli-worker-smoke.ps1` 直接测试 CLI worker、命名事件与 Job Object，不依赖 UI Automation。

## GitHub 发行归档（1.0.4+）

所有暂存、归档和旧版样本都位于仓库 `build/`、`bin/`。从 1.0.6 起，`scripts/package-linux-release.sh` 必须在原生 Linux 文件系统中生成、`7z t`、全新解压并启动验证 `AWJ_Linux.7z`，以保留 `AWJ` 可执行位；Windows `scripts/package-release.ps1` 只生成 `AWJ_Win.7z`，并重新解压、逐文件哈希复核原生 Linux 归档。两个包分别包含平台二进制及其校验、`LICENSE`、`THIRD_PARTY_NOTICES.txt`、`BUILD_INFO.txt`，不混装跨平台二进制。

1.0.4 prerelease 只上传这两个归档，并由签名 `update-manifest-v2.json` 记录归档和每个必需成员的 URL、大小和 SHA-256。客户端严格拒绝路径穿越、链接、额外成员和解压炸弹。1.0.5 是功能等价的 1.0.3 自动更新桥接 prerelease：除两个归档外仅额外上传裸 `AWJ.exe`、`AWJ.com`，只写入旧 v1 manifest；普通用户应下载 1.0.4。Windows 更新在本机测试，WSL 仅进行普通构建、CTest、CLI、ELF 和归档验证。


## Linux UI 与文件管理器

Linux Studio 继续使用 `ui/awj_studio.slint`，不重做 UI。Win32 DWM、COM picker、注册表 shell 菜单和 `AWJ.com` 只在 Windows 构建；Linux 使用系统工具做最小平台适配：文件选择器依次尝试 `zenity`、`yad`、`kdialog`，打开输出目录依次尝试 `gio`、`xdg-open`、`thunar`。Ubuntu 默认 Nautilus 通过用户脚本目录支持右键入口，Thunar 通过 `~/.config/Thunar/uca.xml` 支持用户级自定义动作。

## Studio 可访问性与队列诊断（0.10.3）

- 自绘 combo、button、导航和队列菜单显式提供焦点、键盘操作、可访问角色/名称/状态；生产 Slint 输出不启用 experimental debug metadata，只有测试目标使用独立 `_ui_smoke` 生成文件。
- 字体列表最多显示 10 行并保留滚轮/滚动条；参数页按常用、资源、高级格式分组，危险说明常驻。
- 队列模型保留 pending/running/success/failed 计数、失败过滤、失败重试和详情字段。完整错误与路径不再依赖表格列宽展示。
- 820×560、100%/150%/200% scale 和长文本几何由无窗口 smoke 覆盖；这不是屏幕阅读器认证，仍需在平台可访问工具发生兼容问题时补充针对性验证。
## 后续可扩展点

- 如果未来需要更精确的目标体积，可以在 native 视觉质量搜索之外增加体积约束策略。
- 可以把体积较大的源码依赖接入 CI cache，减少首次 Release 构建时间；体积继续优化优先考虑拆分可选 codec/Slint renderer，而不是牺牲静态 `libstdc++` / `libgcc` 兼容性。
- 如需重新接入 Magick/ffmpeg，应以外部 exe/runtime 形式隔离，不能恢复为默认内部后端。

## 大图处理（0.10.1）

- 超过 AVIF 单图硬限制的输入进入自动大图链路：使用 AOM Grid；不再提供编码器优先顺序选项。
- Studio 不再保留独立“大图模式”页面，自动处理状态并入主队列。
- 两条路径都不可用/失败，或触达输入/运行时上限时明确报错。
- 默认 20 GiB 输入/运行时上限可通过会话开关解除（UI 设置页 / CLI `--unlock-max-input-file-bytes`），不写入 `AWJ.jsonc`。
- grid 支持非整数倍布局，右列和底行使用实际剩余尺寸并保持原始输出宽高；默认 420 与奇数 grid 不兼容时会明确报错，不会自动改为 444。
