# AWJimage

[English](README.en.md)

AWJimage 是支持 Windows 和 Linux 的批量图片转换工具，提供图形界面和命令行。当前源码版本：**1.2.3**。

## 格式支持

| 用途 | 格式 |
|---|---|
| 输入 | AVIF、HEIC/HEIF、JPEG、PNG、WebP、JXL、BMP、GIF、TIFF、相机 RAW；Windows 另支持 JXR |
| 输出 | AVIF、PNG、WebP、JXL、JPG |
| 动画 | GIF、APNG、WebP、AVIF 转 AVIF 时保留帧时序、循环和透明度 |
| HDR | 支持 HDR 图像和受支持的 Gain Map；JPEG 输出可生成 HDR Gain Map |

默认输出 AVIF，质量 70、速度 5。JPG 默认输出 8-bit YCbCr，auto 与 AVIF 一样跟随源采样：保留已知 4:2:0 / 4:2:2 / 4:4:4，RGB、RGBA 和未知源使用 4:4:4；JPG 灰度源在 auto 下使用三分量 4:4:4，不支持显式 4:0:0。HDR JPG 使用 SDR 主图与 ISO 21496-1 Gain Map，仅接受 ISO 增益图协议，不再兼容旧 Adobe／Google／Apple 增益图。质量 100 的无损行为取决于格式；JPEG 支持无损转封装为 JXL。[Gain Map 与动画说明](docs/gain-map-animation.md)。

各格式使用统一的目标像素转换阶段。兼容的 YUV 布局直接复用，色彩矩阵、位深、采样及几何调整合并处理，减少整图中间缓冲。HDR 亮度分析、Gain Map 重建和动画合成按格式要求执行。

## 下载与系统要求

从 [GitHub Releases](https://github.com/Dominic485649/AWJimage/releases) 下载已发布版本。

- Windows x64：Windows 10 或更新版本，CPU 支持 AVX2。运行 `AWJ.exe` 打开界面，`AWJ.com` 用于命令行；`AWJ.com` 可独立转换；界面和右键转换需要两个程序位于同一目录。
- Linux x64：CPU 支持 x86-64-v3，glibc 2.39 或更新版本，并安装 fontconfig、FreeType；桌面使用 X11 或 Wayland。运行 `./AWJ` 打开界面。
- 普通转换使用 CPU；图形加速不可用时使用 CPU 指标和软件界面渲染。
- 解压后保留程序旁的 `LICENSE` 和 `NOTICE.txt`。1.0.12 及更早版本请手动下载升级。

## 快速使用

将图片或文件夹拖入编码队列，选择输出目录和格式，点击“开始转换”。参数设置可保存为预设。Windows 可在参数页安装资源管理器右键菜单；Linux 支持 Nautilus Scripts 和 Thunar UCA。

配置保存在程序旁的 `AWJ.jsonc`，用户预设放在 `preset/*.jsonc`。

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

## 构建

构建依赖和平台命令见 [贡献指南](CONTRIBUTING.md)。发布包生成流程见 [发布指南](docs/release.md)。

## 许可证与交流

项目使用 [AGPL-3.0 许可证](LICENSE)，第三方组件许可证见 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) 和发行包中的 `NOTICE.txt`。

[GitHub 仓库与问题反馈](https://github.com/Dominic485649/AWJimage) · [Q 群交流](https://qm.qq.com/q/Wt4Ssh1igi)
