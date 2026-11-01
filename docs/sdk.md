# AWJimage 1.3.1 C ABI SDK

本 SDK 接收其他软件主动提交的图像帧，直接处理 CPU 内存、Windows D3D11 纹理、共享内存槽位或二进制帧流。它不附带录屏选择器、WGC capture session、视频播放器或完整 FFmpeg 解码器，也不能无侵入读取任意软件的私有内存。“任何软件”指能够按平台 C ABI 和资源所有权契约集成 SDK 的软件，不等于支持未知像素布局或任意地址。

**公共接口只有 C ABI。** C++ 调用方也包含同一套 C 头；没有另一套 C++ ABI、`awj.hpp` 或导出的 C++ 封装。内部保持 C++23，私有 modules/BMI 不进入 SDK 消费接口。C99、C++11 和 C++17 调用方使用同一公共 C 接口，并匹配目标 OS、架构、C 结构布局和调用约定。

本项目继续适用现有 AGPL-3.0；库化和 C ABI 不会自动给予闭源软件链接/分发的许可豁免。第三方许可证见 `LICENSE`、`THIRD_PARTY_NOTICES.txt` 和 `licenses/`。各目标平台的动态 SDK 通过独立归档分发，下载与校验文件随同一版本 Release 提供。

## 平台与当前边界

| 目标 | 当前阶段 |
| --- | --- |
| Windows x64，MSVC cl.exe | 应用与完整五编码器 SDK 使用微软编译器及静态 MSVC runtime；Release 优化、IPO/LTO 与实际验收记录随包保存 |
| Windows C ABI 调用方 | MSVC 和 clang-cl 可作为独立消费者；调用方编译器不等同于 SDK 的生产编译器 |
| D3D11 | WARP 测试覆盖回读、严格模式、映射借用、池背压；共享 NT/keyed mutex 子项独立记录是否通过或跳过 |
| 真实 WGC、物理 GPU HDR、跨设备共享 | 未验证；WARP 不替代这些结果 |
| FFmpeg | 可选桥接，按精确 libavutil 头版本构建；Windows DLL 与 Linux `.so` 分别验证，不混用证据；未验证实际 D3D11 解码会话 |
| Linux x86_64，WSL Clang 23 | 1.3.1 使用独立原生 Clang23 构建；保留匿名共享内存、FFmpeg、帧流与只读页适配，执行记录随包保存 |
| HMOS ARM64/API26，WSL Clang23 + Native26.0.0.105 | 完整 CPU C ABI SDK，目标头与原始静态 libc++15004/ABI1/__n1 配对；交叉构建与真实目标执行分别记录，正式放行要求后者通过 |

1.3.0 已纳入固定的 1.2.6。开发主线采用 `main`，`master` 保留旧客户端更新入口；两处使用相同的签名清单与密钥环，1.3.0 客户端从 `main` 获取更新。正式包由同一干净源码提交构建，应用与 SDK 独立分发。

## 复制与无文件保证

| 入口 | 实际边界 |
| --- | --- |
| `awj_frame_create` | 按值复制描述符，不复制像素；来源内存与 ICC 保持到最后一个 frame release |
| FFmpeg 引用计数软件帧 | 使用来源库的 `av_buffer_ref/unref` 保持像素地址，不克隆 AVFrame、字典或未知 side data；ICC 可复制 |
| FFmpeg 非引用计数帧 | strict 拒绝；兼容模式按有效裁切范围逐行复制，并报告 `input_copy_bytes` |
| 共享内存 | 生产者直接写槽位，消费者借用映射；已有私有帧搬入槽位仍然是生产者侧复制 |
| 已可读 staging 纹理 | Map 后可直接借用，来源不能改写/复用；NV12/P010 等后续规范化仍可能需要转换 |
| 普通 GPU/WGC 纹理 | 软件编码器需 Copy/readback；返回帧借用有界 staging 的 RowPitch，不再额外紧凑化，除非目标编码器需要 |
| stdin/普通 OS 管道 | 复用接收缓冲，不创建输入临时文件；OS/接收复制不能消除，不称为零复制 |

`AWJ_COPY_REQUIRE_BORROWED` 拒绝已发生搬运的帧，以及目标入口必须执行的负 stride 重排、BGRA 交换、半平面拆分、JXL 紧凑化和非居中色度相位规范化。编码本身所需的色彩/位深转换和第三方编码器工作存储不能据此称为端到端零复制。

诊断分别提供 `input_copy_bytes`、`conversion_bytes`、`gpu_copy_bytes`、`gpu_readback_bytes`。`encoder_internal_copies_known=0` 表示编码器内部复制未知；`conversion_bytes_known=0` 时，转换量只是已观测下界，0 不代表没有转换。共享槽位前的生产者复制不在消费者的可观测范围。

默认内存 API 不扫描来源文件、不创建 raw/PNG 输入快照、候选文件、日志或报告，编码结果由 `awj_result` 持有。显式文件 sink 才写最终输出；应用的最终文件事务暂存与输入中间文件是不同边界。不能保证 OS 换页、驱动缓存和转储绝不写物理磁盘。

## 构建与安装

Windows 可运行 `scripts/build-sdk.ps1`；默认构建独立 SDK，不构建 GUI/Slint、更新器、libarchive/sodium。依赖、下载、安装缓存、临时文件和构建树都位于当前 worktree 的 `build/`，不复用 1.2.6 的可变构建树，不修改系统工具链。

```bash
pwsh -NoProfile -File scripts/build-sdk.ps1 -BuildDirectory build/sdk-native-msvc
```

脚本通过 Visual Studio 定位工具初始化当前进程的 x64 MSVC 环境；可用 `CompilerDirectory` 显式指定可信 MSVC 工具目录，不接受 clang-cl 替代。应用与 SDK 保留 Release `/O2 /Ob2 /Oi /Ot /Gy /Gw /GF` 和 `/GL`、`/LTCG`、链接裁剪；应用最低 AVX2，独立 SDK 保留 x64 基线与编解码器运行时 SIMD 分派。生成器目标 profile、编译器/链接器摘要与本轮原始命令共同保留。`-CoreOnly` 只构建帧、队列、协议和所选平台适配器，`awj_encode` 返回 UNSUPPORTED，不能将其称为完整编码 SDK。

可选配置：

- `AWJ_SDK_NATIVE_CODECS=ON`：项目锁定的五种编码器。
- `AWJ_SDK_D3D11`：仅 Windows；`AWJ_SDK_SHM`：Windows/Linux，分别选择原生实现，不表示 Linux GPU 或 HarmonyOS 已验证。
- `AWJ_SDK_FFMPEG=ON`、`AWJ_SDK_AVUTIL_INCLUDE_DIR`：编译可选 AVFrame 桥接，**不查找或链接 avutil 导入库**。
- `AWJ_ENABLE_GPU_METRICS=OFF`：CPU 视觉质量，无 FXC/DXC/Vulkan shader 构建依赖。D3D11 导入可独立开启。
- `AWJ_SDK_TOOLS=ON`：构建 `awj-stream`。
- `AWJ_SDK_FFMPEG_TEST_AVUTIL`：仅测试来源动态库的绝对路径，不安装/分发此 DLL 或 `.so`。

Linux 1.3.1 应用与 SDK 使用本项目 WSL 原生文件系统中的 Clang23。当前实际版本为 Ubuntu Clang 23.1.5 时间戳快照，完整 banner 与编译器摘要写入证据，不作为上游稳定版版本声明。桌面完整编码核心保持 C++23 标准设施，配置阶段执行实际编译及最终链接探针；不能仅凭编译器版本推断支持。Linux Release SDK 私有静态链接 C++ 运行库，并精确隐藏非 C API 符号；调用方可独立选择 C/C++ 编译器和标准库。目标系统仍须满足实际 ELF 依赖，本次与已发布 1.3.0 SDK 对照的最高 glibc 需求均为 2.38。部署范围还受共享内存所需内核能力约束，不表示支持任意发行版或 libc。

只安装本 SDK 的组件，避免同时安装第三方依赖的内部包：

```bash
cmake --install build/sdk-native-msvc --prefix build/sdk-install --component AWJimageSDK
```

安装后的 `AWJimage::SDK` 只公开 C 头和动态库/导入库，不要求消费项目的 C++23、私有 modules 或第三方静态 target。当前验证/交付为动态库；没有验证或交付独立静态 SDK，不能把 DLL 的 `.lib` 导入库误称为静态库。

独立消费者见 `examples/basic/`：C99、C++11 和 C++17 均使用同一 C ABI；C++17 示例实际调用全部五种内存编码器。

```bash
cmake -S examples/basic -B build/sdk-consumers -DCMAKE_PREFIX_PATH=<SDK安装目录>
```

```bash
cmake --build build/sdk-consumers --config Release
```

```bash
ctest --test-dir build/sdk-consumers -C Release --output-on-failure
```

CMake `find_package(AWJimage 1.3 CONFIG REQUIRED)`，可要求 `D3D11`、`SHM`、`FFMPEG`、`NATIVE_CODECS` 组件。FFmpeg/HarmonyOS 额外约束不会污染通用帧 ABI。

### HMOS 原生交叉构建

`scripts/build-hmos-sdk.sh` 是 WSL/Linux 原生入口，构建锁定的目标依赖、完整五编码器 SDK 与七个目标测试程序，并安装到全新的项目结果目录。通过 `--native-sdk` 指定已校验的 Native26 目标输入，`--vcpkg-root` 指定原生 vcpkg，`--work-root` 指定当前仓库或包含 `src` 的 AWJ 专属根。`--cmake` 可选择 CMake 3.30+，`--jobs` 默认 1。此入口不执行 ARM64 目标程序。

```bash
bash scripts/build-hmos-sdk.sh --work-root "$HOME/AWJimage" --native-sdk "$AWJ_OHOS_NATIVE_ROOT" --vcpkg-root "$VCPKG_ROOT"
```

若 Native SDK 安装在 Windows，先仅复制本 profile 使用的 metadata、sysroot/usr/include、sysroot/usr/lib/aarch64-linux-ohos、llvm/include/libcxx-ohos/include/c++/v1、llvm/lib/aarch64-linux-ohos 下的 libc++_static.a 与 libunwind.a，以及 llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos 下的 builtins 与两个 CRT 对象；复制前后逐文件核对 SHA-256。不要从 Windows 挂载目录编译，也不随 SDK 分发整套华为 Native SDK。

`cmake/prepare-ohos-clang23-resource.cmake` 校验 26.0.0.105/API26 与 15004/ABI1/__n1，在项目私有目录组合 Clang23 自带头和原始 Native CRT/builtins；只适配驱动要求的 CRT 文件名，不修改原始安装或升级运行库。通用 `llvm/include/c++/v1` 的 __1 头不能替代目标 libcxx-ohos 头。

OHOS 私有兼容层使用固定 tl::expected 与 fmt，补齐实际使用的 expected、宽字符 UTC 格式化、有限 ranges 算法、共享轮询取消状态及 join 型 worker。桌面保留标准实现；错误赋值先拥有目标 string 类型，worker 的内存失败记录不二次分配，全部 join 后才翻译错误。私有依赖不安装为公共 C++ 接口，许可及固定源码摘要随源码保留。

HMOS 安装仅提供 awj.h、真正的 libawj_sdk.so、CMake package、示例和许可；保持 C ABI v1，CPU 编码覆盖 PNG/JPEGli/WebP/AVIF/JXL。D3D11、桌面 SHM、GPU 指标、FFmpeg 桥接及流 CLI 在本 profile 关闭。ArkTS Node-API、PixelMap/NativeBuffer 和 HAP 收集加载另行验证，不能由 `.so` 链接成功推断。

正式 HMOS 包要求真实华为目标或匹配官方运行环境的消费者、完整 SDK 测试及加载证据，并绑定本次库、源码、系统 build 和原始日志。Linux 宿主测试、普通 Linux QEMU 和交叉链接都不能替代；缺少目标执行证据时保留本地候选，阻止正式发布。

## 公共 C ABI 与生命周期

公共头：`awj.h`、可选 `awj_d3d11.h`、`awj_shm.h`、`awj_ffmpeg.h`。Windows 是默认对齐的 x64 C ABI、`__cdecl`；不要以不同 packing 编译结构。结构包含固定宽度整数、指针/长度、size/version 和 reserved；ABI 1 当前要求准确结构大小，reserved 为 0，不接受随意的前缀截断。原生结构不作为网络/wire 布局。

- 使用 `*_init`，再覆盖字段。frame layout/diagnostics 等输出结构先设置 size/version。
- 状态码不跨边界传异常；`awj_last_error()` 是当前线程 UTF-8 缓冲，保存到本线程下一次可能失败的调用之前。
- 所有分配在分配模块释放。调用方不得 `free/delete` result/frame/context，也不得先卸载 SDK 再释放对象。
- `awj_frame_create` 只复制描述符，owner retain/release 必须同时提供或同时省略；成功 retain 一次，最后一次 release 恰好归还一次。owner 必须涵盖像素和 ICC 的真实来源。
- owner 为空时，调用方保证全部来源存储在 frame 最后释放前有效且不变。适配器与传输返回的 SDK storage 会自行保活。
- `release` 可在最后一个引用释放的线程执行，不持内部队列/pool 锁。回调不得抛异常，不得销毁当前正在调用的 context/channel/reader，或重入销毁仍在使用的同一对象。
- `awj_encode` 同步完成，但 context 可有多个受预算准入的调用；调用方保持帧不可变，不并发销毁句柄。
- `awj_context_cancel` 永久取消此 context。destroy 前需等待所有使用该 context 的 API 完成；已导入/借出的 frame 可以晚于 context 释放。
- result 数据指针有效到 result release；来源 frame 可以先释放。
- 队列有固定容量，只接受带真实所有权回调或 SDK storage 的帧。push 成功才增加引用；pop 移交该引用，close 可 drain，destroy 前必须 join 全部队列调用。队列取消通过 close，不与 context 自动联动。
- context 的帧上限、传输槽位、pool 和编码准入均有界；预算是 SDK 上限/工作集准入，不是对第三方分配或进程总 RSS 的硬沙箱。

## CPU 帧与色彩

`awj_plane.data` 为分配区起点，`size` 是真实可读范围，`offset` 为首行位置，`stride` 为有符号行距。支持 padding、负 stride、不同平面地址；只访问有效行字节，不要求最后一行之后的 padding 存在。SDK 能检查描述范围和整数溢出，不能探测任意地址是否真实可读。

支持 RGBA8/BGRA8/RGB8/gray8、RGBA16/gray16、RGBA FP16、8/10/12/16-bit planar YUV420/422/444、NV12、P010。多字节 CPU 样本为小端；P010 的有效 10 位在 word 高位，普通 10-bit planar 样本在低位。使用 `awj_frame_layout` 获取奇数尺寸的准确平面行宽/行数，不自行用 floor 除二。

- 色彩使用 CICP primaries/transfer/matrix、range（0 limited、1 full、-1 未指定）、chroma location、alpha、ICC 与线性参考白。
- 编码必须明确源 CICP/range 或提供 ICC；YUV 仍必须提供 matrix/range，ICC 不能替代 YUV 采样矩阵。
- FP16 编码必须明确 transfer=8（linear）和正参考白亮度。WGC/scRGB 为 BT.709、linear、RGB、full、80 nits；不能对所有 FP16 猜测这一语义。
- 非居中 subsampled chroma 按 AVChromaLocation 1..6 显式重采样到规范化 RGB，再编码；strict 模式拒绝该转换。未指定的色度相位沿用原文件管线的居中约定，不声称从裸流恢复了丢失信息。
- FFmpeg crop_left/top 的奇数子采样相位裁切当前明确拒绝，不静默取整。硬件帧的左/上裁切也暂不支持。
- 缺失元数据、未知格式/硬件后端、越界、预算不足均明确返回状态，不隐式落盘或下载到未知路径。
- `lossless=1` 是编码器模式，不保证色彩/采样转换无损；JPEGli 不提供像素无损，明确拒绝，不以 q100 冒充无损。

最小 C 用例见 `examples/basic/basic.c`，包含创建 context、借用像素、内存 PNG 编码和按分配方释放。用 `awj_frame_row` 可以核对导入地址身份；不能以执行耗时推断零复制。

## 自动 SDR/HDR 编码

`awj_encode` 与文件转换共用呈现策略。C ABI v1 输入是一帧像素，没有第二呈现／Gain Map 输入字段，因此外部软件已重建后提交的 HDR 帧仍视为当前单层，不凭来源猜测缺失的 SDR。

| 单帧输入 | PNG / JXL / AVIF | JPEGli | WebP |
| --- | --- | --- | --- |
| SDR | SDR 单层，不增加 Gain Map | 普通 SDR JPEG | SDR 单层 |
| 有明确颜色解释的 HDR | HDR 单层，不增加 Gain Map | 自动高光滚降生成 SDR 底图，再用 ISO Gain Map 保留 HDR | 自动高光滚降为 SDR 单层 |

高光滚降复用 libplacebo spline tone mapping 与 perceptual gamut mapping，不以截断高光或改标签替代像素处理。JPEG 的 SDR 底图不单独宣称保留完整 HDR；HDR 由增益图重建。颜色／位深转换仍可能分配工作存储，不改变复制诊断与 owner 生命周期合同。格式只支持 SDR 不构成自动转换的失败理由；无效颜色、损坏输入、明确不支持的 lossless/visual-quality 组合、预算和取消仍按既有状态码报告。

完整文件转换可保留原有真实 SDR/HDR 两端：JXL/AVIF 使用 HDR 主图与重建原 SDR 的 Gain Map，JPEG 复用原 SDR 底图与 HDR Gain Map。没有不同动态范围端点时不增加双呈现，JPEG 的单 HDR 例外除外。详细规则见 [Gain Map 与自动呈现](gain-map-animation.md)。

## WGC / D3D11

调用方在持有有效 WGC capture frame 时提交其 texture、ContentSize 及 owner。**单纯 AddRef 底层 texture 不保证捕获内容不会被 frame pool 重用。** resize/recreate/device lost 后，不得把旧代资源继续当新内容使用。

`awj_d3d11_import` 接受同进程纹理，或 `device + shared_nt_handle`。共享资源必须由来源合法创建并通过控制通道传递/复制 NT 句柄；普通 WGC texture 不会自动变为跨进程共享纹理。裸整数在另一个进程中不是有效 HANDLE。

来源明确完成同步时设置 `AWJ_D3D11_SOURCE_READY`；否则使用 keyed mutex 与 acquire/release key。timeout 约束 keyed mutex 获取，不保证强制打断驱动 Map/Copy。提交 copy 后，即使取消也必须等来源不再被 GPU 使用，才能归还 WGC capture frame。

context 必须开启 D3D 多线程保护，或调用方用 `AWJ_D3D11_CALLER_SERIALIZED` 保证所有外部/SDK D3D 调用（包括最后一个 frame release 中的 Unmap）串行化。SDK 不改变外部设备全局线程保护设置，拒绝 SINGLETHREADED device。

普通 GPU texture 使用最多 3 个可复用 staging 槽位，返回帧持有 Map，释放后才复用。有效区域只采用左上角 width/height，半平面按 2x2 单元复制必要样本，不将未定义边缘当有效像素。直接可读 staging 在借用期间保持来源 owner；普通 copy/readback 在 Map 完成后即可归还来源。

## FFmpeg 桥接

编译时必须使用与来源动态库（Windows DLL／Linux `.so`）**精确版本匹配**的 libavutil 公共头。当前验证使用固定提交 `d8990137eec96e8ead292f5de2fc2c6de450e676`，libavutil 61.18.100。可查询 `awj_ffmpeg_expected_avutil_version()`；不同版本的 `AVFrame` 不能跨此适配器使用。

`awj_ffmpeg_api` 由调用方提供 `avutil_version`、`av_buffer_ref`、`av_buffer_unref` 三个 C 函数指针和可选 module_owner。必须来自同一匹配的 libavutil。SDK 不加载、不硬链接、不分发 FFmpeg；调用方从可信绝对路径加载并保持运行库直到最后一次 buffer_unref 返回。

只引用像素 buffers 与必要的 hw_frames_ctx，不读取/转移 `private_ref`，不克隆未知字典或 side data。支持常见 CPU 像素、linesize、crop、CICP/range、alpha、ICC、pts/time_base；有有效 pts 却无 time_base 时必须显式补入。导入成功后可释放原 AVFrame，但在 SDK 引用存在时不能绕过 FFmpeg 可写性契约继续写同一像素存储。

D3D11 硬件帧走平台适配器，保持 AVBufferRef 到资源使用结束；其他未实现的硬件格式返回 UNSUPPORTED，不把 opaque `data` 当 CPU 指针或静默调用硬件下载。

验证用的用户 FFmpeg 是 `--enable-gpl --enable-nonfree` 运行库，**不在 SDK 开发包中再分发**。普通原版 FFmpeg CLI 也不会自动采用本 SDK 的共享槽位协议。

## 共享内存

`awj_shm_create/open` 是当前用户范围的 Windows/Linux 本机 SPSC 通道；名称限制 1..64 个 ASCII 字母/数字/`-`/`_`，建议通过可信控制通道传递不可预测随机名称。Windows 对象 ACL 限制当前用户与 SYSTEM；Linux 用 `SO_PEERCRED` 限制相同 effective UID。均不开放网络服务，也不提供针对恶意同权限进程的完整防篡改隔离。

生产者 `begin_write` -> 直接写槽位紧凑平面与尾部 ICC -> `commit`；消费者 `receive` -> 持有 frame -> `frame_release`。WRITING/READY/READING 都占槽位；不得在 commit 后继续写或在消费完成前复用。满队列返回 BUSY/超时，拒绝无界扩容。token/代次防止迟到提交/归还误用。

通道使用显式 little-endian 元数据，不把 `std::atomic` 或 C++ 对象布局当公共跨进程 ABI。Windows 使用 Win32 mutex/events、进程句柄和创建时间；Linux 使用预留 backing 的匿名 `memfd`、大小 seals、`SCM_RIGHTS`、process-shared robust pthread mutex、共享 futex 与内核提供的 `pidfd`，不创建命名 shm／业务文件节点，也不把复用的 PID 当原进程。Linux 两端须处于相同本机 IPC／网络命名空间，使用兼容的 pthread 布局；需要 `SO_PEERPIDFD`、`memfd` seals 和 `pthread_mutex_clocklock` 支持，不具备时明确失败，不退回磁盘文件。通道绑定 create/open 的进程，不得在 fork 后使用继承句柄，应 exec 后重建。

生产者或消费者异常退出会使通道进入不可恢复 I/O 错误，不偷回仍借出的槽位。已经借出的 frame 保持映射有效，channel/context 可先释放。Linux 仅用 socket 传小型控制消息与 fd，不传像素；消费者预算等验证通过并 ACK 后才占用唯一连接，失败连接不永久抢占消费者身份。FIFO 按 begin 的 token 排序，未提交的早期槽位会阻塞后续 READY，直到提交、abort 或 close。

正常 close 唤醒并允许 drain 已提交帧；EOF 表示关闭且无待取帧。mutex abandoned／owner-dead、损坏头或状态无效均失败。归还槽位的锁等待有界，失败后当前映射标记不可继续使用，不能把这种失败当已成功归还。共享内存 API 的 timeout 为 0..60000 毫秒；0 不阻塞，返回 BUSY，正超时返回 TIMEOUT。同名对象在最后一个拥有者销毁后才可重建；Linux 不留下 abstract socket／命名 shm 文件清理负担。

## 二进制帧协议与流工具

`awj-stream` 仅包含公共 C 头，通过 C ABI 调用 SDK，不依赖项目私有 modules。默认输入 `AWJFRM1`，输出 `AWJRES1`；`--raw` 接收紧凑 rawvideo，必须显式提供格式、尺寸与 `--color`。stdin/stdout 是二进制，所有说明/日志写 stderr；没有文件输出参数，需要时由调用方显式重定向最终输出。

输入等待为每帧 60 秒，短读与任意碎片正常处理；截断失败后不继续解释剩余字节。CLI 的 stdout 写入采用平台同步 I/O，不能承诺对不配合消费者的写阻塞强制超时；应用内调用可提供符合期限契约的 I/O 回调。Ctrl+C 会请求 context 取消，不能强制中断第三方驱动/编码器的不合作阻塞。

`--single` 只接受单帧输入，输出原始编码码流，不能用于无分帧拼接多张图。默认多帧模式允许帧描述变化，通过结果长度界定码流，不把任意编码格式无边界拼接。

示例（以下命令需要在支持二进制 native 管道的 shell 中执行；不能让旧文本管道改写字节）：

```bash
ffmpeg -f lavfi -i testsrc=size=640x360:rate=1 -frames:v 2 -pix_fmt rgba -f rawvideo pipe:1 | awj-stream --raw rgba8 640 360 --color 1 13 0 1 --codec png --lossless > frames.awjres
```

已知单帧 WGC FP16 原始流可用 `--raw rgba16f W H --color 1 8 0 1 --reference-white 80 --alpha straight --single`。原有 `AWJ --stdin-wgc-rgba16f WxH -o <目录>` 语法仍在：直接在接收缓冲内编码，不再创建临时 `.awsraw`；它复用与 SDK 相同的 `NativeBackend::encode_memory` 核心并保留原应用参数决策，只有显式最终文件 sink 写盘。该内存分支不生成文件日志/summary，也不提供“原输入文件”的 SHA-256 命名模板。

### AWJFRM1 输入

固定 160 字节头，所有整数 little-endian，float 为 IEEE-754 位模式，时间戳为 int64 位模式；不是直接发送 C 结构。

| offset | 字段 |
| --- | --- |
| 0 | `AWJFRM1\0`，8 字节 |
| 8 / 12 | version=1 / header_size=160，u32 |
| 16 / 20 / 24 / 28 | width / height / format / bit_depth，u32 |
| 32 / 36 | plane_count / alpha，u32 |
| 40 / 48 | frame_id u64 / timestamp i64 |
| 56 / 60 | time_base_num / time_base_den，u32 |
| 64..80 | primaries / transfer / matrix / range / chroma_location，5 个 i32 |
| 84 | linear_reference_white_nits，float |
| 88 / 96 | icc_size / payload_size，u64 |
| 104..135 | row_bytes[4]，u64 |
| 136..151 | rows[4]，u32 |
| 152 | reserved=0，u64 |

payload 是各平面逐行的有效像素字节，再附 ICC，不传 padding。尺寸/格式派生的平面布局必须与头一致，接收器校验长度、溢出和预算。reader 保留固定槽位，全部借出时返回 BUSY；frame 可比 reader/context 活得更久。

### AWJRES1 输出

固定 40 字节头：0=`AWJRES1\0`，8=version u32(1)，12=header_size u32(40)，16=frame_id u64，24=codec u32，28=reserved u32(0)，32=encoded_size u64；随后紧跟对应长度的完整编码结果。失败走非零退出码/stderr，不输出伪造成功结果。

## 验证、证据与交付

`awj_sdk_validation` 只构建 SDK 和受影响的定向检查，不运行更新器/UI 全量回归。测试覆盖公共 C/C++ 消费、布局/owner/队列/协议、真实跨进程 SHM 和异常退出、WARP、真实 libavutil、五种编码器内存回读、只读页、负 stride、常见 RGB/YUV、FP16 和 CPU 视觉质量。

Windows 的 `tests/sdk_stream_cli_tests.ps1` 用真实 FFmpeg 子进程、内存 stdin/stdout 验证多帧结果、原生单帧码流、截断和旧 WGC 分支。Linux 配置 `AWJ_SDK_FFMPEG_TEST_CLI` 后注册 `sdk_stream_cli`，用原生 FFmpeg 及二进制校验器覆盖多帧、单帧 PNG 和截断拒绝。检查测试工作目录不产生输入中间文件，但这不等于系统级全进程文件 I/O 跟踪。

安装后独立 C99/C++11 项目消费；`scripts/verify-sdk-exports.ps1` 从实际安装公共头生成精确白名单并核对 DLL，Linux 对应 `scripts/verify-sdk-exports.cmake` 核对 ELF，不能只检查 `awj_` 前缀。构建证据记录源码指纹/每文件哈希、工具链、配置、原始日志、测试与产物 SHA-256；不把先前测试当本次重跑。

正式 SDK 独立提供 `AWJ_SDK_Win_x64.zip` 与 `AWJ_SDK_Linux_x64.tar.gz`，同一 Release 附对应源码和 `SHA256SUMS`。`SDK-BUILD.json` 记录版本、C ABI、源码提交、归一化指纹与编译器；`scripts/package-sdk-release.cmake` 校验干净 tag、实际库和测试记录，并在归档回解位置执行精确导出、C99/C++11 消费者及流工具验证。Linux 流工具使用相对安装 RPATH 查找包内 SDK。

历史开发交付保留在各工作根的 `bin/1.3.0/dev/<构建标识>/`，Windows 汇总目录仍保留其原始证据。复制记录不等于在新路径重新验证；真实 HarmonyOS、WGC/物理 HDR GPU 等范围按平台表和对应验收记录分别说明。
