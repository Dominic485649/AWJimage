# AWJimage 1.2.5

本版修复队列菜单动效，并恢复左上角版本号的原有点击行为。

- **统一菜单动效**：表头右键菜单现在与任务右键菜单一样，具有展开动画和选项背景过渡。
- **保留键盘操作**：开场裁剪结束后恢复首项焦点，保留列勾选、Escape 关闭和两类菜单互斥。
- **恢复版本号操作**：有可用更新时下载、验证并安装；没有更新时打开仓库 Releases，不再跳到内置更新日志。
- **保留队列增强**：包含源文件与输出大小对比、独立列宽、列显隐和队列预设记忆。Windows 与 Linux 同步提供，编解码器依赖及配置格式不变。

下载 `AWJ_Win.7z` 或 `AWJ_Linux.7z`，校验值见 `SHA256SUMS`。Windows 请将 AWJ.exe 与 AWJ.com 放在同一目录。

<details>
<summary>English</summary>

This release fixes queue-menu animations and restores the original top-left version action.

- **Consistent menu animations**: the header context menu now opens and transitions its option backgrounds like the task context menu.
- **Keyboard support**: restore initial focus after the opening clip completes while retaining column toggles, Escape dismissal and mutually exclusive menus.
- **Restored version action**: download, verify and install an available update, or open the repository Releases page when no update is available, instead of navigating to the embedded changelog.
- **Queue enhancements retained**: includes source/output size comparisons, independent column widths, column visibility and queue-preset persistence on Windows and Linux. Codec dependency versions and configuration formats are unchanged.

Download `AWJ_Win.7z` or `AWJ_Linux.7z` and use `SHA256SUMS` to check the archives. Keep AWJ.exe and AWJ.com in the same directory on Windows.

</details>
