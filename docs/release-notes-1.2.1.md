# AWJimage 1.2.1

本次更新完善 Windows 命令行独立运行、线程与内存控制，并调整界面布局和主题。

## 优化

- 统一目标像素转换阶段：解码器优先保留原生 YUV，兼容布局直接复用；色彩矩阵、位深、采样、HDR 和几何调整合并处理，减少整图中间缓冲。保留必要的 HDR 亮度分析、Gain Map 重建和动画合成。
- Windows 转换核心独立放入 `AWJ.com`，可单独使用；`AWJ.exe` 负责界面、右键菜单和更新，转换任务交给同目录的 `AWJ.com`。
- 自动线程按允许的逻辑 CPU 数预留桌面余量；明确指定 `--threads N` 时使用完整线程预算。新增 `--cpu-affinity`，支持指定逻辑 CPU 编号和范围。
- 普通大图直接进入编码队列，Grid 单独调度。完善大图内存估算，减少 HDR 转换和 JPG 编码的中间缓冲。
- 格式名称统一为 JPG，默认输出 8-bit YCbCr 4:2:0，可选择 4:2:2、4:4:4，保留 HDR Gain Map 和旧参数兼容。
- CPU 视觉质量搜索复用参考图和候选缓冲；DX11、Vulkan 和 WIC 按需初始化，缺少可选图形功能时使用 CPU 指标和软件界面渲染。

## 界面

- 侧栏缩窄，标题改为居中的 AWJ；修正命名按钮、下拉框、复选框排列及设置页留白。
- 统一侧栏与标题栏配色，恢复分组卡片；修正滚动区域圆角、边框和连续缩放时的绘制。
- 更新日志选项合并为默认勾选的“仅在更新后显示更新日志”，与日志页同步；更新按钮和状态提示保持同一行。
- 更换深浅主题图标；关于页提供 GitHub 仓库和 Q 群链接。

## 使用说明

- Windows x64：Windows 10 或更新版本，CPU 支持 AVX2。使用界面和右键转换时，将 `AWJ.exe` 与 `AWJ.com` 放在同一目录。
- Linux x64：CPU 支持 x86-64-v3，glibc 2.39 或更新版本；安装 fontconfig、FreeType，桌面使用 X11 或 Wayland。
- 解压后保留 `LICENSE` 和 `NOTICE.txt`。1.0.12 及更早版本请手动下载升级。

使用方法见 [README](https://github.com/Dominic485649/AWJimage/blob/master/README.md)，完整记录见 [CHANGELOG](https://github.com/Dominic485649/AWJimage/blob/master/CHANGELOG.md)。
