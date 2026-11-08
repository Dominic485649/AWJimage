# AWJimage

[English](README.en.md)

AWJimage 是支持 Windows 和 Linux 的批量图片转换工具，提供图形界面和命令行。当前开发版本：**1.3.5**（本地候选），包含应用与 C ABI SDK。

## 格式支持

| 用途 | 格式 |
|---|---|
| 输入 | AVIF、HEIC/HEIF、JPEG、PNG、WebP、JXL、BMP、GIF、TIFF、相机 RAW；Windows 另支持 JXR |
| 输出 | AVIF、PNG、WebP、JXL、JPG |
| 动画 | GIF、APNG、WebP、AVIF 转 AVIF 或 WebP 时保留动画时序、循环和透明度；单帧使用静态编码 |
| HDR | 单层 SDR/HDR 保持单层；真实双呈现可保留 Gain Map；JPG HDR 可自动生成 SDR 底图，SDR-only 输出自动高光滚降 |

默认输出 AVIF，质量 70、速度 5。JPG 默认输出 8-bit YCbCr，auto 与 AVIF 一样跟随源采样：保留已知 4:2:0 / 4:2:2 / 4:4:4，RGB、RGBA 和未知源使用 4:4:4；JPG 灰度源在 auto 下使用三分量 4:4:4，不支持显式 4:0:0。HDR JPG 使用 SDR 主图与 ISO 21496-1 Gain Map，仅接受 ISO 增益图协议，不再兼容旧 Adobe／Google／Apple 增益图。质量 100 的无损行为取决于格式；JPEG 支持无损转封装为 JXL。[Gain Map 与动画说明](docs/gain-map-animation.md)。

SDR 输入不因宽色域、高位深或显示提亮而增加 HDR 层。JXL/AVIF 仅为原有 SDR/HDR 双呈现保留 HDR 底图与重建 SDR 的 Gain Map；普通单层仍为单层。JPG 是例外：HDR 没有现成 SDR 时自动高光滚降生成 SDR 底图，再用 Gain Map 保留 HDR。WebP 自动滚降至 SDR，有原 SDR 时优先复用；PNG 保留所选 HDR/SDR 单层。`--jpeg-hdr hdr` 遇到 SDR 输入也保持普通 SDR，不重标为 HDR。

JXL 自动位深按所选呈现的有效精度选择 10/12/16-bit，16-bit 源保留 16-bit，显式 8/10/12/16 优先。有损主图使用 XYB，无损主图与 Gain Map 数值图保留原模式。质量 100 表示目标位深的主图无损；先降到 10/12-bit 再无损编码不等于原始 16-bit 逐样本无损，重新生成的双呈现也不声明整对逐样本无损。JPEG 原始码流可逆转封装仅限显式 8-bit；自动位深走像素编码。

各格式使用统一的目标像素转换阶段，真实尺寸重采样统一使用 Spline36。尺寸可选自动、无限制、手动放大、手动缩小和固定长／宽；放大／缩小分别取已填宽、高、长边、短边和比例算出的最大／最小倍率，比例可为正小数或超过 100%。固定模式在尺寸与比例之间二选一：填写尺寸后比例框禁用，等比居中放入指定画布，支持 alpha 时补透明边，否则补黑边；只填比例时禁用四个尺寸框并按比例等比缩放。HDR 亮度分析、Gain Map 重建和动画合成按格式要求执行。

## 下载与系统要求

从 [GitHub Releases](https://github.com/Dominic485649/AWJimage/releases) 下载已发布版本。

- Windows x64：Windows 10 或更新版本，CPU 支持 AVX2。运行 `AWJ.exe` 打开界面，`AWJ.com` 用于命令行；`AWJ.com` 可独立转换；界面和右键转换需要两个程序位于同一目录。
- Linux x64：CPU 支持 x86-64-v3，glibc 2.39 或更新版本，并安装 fontconfig、FreeType；桌面使用 X11 或 Wayland。运行 `./AWJ` 打开界面。
- 普通转换和视觉质量指标（luma、GMSD、MS-SSIM）统一使用 CPU；界面渲染仍支持图形加速与软件渲染。
- 解压后保留程序旁的 `LICENSE` 和 `NOTICE.txt`。1.0.12 及更早版本请手动下载升级。

## 快速使用

将图片或文件夹拖入编码队列，选择输出目录和格式，点击“开始转换”。参数设置可保存为预设。Windows 可在参数页安装资源管理器右键菜单；Linux 支持 Nautilus Scripts 和 Thunar UCA。

队列显示源文件大小与成功转换后的输出大小。拖动表头分隔线只调整左侧列，列宽不会随窗口缩放；超出可视区域时可用底部滚动条左右查看。右键表头可勾选显示的列，列宽与显隐状态在当前会话保留。

完成的图片可以再次导入，无需清空队列；未重新导入的完成项保留，新图和重新导入项变为等待编码。按规范化路径去重，并以最后一次有效导入的位置追加，不改变现有输出冲突策略。编码或取消收尾期间仍禁止导入。

配置保存在程序旁的 `AWJ.jsonc`，用户预设放在 `preset/*.jsonc`。队列预设初始使用内置默认；选择用户预设后，通过 `queue_preset_file` 记忆预设文件名并在下次启动恢复。切回内置默认会清除该选择，预设被删除或失效时也回退内置默认。

## 命令行

Windows 使用 `AWJ.com`；Linux 将下列命令中的程序名换为 `./AWJ`。

```powershell
# 批量转换，使用默认 AVIF 参数
.\AWJ.com -i .\images -o .\output
# 指定格式、质量和速度
.\AWJ.com -i .\image.png -o .\output --format webp -q 90 --speed 5
# 指定线程预算与逻辑 CPU
.\AWJ.com -i .\images -o .\output --threads 10 --cpu-affinity 0-4,9-13
# 查看参数和预设
.\AWJ.com --help
.\AWJ.com --list-presets
```

`--threads auto` 按允许的逻辑 CPU 数预留桌面余量；`--threads N` 使用 N 的总编码线程预算（1–128），不扣除预留。文件并发与每图编码线程在该预算内分配，并受内存预算约束。`--cpu-affinity` 使用从 0 开始的逻辑 CPU 编号，只能缩小进程当前允许集合；Windows 编号位于当前处理器组。

AVIF 静态图可通过 `--avif-progressive` 使用自动两层渐进，或在 CLI 配置 2～4 层的分辨率与质量；默认关闭，暂不增加 UI 控件。详见 [渐进式与分层参数](docs/avif-progressive.md)。

内存计划按解码、准备、编码、指标与输出阶段估算，并在解码后按真实像素、透明度和采样精化。有限预算内可独占精化不确定输入，不改变自动预算或默认解除限制；CSV 保留初值、精化值和预约诊断。预约不是第三方编码器内存的硬上限。

## 队列日志与源文件

“写入运行日志”同时生成输出目录中的 `log/awj.log` 与 `log/summery.csv`，不再单独设置 CSV 报告。CLI 的 `--log` 与兼容别名 `--summary` 同样开启两者，`--no-log`／`--no-summary` 关闭两者，最后一次开关生效。

重复处理默认“追加序号”，已有同名输出时使用 `name(1).ext`、`name(2).ext`，也可显式选择覆盖、跳过、追加时间或随机值。队列图片编码前双击打开源文件，编码成功后打开实际输出文件。

“删除源文件”默认关闭；开启后只在该文件编码成功且输出落盘后立即尝试移入系统回收站，失败、取消或跳过时保留源文件。回收站不可用或操作失败时保留源文件并提示，不降级为永久删除；Linux 使用系统 GIO，缺失时同样保留源文件。链接路径、硬链接或其他输入依赖无法安全确认时也会保留并提示。CLI 使用 `--delete-source`／`--no-delete-source` 控制同一行为。

## C ABI SDK

提供 Windows / Linux 动态 SDK，C / C++ 项目通过同一套 C 头接入。支持 CPU 图像帧、共享内存、可选 FFmpeg 帧桥接和 Windows D3D11 纹理，直接返回内存编码结果；附 CMake package、`awj-stream` 和集成示例。Windows SDK 使用 MSVC，Linux SDK 使用 Clang 23，提供 C99 / C++11 / C++17 消费示例。

从同一 Release 下载 `AWJ_SDK_Win_x64.zip` 或 `AWJ_SDK_Linux_x64.tar.gz`。接入契约、复制诊断、色彩和平台要求见 [SDK 文档](docs/sdk.md)。

## 构建

构建依赖和平台命令见 [贡献指南](CONTRIBUTING.md)。发布包生成流程见 [发布指南](docs/release.md)。

## 许可证与交流

项目使用 [AGPL-3.0 许可证](LICENSE)，第三方组件许可证见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) 和发行包中的 `NOTICE.txt`。

[GitHub 仓库与问题反馈](https://github.com/Dominic485649/AWJimage) · [Q 群交流](https://qm.qq.com/q/Wt4Ssh1igi)
