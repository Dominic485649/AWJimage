# AWJimage

English: [README.en.md](README.en.md)

当前源码版本为 **1.0.13**。本地验证见 [验证记录](docs/validation-1.0.13.md)。已发布版本与下载见 [GitHub Releases](https://github.com/Dominic485649/AWJimage/releases)。

AWJimage 是一个 C++23 / Slint 批量图片转换工具。Windows 与 Linux 现已合并到同一主线。Windows 保留完整 shell/WIC/D3D11 支持；Linux 提供 Vulkan visual metrics 与 Release ELF。当前内置转换路径只保留 native codec：

- AVIF：libavif/AOM 与自动 AOM Grid；Windows 和 Linux Release 均静态链接
- PNG：libpng；q100 不量化，q1–99 量化 RGB 有效精度
- WebP：libwebp
- JXL：libjxl
- JPGLI：google/jpegli；Windows 与 Linux Release 均可用，生成 JPEG 兼容 bitstream，默认扩展名仍为 `.jpg`
- HEIC/HEIF 输入：libheif 1.23.4 + libde265 1.1.2 原生解码；仅编译 HEVC 解码能力，不启用 HEIC 编码器或动态插件

内置 ImageMagick/MagickWand 后端已经移除，Release 输出不再携带 ImageMagick XML、许可文件或模块目录。Magick 与 ffmpeg 以后只能作为外部集成重新引入；当前版本不处理该环节。

Linux 首版保留 Slint UI 与 CLI 共用单个 ELF `AWJ`；visual_quality GPU 指标路径使用 Vulkan，失败、小图或资源超限时自动回退 CPU。HEIC/HEIF 使用跨平台 native libheif/libde265 解码；WIC、JXR、`AWJ.com` shim 和 Windows 注册表 shell 集成仅限 Windows。Linux 上 WIC 兜底会被忽略并在界面中隐藏。Linux 右键入口使用用户级 Nautilus Scripts 与 Thunar UCA，不需要 sudo。

## 发行包

| 归档 | 精确内容 |
|---|---|
| `AWJ_Win.7z` | `AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt` |
| `AWJ_Linux.7z` | `AWJ`、`LICENSE`、`NOTICE.txt` |

两包均为扁平目录，使用 LZMA2 最高压缩、单线程且关闭自动过滤（`-m0=lzma2 -mx=9 -mmt=1 -mf=off`）。Linux 包在原生 Linux 文件系统打包并验证可执行位；发布正文列出实际大小和 SHA-256。构建信息、第三方通知与完整许可证汇总于 `NOTICE.txt`。

## 构建

Windows 推荐使用 MSVC 预设：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ AWJ-com
```

Linux 通用预设不绑定任何私有工具链，编译器和 vcpkg 都从环境读取：

```bash
export VCPKG_ROOT=/path/to/vcpkg
export CC=gcc-16 CXX=g++-16      # 默认 cc/c++ 不支持 C++23 模块时才需要
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release --target AWJ
```

前置条件：

- 支持 C++23 modules 的编译器（GCC 16+ 或 Clang 20+）与 Ninja、CMake 3.30+
- vcpkg：`VCPKG_ROOT` 指向其检出目录，manifest 依赖由 `vcpkg.json` 声明
- Linux 系统构建工具：`autoconf`、`autoconf-archive`、`automake` 和 `libtool`（vcpkg 构建 libsodium 需要）
- Rust toolchain（`cargo` 在 `PATH` 中）：用于构建 Slint。
- Vulkan：`find_package(Vulkan REQUIRED)`，由 vcpkg 的 `vulkan-headers` / `vulkan-loader` 满足
- DXC：visual_quality shader 在构建期编译为 SPIR-V，优先使用 vcpkg `directx-dxc` 提供的 `dxc`，否则回退到 `PATH` 中的 `dxc`

`linux-x64-debug` 用于 Debug。`linux-gcc-x64-*` 与 `linux-clang-x64-*` 是维护者专用预设，固定引用 `$HOME/.local/gcc-16.1-deb/wrappers/*-awj` 与 `$HOME/.local/vcpkg`，其他环境请使用上面的通用预设。

维护者在 WSL 本地路径（推荐 `/home/dominic/Code/Cpp/AWJimage`，不要直接编译 `/mnt/d`）使用：

```bash
cd /home/dominic/Code/Cpp/AWJimage
cmake --preset linux-gcc-x64-release
cmake --build --preset linux-gcc-x64-release --target AWJ
```

Release 预设（通用与维护者版本相同）使用 `-O3`、IPO/LTO、`-march=x86-64-v3`、section GC/strip，并静态链接 `libstdc++` / `libgcc`；只生成单个 `AWJ` ELF，没有 `AWJ.com`。`readelf -d bin/x64/Release/AWJ` 不应出现 `libstdc++.so.6` 或 `libgcc_s.so.1`。

Windows 也可使用脚本：

```powershell
# 本地开发/不生成发布 manifest
.\release.ps1 -SkipUpdateManifest
```

> 1.0.4 起 `release.ps1` 只负责 Windows 构建，拒绝写 manifest。跨平台归档、v2/v1 签名与 GitHub 发布流程见 [docs/release.md](docs/release.md)。

Windows 脚本会配置 native 依赖并清理 Release 输出目录。1.0.9 起公开 `AWJ_Win.7z` 的成员固定且必须严格只有：`AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt`；不再把 `.sha256`、`BUILD_INFO.txt`、`THIRD_PARTY_NOTICES.txt` 或许可证子目录放进归档。`NOTICE.txt` 汇总构建信息、第三方 notices 与所需完整许可证文本。签名 v2 manifest 继续绑定归档及逐成员 SHA-256。维护者本地 `bin\x64\Release` 还可保留 `awj_update_manifest_sign.exe` 作为签名工具，但它绝不进入公开发行包。

版本号由仓库根目录的 `VERSION` 文件控制，`CMakeLists.txt` 构建时自动读取，`scripts/Update-VcpkgVersion.ps1` 可同步到 `vcpkg.json`。正式发布必须使用仓库外保存的 Ed25519 seed，并将匹配的公钥编入客户端；私钥/seed 不得提交、复制到产物目录或写入日志。1.0.4+ 的实际归档、签名和 GitHub 发布步骤以 [docs/release.md](docs/release.md) 为准。

## 自动更新

1.0.6 起，客户端使用三把编译 root 中至少两把验证的 keyring 选择未撤销 release key，再验签带 `key_id`、递增 sequence、`issued_at` 和 `expires_at` 的 v1/v2 manifest。1.0.13 起优先读取 `update-keyring.json` / `update-archive.json` 及其签名，仅在新地址缺失时成对回退旧名称；验签失败、内容损坏或防重放失败不会触发降级。各类文档把最后已验签 sequence 和原始 SHA-256 以跨进程锁、刷盘、原子替换持久化到可执行文件同目录。Windows 与可写 Linux 安装目录继续在同卷 staging 中原子替换、健康检查和回滚；不可写 Linux 安装目录只打开 Release 页面，不静默提权。

旧 `update-manifest.json` schema 1 仍只用于 Windows 1.0.3→1.0.5 本机桥接；1.0.5 不加入 v2 候选。密钥保管、过期、撤销和轮换流程见 [自动更新签名与密钥轮换](docs/update-security.md)。

AVIF 使用静态集成的 AOM；超出单图能力范围时自动使用 AOM Grid。

`AWJ_ENABLE_JPEGLI` 默认开启，CMake 会拉取 google/jpegli 并静态链接 `jpegli-static`。JPGLI 没有独立容器格式，AWJ 的 UI、CLI、summary 和日志统一显示 `JPGLI` / `jpegli`，但输出文件扩展名默认保持 `.jpg`，以兼容系统缩略图和常见图片查看器。

构建 visual_quality GPU 指标路径时，Windows 使用 Windows SDK `fxc.exe` 预编译 Direct3D 11 shader；Linux 使用 vcpkg `directx-dxc` 生成 Vulkan SPIR-V。shader 会内嵌进程序，最终用户运行 Release 不需要 shader sidecar。

## visual_quality GPU 指标路径

`--visual-quality` 的 1..99 自动搜索会重复编码候选、从内存解码候选并计算视觉指标。默认启用的 `--visual-quality-gpu` 加速的是指标分析路径：Windows 走 Direct3D 11 compute shader，Linux 走 Vulkan compute shader，负责 luma、GMSD、MS-SSIM 和 MS-SSIM downsample。候选 codec 编码/解码、候选选择和最终 encoded bytes 返回仍是 native CPU memory pipeline；这不是端到端 GPU 转码。

GPU 不可用、小图低于收益阈值、资源超限或 GPU readback/shader 创建失败时会自动回退 CPU。启用 `--summary` 或 `--log` 后，可通过 `visual_quality_gpu_requested`、`visual_quality_gpu_used`、`visual_quality_gpu_path`、`visual_quality_gpu_fallback_reason`、`visual_quality_gpu_fallback_count` 以及各阶段耗时判断实际路径。

## CLI 示例

Windows 命令行建议直接输入 `AWJ ...`；终端中会优先使用同目录的 `AWJ.com` shim，并转发到 `AWJ.exe`，从而保留等待、stdout/stderr 和退出码。Linux 直接运行单个 ELF `AWJ`，没有 `AWJ.com`。无参数启动 Slint Studio，带 CLI 参数进入命令行转换。

```powershell
AWJ -i "D:\图片" -o Avifoutput --format avif -q q90
AWJ -i input.png --format webp --template "{name}-{date}"
AWJ -i input.png -o output.jxl --format jxl -q 90
AWJ -i input.png --format jpgli -q 95 --summary
AWJ -i input.png --format avif --avif-encoder aom --chroma 444 --bit-depth 10
```

`-i` / `-o` 会去除外围空白和一对完整双引号，因此 `-i '"D:\example.jxr"'` 与 `-i D:\example.jxr` 等价；空路径、不配对引号或路径内部引号会明确报错。`--list-presets` 动态列出可执行文件旁有效的 `preset/*.jsonc`（无效文件也会报告具体原因）；`-p/--preset <名称>` 按名称加载，`--preset-file <路径>` 加载指定 JSONC。不选预设时使用当前内置默认，显式 CLI 参数始终覆盖预设且与参数顺序无关。AVIF 默认 speed 为 5。

Windows CLI 还可在输出原子提交成功后保留源文件时间：`--preserve-creation-time`、`--preserve-modification-time`、`--preserve-access-time`。写回失败只产生 stderr/日志警告，不会丢弃有效输出。Linux 不显示并拒绝这些 Windows 专用参数。

Windows CLI 可把单帧 WGC HDR 捕获管道接入：`捕获程序 | AWJ --stdin-wgc-rgba16f 3840x2160 -o D:\输出 --format avif`。它只接受精确长度的 `DXGI_FORMAT_R16G16B16A16_FLOAT`（小端 RGBA binary16、线性 scRGB）一帧，必须显式给出宽高和 `-o`，不能同时传 `-i`；短帧、额外帧字节或未知裸 RAW 一律拒绝，不猜测格式。

旧配置或预设若显式选择已移除编码器，会报告需要修正的字段；CLI 仅保留 `--avif-encoder auto/aom`。

### AVIF 大图与队列策略

AVIF 普通单图会尽量留在普通编码队列：AOM/libaom 上限为宽高各 1..65536 且总像素不超过 `2^30`（1,073,741,824）。超过 1000 万像素但仍在单图上限内的图片会排到普通队列末尾，避免单张大图的内存估算降低全部小图的并发；它们仍使用普通编码器，不会强制进入大图链路。普通、延后和大图阶段先取 CPU、内存与任务数允许的有效并发，再分配每图线程。

PNG 默认 q100，不增加像素量化；没有其他像素变换时，解码后的像素保持一致。q1–99 逐行量化 RGB，再缩放回完整数值范围；8-bit/16-bit 存储位深不变，16-bit 有效精度最低为 10 位，alpha 保持原精度。合法的逐通道 `sBIT` 会被读取并保存，量化后的 RGB 有效位数独立记录。PNG 不支持视觉质量搜索；降低质量不保证文件更小，渐变与暗部应检查条带。

`--alpha auto` 会自动保留非不透明 alpha。AVIF 的颜色与 alpha 都按请求的质量或 visual-quality 搜索结果编码；`--chroma auto` 优先保留源图表示：YUV 源保留 420/422/444，RGB/RGBA 使用 444，灰度或未知源使用 420。默认 YUV 不会因无损或 444 自动改用 Identity；只有显式 `source/rgb` 颜色表示可选择 RGB/GBR Identity。q100 仅对未请求改写色彩、alpha、位深或元数据的既有 YUV420 AVIF 原码流直通；其他输入使用 AOM 无损量化，并按上述 auto 规则重编码。CICP 按“用户显式值 > 源图值 > 兜底”生效，BT.2020/PQ/HLG 等 HDR 源会原样保留，只有源图和用户都没有 CICP 时才回退 BT.709/sRGB；source CICP range 默认保持（PC/full 或 TV/limited），未知 range 使用 full。`--alpha off` 会移除 alpha。

超过 AOM 单图上限后自动进入大图链路：
使用 AOM Grid；编码器不可用、规划失败或触达输入／运行时内存上限时明确报错。

Studio 不再提供独立大图页；自动处理状态直接显示在主队列。CLI：
- `--unlock-max-input-file-bytes` / `--unlock-20gib-limit`：仅当前会话解除默认 20 GiB 输入/运行时上限，不写入 `AWJ.jsonc`；过大图片可能 OOM。
参数设置页其余编码参数同样只在本次运行内保持，不写入 `AWJ.jsonc`。

Studio 的自动线程预算在逻辑线程数 >=12、5-11、2-4 时分别预留 4、2、1 个线程；单线程仍使用 1。自动内存上限取总内存 80% 与当前可用内存 50% 的较小值，只有一项可用时按该项计算。

`--format jpgli` 和 `--format jpegli` 是 JPGLI 入口；`--format jpg` 不作为新增入口。启用 `--summary` 后，`summary.csv` 会记录 `format=JPGLI`、`encoder_id=jpegli`，输出路径仍通常是 `.jpg`。

`--allow-wic-fallback` 仅 Windows 有效；Linux 会接受但忽略该参数。Linux UI 的“选择”按钮会调用 `zenity`/`yad`/`kdialog`。右键菜单为 AVIF、WebP、JXL、JPGLI、PNG 分别写入 Nautilus 用户脚本，并同步写入 Thunar UCA；必要时重启文件管理器。

多帧 WebP/GIF/APNG/JXL/TIFF/AVIF、Windows WIC 多帧和 JPEG MPF 输入只转换合成后的第一帧；无法可靠提取时直接报错。AVIF grid 支持非整数倍布局，右列和底行使用实际剩余尺寸，不会改变输出宽高；若奇数 grid 与实际选择的 420/422 采样不兼容，会明确要求显式选择 444，而不会静默改变采样。

Studio 的编码队列支持拖拽文件/文件夹导入，也可以继续使用“选择输入”按钮。拖入目录时会按现有扫描规则批量处理图片；主页队列中未开始的项目可直接拖动调整顺序，右键仍可打开队列菜单。0.10.3 增加待处理、处理中、成功和失败计数、仅看失败、重试失败项及选中项详情；详情保留完整错误、输入/输出路径、编码器、线程数和 decode/prepare/encode/write 阶段耗时。

队列的输出格式、预设、移除元数据和 Windows 三种时间戳均是本次运行设置：点击开始时会快照整批任务，默认输出 AVIF、保留元数据、三个时间戳均不勾选。它们与参数页和右键菜单设置独立；参数页仅编辑五种格式参数，用户预设必须在队列中显式选择。

SoftComboBox、SoftButton、左侧导航与队列右键菜单提供 Tab 焦点、可见焦点状态、Enter/Space、方向键、Home/End、Esc 以及可访问角色/名称。字体下拉框仍最多显示 10 行，支持滚轮和滚动条，不提供搜索或手动输入。参数页按常用参数、资源限制、格式高级选项分组，右侧以彩色文字说明格式、资源和风险；右键菜单预设独立保留。

Windows Explorer 多选图片或文件夹后执行同一个 AWJ 右键命令时，启动请求会合并到一个右键窗口和同一队列。右键窗口提供普通取消与强制终止；本体和右键窗口点击右上角关闭时会先终止活动任务。

## 基准测试

当前 Windows 基准只测试 CLI，使用 `D:\图片\benchmark\test` 的 613 个文件。当前脚本测量 AWJ 默认 AVIF 行为和严格的 ffmpeg 对照；运行命令：

```powershell
pwsh -NoProfile -File .\scripts\benchmark.ps1 `
  -InputRoot 'D:\图片\benchmark\test' `
  -AwjExecutable .\bin\x64\Release\AWJ.exe `
  -FfmpegExecutable 'D:\DevTools\Cli\FFmpeg\ffmpeg.exe' `
  -PowerSchemeGuid 381b4222-f694-41f0-9685-ff5bb260df2e `
  -Mode All
```

当前协议不向 AWJ 传入质量、速度、色度、位深或内存参数，因此验证实际默认值：AOM、quality 70、speed 5、按源格式自动选择色度、自动至少 10-bit，以及非不透明 alpha 自动保留且同样按 q70 编码。严格对照固定使用 ffmpeg AOM QP 23、10-bit 4:2:0、all-intra、row-mt，并按像素、字节、路径排序。脚本先自检输入和 `summary.csv` 的实际参数，再记录 Job Object 覆盖的整棵进程树 CPU/峰值内存。

默认 `All` 同时运行回归和严格对照；`Regression` 只运行 AWJ，`Strict` 只运行 210 张不含 ICC/EXIF/XMP 的不透明图片与 ffmpeg。非 smoke 运行每组预热一次、测量五次，P95 使用 nearest-rank；每次调用间冷却 30 秒。结果写入 `build/benchmarks/`，不应把单轮最快值当作版本结论。

### 0.10.3 严格对照归档

以下结果是 2026-07-16 用旧版 q80/8-bit 协议采集的历史记录，不是当前脚本的复现输出。被测版本为 AWJ 0.10.3 MSVC Release、commit `2798db2`、AOM 3.13.3，以及 ffmpeg `git-2026-07-14-312c830916`、AOM 3.14.1。

### 大图能力结果

| 工具 | 结果 | 墙钟 | 进程 CPU | 采样峰值内存 | 输出 |
|---|---|---:|---:|---:|---:|
| AWJ | AOM grid 编码成功 | 167.554 s | 892.016 s | 23,707.0 MiB | 164.39 MiB |
| ffmpeg | 进入 libaom 前被 MJPEG 解码器拒绝 | 0.246 s（失败用时） | 不适用 | 309.5 MiB | 无 |

ffmpeg 的失败签名为 `MJPEG packet ... too big`；0.246 秒只是读入缓存后的失败用时，不是编码吞吐量，因此不计算大图速度比。

### 613 张结果

| 工具 | 调度 | 成功 / 失败 | 墙钟中位 / P95 | 进程 CPU 中位 / P95 | 采样峰值中位 / P95 | 吞吐量 | 输出中位数 |
|---|---|---:|---:|---:|---:|---:|---:|
| AWJ | 12 文件 × 1 编码线程 | 612 / 1 | 110.878 / 111.891 s | 940.047 / 956.859 s | 2810.0 / 2820.5 MiB | 5.529 img/s，14.668 MP/s | 128.98 MiB |
| ffmpeg | 12 进程 × 1 编码线程 | 612 / 1 | 107.671 / 109.772 s | 855.688 / 865.672 s | 3242.1 / 3275.8 MiB | 5.693 img/s，15.104 MP/s | 131.48 MiB |

五轮墙钟原始值：AWJ 为 111.364、109.919、110.883、111.891、107.727 秒；ffmpeg 为 109.772、108.981、106.784、107.671、106.767 秒。ffmpeg/AWJ 墙钟中位数比为 0.97×：本机此次协议下 ffmpeg 约快 2.9%，而 AWJ 的采样峰值内存中位数约低 13.3%，不构成 AWJ“大幅提速”的证据。

AWJ 的 `decode / prepare / encode / write` 阶段总时长中位数/P95 分别为 37.529/38.040、0.278/0.291、1077.766/1090.467、8.916/9.609 秒。阶段值是并发图片的逐项求和，可以大于整批墙钟；ffmpeg 多进程运行未提供可比的阶段拆分。

这是端到端 CLI 比较，不是同一 libaom 构建的微基准，也没有证明两组输出视觉质量相同。两者使用不同的 AOM 版本、构建选项以及解码/色彩转换前端；固定 AWJ→ffmpeg 顺序还可能让 AWJ 获得较低初始温度、让 ffmpeg 获得文件缓存。因此结果只描述这台机器和本次协议，不把差异归因于 AWJ glue code 或某个 AOM 参数。

### visual_quality 视觉质量评估

AWJ 的 `--visual-quality` 选项通过 IQA 算法自动搜索最优编码参数：**GMSD**（对模糊与结构失真敏感）与 **MS-SSIM**（多尺度结构相似性）混合评分。

该混合算法兼顾稳定性与速度，在以 3–4 倍于普通编码的时间消耗下，能为指定批次找到同视觉质量下的最小体积。`--visual-quality-gpu` 在 Windows 使用 Direct3D 11、在 Linux 使用 Vulkan 加速 luma、GMSD、MS-SSIM 及 MS-SSIM downsample 计算路径；GPU 不可用或小图低于收益阈值时自动回退 CPU。

## 测试

普通构建/验证只构建面向平台的发布目标：Windows 为 AWJ 和 AWJ-com，Linux 为 AWJ，不构建测试可执行文件。只有明确需要测试验证时，才单独构建测试目标并运行：

```powershell
ctest --test-dir build/x64/Release -C Release --output-on-failure
pwsh -NoProfile -File .\scripts\cli-worker-smoke.ps1
```

`cli-worker-smoke.ps1` 不启动 Studio 窗口：它通过真实 CLI manifest 验证 ITEM/DETAIL、失败项重试输入、命名事件普通取消和 Job Object 强制终止进程树。Slint 的小型 component smoke 使用 testing backend，无窗口验证页面切换、键盘、字体滚动、深浅色、820×560 与 100%/150%/200% scale。0.10.4 Windows MSVC Release 为 31/31 通过。

Linux Release 测试：

```bash
cmake --preset linux-gcc-x64-release -DBUILD_TESTING=ON
cmake --build --preset linux-gcc-x64-release
ctest --test-dir build/linux-gcc-x64-release --output-on-failure
```

0.10.4 Linux GCC 16.1 Release 为 16/16 通过；ELF 为 55,614,152 bytes，不依赖动态 `libstdc++.so.6` / `libgcc_s.so.1`，并确认 `-O3`、LTO、`x86-64-v3` 与静态 C++/GCC runtime 标志。
