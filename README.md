# AWJimage

[English](README.en.md)

AWJimage 是支持 Windows 和 Linux 的批量图片转换工具，提供图形界面和命令行。当前版本：**1.3.0**，同时提供应用与 C ABI SDK。

## 格式支持

| 用途 | 格式 |
|---|---|
| 输入 | AVIF、HEIC/HEIF、JPEG、PNG、WebP、JXL、BMP、GIF、TIFF、相机 RAW；Windows 另支持 JXR |
| 输出 | AVIF、PNG、WebP、JXL、JPG |
| 动画 | GIF、APNG、WebP、AVIF 转 AVIF 时保留帧时序、循环和透明度 |
| HDR | 单层 SDR/HDR 保持单层；真实双呈现可保留 Gain Map；JPG HDR 可自动生成 SDR 底图，SDR-only 输出自动高光滚降 |

默认输出 AVIF，质量 70、速度 5。JPG 默认输出 8-bit YCbCr，auto 与 AVIF 一样跟随源采样：保留已知 4:2:0 / 4:2:2 / 4:4:4，RGB、RGBA 和未知源使用 4:4:4；JPG 灰度源在 auto 下使用三分量 4:4:4，不支持显式 4:0:0。HDR JPG 使用 SDR 主图与 ISO 21496-1 Gain Map，仅接受 ISO 增益图协议，不再兼容旧 Adobe／Google／Apple 增益图。质量 100 的无损行为取决于格式；JPEG 支持无损转封装为 JXL。[Gain Map 与动画说明](docs/gain-map-animation.md)。

SDR 输入不因宽色域、高位深或显示提亮而增加 HDR 层。JXL/AVIF 仅为原有 SDR/HDR 双呈现保留 HDR 底图与重建 SDR 的 Gain Map；普通单层仍为单层。JPG 是例外：HDR 没有现成 SDR 时自动高光滚降生成 SDR 底图，再用 Gain Map 保留 HDR。WebP 自动滚降至 SDR，有原 SDR 时优先复用；PNG 保留所选 HDR/SDR 单层。`--jpeg-hdr hdr` 遇到 SDR 输入也保持普通 SDR，不重标为 HDR。

各格式使用统一的目标像素转换阶段。兼容的 YUV 布局直接复用，色彩矩阵、位深、采样及几何调整合并处理，减少整图中间缓冲。HDR 亮度分析、Gain Map 重建和动画合成按格式要求执行。

## 下载与系统要求

从 [GitHub Releases](https://github.com/Dominic485649/AWJimage/releases) 下载已发布版本。

- Windows x64：Windows 10 或更新版本，CPU 支持 AVX2。运行 `AWJ.exe` 打开界面，`AWJ.com` 用于命令行；`AWJ.com` 可独立转换；界面和右键转换需要两个程序位于同一目录。
- Linux x64：CPU 支持 x86-64-v3，glibc 2.39 或更新版本，并安装 fontconfig、FreeType；桌面使用 X11 或 Wayland。运行 `./AWJ` 打开界面。
- 普通转换使用 CPU；图形加速不可用时使用 CPU 指标和软件界面渲染。
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

## C ABI SDK

提供 Windows / Linux 动态 SDK，C / C++ 项目通过同一套 C 头接入。支持 CPU 图像帧、共享内存、可选 FFmpeg 帧桥接和 Windows D3D11 纹理，直接返回内存编码结果；附 CMake package、`awj-stream` 和集成示例。SDK 源码已通过 Clang 21 构建，提供 C99 / C++11 消费示例。

从同一 Release 下载 `AWJ_SDK_Win_x64.zip` 或 `AWJ_SDK_Linux_x64.tar.gz`。接入契约、复制诊断、色彩和平台要求见 [SDK 文档](docs/sdk.md)。

## 构建

构建依赖和平台命令见 [贡献指南](CONTRIBUTING.md)。发布包生成流程见 [发布指南](docs/release.md)。

## 许可证与交流

项目使用 [AGPL-3.0 许可证](LICENSE)，第三方组件许可证见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) 和发行包中的 `NOTICE.txt`。

[GitHub 仓库与问题反馈](https://github.com/Dominic485649/AWJimage) · [Q 群交流](https://qm.qq.com/q/Wt4Ssh1igi)
