# Contributing / 开发指南

使用 C++23 模块、CMake 3.30+、Rust stable 和 vcpkg。依赖版本由 `vcpkg-configuration.json`、`vcpkg.json` 和 `cmake/codec-dependencies.json` 固定。Windows 使用 MSVC 2026；Linux 使用支持模块的 GCC 16+ 或 Clang 20+、Ninja，以及 autoconf、autoconf-archive、automake、libtool。着色器在构建时由 DXC 生成。

## Windows

在 PowerShell 7 中设置 `VCPKG_ROOT`，使用已初始化 MSVC 环境的开发者终端：

```powershell
cmake --preset windows-msvc-x64-release
cmake --build --preset windows-msvc-x64-release --target AWJ AWJ-com
# 生成带第三方许可证的 Windows Release
.\release.ps1 -SkipUpdateManifest
```

发行配置保留 AVX2、静态 C/C++ 运行时和 IPO/LTO。独立 CLI 不依赖 Slint 或更新模块。

## Linux

在原生 Linux 文件系统编译；WSL 通过拷贝交换源码和产物。

```bash
export VCPKG_ROOT=/path/to/vcpkg
export CC=gcc-16 CXX=g++-16
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release --target AWJ
```

发行配置保留 x86-64-v3 和 IPO/LTO。Vulkan 头文件与 DXC 用于编译；运行时缺少 Vulkan 可回退 CPU。

## 验证 / Validation

在单独的构建目录启用 `BUILD_TESTING`，保留发行 CPU 基线和依赖版本。Windows CTest 串行执行，避免资源管理器菜单测试争用注册表事务锁。

```powershell
cmake -S . -B build/validation -G 'Visual Studio 18 2026' -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DBUILD_TESTING=ON
cmake --build build/validation --config Release --parallel 2
ctest --test-dir build/validation -C Release -j 1 --output-on-failure
```

```bash
cmake --preset linux-x64-release -DBUILD_TESTING=ON
cmake --build --preset linux-x64-release
ctest --test-dir build/linux-x64-release --output-on-failure
```

回归覆盖编码、色彩、HDR、透明度、动画、取消、资源限制、配置与更新安全。界面检查中文/英文、深浅主题、系统标题栏、窄窗口和不同缩放比例。 `scripts/check-window-icons.ps1 -Executable <candidate-dir>\AWJ.exe -WorkDirectory <benchmark-dir>\window-icons` 检查三个应用主题下的原生大小图标、标题栏和正常关闭，默认使用软件渲染；`-Backend auto` 检查自动选择的渲染器。工作目录必须为新目录，脚本不修改系统主题。原生 GUI 检查应在 CTest 完成后执行，避免菜单自动同步与 Shell 注册验证并发。

`scripts/check-window-resize.ps1 -Executable <test-dir>\awj_ui_smoke_tests.exe -OutputDirectory <benchmark-dir>\window-resize` 使用与 AltSnap 相同的窗口缩放调用，检查 GPU/软件渲染及深浅主题下的画面尺寸、边缘和正常关闭。脚本从渲染器回读画面，记录原始帧检查及哈希；实际 Alt+右键手势和 DWM 屏幕合成仍需桌面验收。输出目录必须为新目录。

字体像素回归使用真实 Winit/FemtoVG 窗口，需可用桌面和 OpenGL；默认不注册到无桌面测试。启用后，各缩放比例使用独立进程，对照直接绘制、矩形裁剪、圆角裁剪与嵌套裁剪，并检查贴边色块的内部覆盖与外部边界，不依赖跨机器字体截图基线。

```bash
cmake -S . -B build/validation -DAWJ_ENABLE_NATIVE_RENDERER_TESTS=ON
```

```bash
cmake --build build/validation --config Release --target awj_text_rendering_tests
```

```bash
ctest --test-dir build/validation -C Release -R "^text_rendering_pixels_" -j 1 --output-on-failure
```

可通过进程环境变量 `AWJ_TEXT_TEST_FONT` 指定像素回归的字体。正式 Studio 组件的独立截图入口为测试程序的 `--font-rendering-snapshots <全新目录> <缩放比例>`；它读取 `AWJ_UI_TEST_FONT`，检查导航点击、实际渲染尺寸与缩放，并输出深浅主题 PPM，不加载正式配置或同步资源管理器菜单。上述环境变量仅用于测试程序，不改变应用默认字体或系统设置。

## 基准复现 / Benchmarks

`scripts/prepare-benchmark.py` 使用 Pillow，从来源目录只读拷贝并校验 SHA-256。固定样本为 86 张 JXR、258 张 PixPin 图片及 256 张补充静态图片，共 600 张；数据、输出和原始测量仅保存在指定工作目录，私有路径及原图不提交。

重测须由用户明确启动。所有转换进程树限制在 CPU 0–4 与 9–13（共 10 个逻辑 CPU，Windows Job Object 硬亲和性 `0x3E1F`），在挂起状态下核对限制后恢复执行。

| 分组 | 线程与文件并发 |
|---|---|
| AWJ auto | `--threads auto --cpu-affinity 0-4,9-13`，自动内存，q70/speed5；线程预算 8 |
| AWJ 显式 10 | `--threads 10 --cpu-affinity 0-4,9-13`，自动内存，q70/speed5；线程预算 10 |
| FFmpeg 单线程编码 | 解码与滤镜各 1 线程，每个颜色/alpha编码器 1 线程，12 个文件任务，row-mt 0 |
| FFmpeg 四线程编码 | 解码与滤镜各 1 线程，每个颜色/alpha编码器 4 线程，3 个文件任务，row-mt 1 |

含 alpha 的文件有两个编码器，仍占一个文件并发槽；编码器线程数不等于文件内的全部操作系统线程数，整棵进程树的 CPU 硬上限保持 10。色彩 AOM 使用 CRF23、speed5、all-intra、still-picture、tune=iq；alpha 按 AWJ quality70 的映射使用 QP19/PSNR。透明图片必须封装关联正确、同位深的单色 alpha 辅助层，并核验真实解码透明度；透明 AVIF 输入也必须读取其辅助层。

86 张 JXR 统一为实际 scRGB→BT.2020/PQ 色彩变换及 12-bit YUV444 full range：CICP 9/16/9/full，scRGB 1.0 为 80 nit。滤镜转换与 AVIF/AV1 两处标签分别验证，禁止只改标签或在转浮点前截断 HDR。其他图片保留指定预设的 10-bit YUV 420/422/444 流程，报告仍需披露 Gain Map JPEG 等剩余处理差异，不能仅凭输出标签宣称像素或画质完全等效。

按 AWJ auto → AWJ 显式 10 → FFmpeg 单线程编码 → FFmpeg 四线程编码各完整预热一次，再依序正式测量三轮，组间冷却至少 30 秒。每批须 600/600 成功并通过输出属性和透明度检查；失败即停止，不统计成功子集。预检和输出校验在计时之外，转换计时包含实际解码、滤镜、颜色/alpha 编码和封装；不能隐藏预转图像的成本。测量时不运行构建、打包或其他基准。

记录输入、程序、动态库、预设、最终命令、测量脚本和输出哈希。CPU 使用整棵进程树的用户态与内核态时间；峰值提交内存使用 Job 的 `PeakJobMemoryUsed`，曲线同时刻求和每个进程的私有提交和工作集，分别展示采样峰值，不能相加各进程独立峰值。记录每个 PID 的当前任务及内存、CPU、时间；AWJ 内部在途文件的近似重建须标注其误差。

正式测试后以固定大图进行独立诊断，核对预测工作集和实测提交峰值，覆盖 JXR HDR、透明图、Gain Map JPEG 及超宽图。工作集估计包含 HDR/alpha/编码器余量及压缩输入，数值是调度预留而非分配器硬限。README 不展示测试或基准结果；测量与诊断保存在指定工作目录。MiB 为 2²⁰ 字节。旧测量只保留作诊断，旧 `scripts/benchmark.ps1` 不能直接复现此协议。
