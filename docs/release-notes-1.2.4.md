# AWJimage 1.2.4 发布说明草稿

状态：已获用户正式发布授权。本文为 1.2.4 发布文案，实际发布状态以 GitHub Release 为准。

## 中文发布说明

本版增强编码队列的文件信息、表头操作和预设记忆，并修复深色菜单显示问题。

- **大小对比**：明确显示源文件大小，新增输出大小列。成功转换后可直接对比，失败、跳过或尚未生成的任务不显示旧输出大小。
- **独立列宽与横向滚动**：拖动分隔线只调整左侧列，窗口最大化、缩小或切换页面不重新分配列宽。列内容超出可视区域后，使用底部滚动条同步移动表头和正文。
- **自选显示列**：右键表头勾选序号、文件名、目录、源文件大小、输出大小、状态、输出或日志。全部隐藏后仍可恢复。文字左对齐，超长内容右侧省略。
- **菜单与细节修复**：移除列菜单各选项的整行白色焦点框，保留复选框和键盘操作。任务右键菜单正确跟随深浅色主题。表头分隔线使用对齐像素网格的单个物理像素，高 DPI 下仍保持细线；主题配色更柔和，拖动命中区域不缩小。
- **记住队列预设**：初始使用内置默认。选择用户预设后保存到 AWJ.jsonc，下次启动恢复。列表重排或修改显示名不会选错，预设失效时回退内置默认。

Windows 和 Linux 同步提供以上改进，编解码器依赖版本保持不变。

下载资产拟为 `AWJ_Win.7z` 和 `AWJ_Linux.7z`，以实际验收后的校验值为准。Windows 图形界面和右键转换需要将 AWJ.exe 与 AWJ.com 放在同一目录。

## English release notes

This release improves queue file information, header controls and preset persistence, and fixes dark-menu rendering.

- **Compare sizes**: clearly labeled source sizes and a new output-size column show the result of successful conversion without retaining stale sizes for pending, skipped or failed tasks.
- **Independent column widths**: drag a divider to resize only the column on its left. Widths remain unchanged when maximizing, narrowing or switching pages. Scroll overflowing headers and rows together with the bottom scrollbar.
- **Choose visible columns**: right-click the header to toggle every column, including restoring columns after hiding them all. Text is left-aligned and ellipsized on the right.
- **Menu fixes**: remove full-row white focus outlines from column-menu options while retaining checkboxes and keyboard support. Task context menus now follow the light/dark theme. Header dividers use a pixel-aligned single physical pixel to stay thin at high DPI, with softer theme colors and unchanged drag targets.
- **Remember queue presets**: start with the built-in defaults, save user selections in AWJ.jsonc and restore them on the next launch. Reordering or editing display names does not select a different preset. Missing or invalid presets fall back to the built-in defaults.

The changes are available on Windows and Linux without changing codec dependency versions.

Planned assets are `AWJ_Win.7z` and `AWJ_Linux.7z`. Final checksums are determined after candidate validation. Keep AWJ.exe and AWJ.com together for the Windows GUI and context-menu converter.
