# AWJimage 1.2.4 Issue 回复草稿

以下正文用于 AWJimage 1.2.4 正式发布且下载可用后回复对应 issue，随后以已完成原因关闭。此文件是发布文案存档，实际回复和关闭状态以各 issue 页面为准。Release 链接指向仓库发布页。

## Dominic485649/AWJimage#8

对应 issue：https://github.com/Dominic485649/AWJimage/issues/8

### 正式发布后发送的正文

感谢建议！AWJimage 1.2.4 已增强编码队列，现在可以直接比较转换前后的文件大小：

- 明确显示“源文件大小”，新增“输出大小”列，成功转换后显示实际输出文件大小。
- 尚未生成、失败、取消或跳过的任务不会显示旧的输出大小。
- 各列可独立拖动调整，窗口大小变化或切换页面不会重新分配列宽；内容超出窗口时可横向滚动。
- 右键表头可以勾选显示或隐藏各列，文字左对齐，过长内容在右侧省略。
- 分隔线和右键菜单已适配深浅色主题，细线在不同缩放下保持清晰。

下载与更新说明：[AWJimage Releases](https://github.com/Dominic485649/AWJimage/releases)。

相关增强已随新版本发布，本 issue 将关闭。感谢反馈！

## Dominic485649/AWJimage#9

对应 issue：https://github.com/Dominic485649/AWJimage/issues/9

起草依据仅为本轮已实现的队列预设增强，没有读取该 issue，也不推断其标题或原文。

### 正式发布后发送的正文

感谢反馈！AWJimage 1.2.4 已增强编码队列的预设选择与记忆：

- 首次使用时默认选择内置预设。
- 切换到用户预设后，会将选择保存到 AWJ.jsonc，下次启动自动恢复。
- 通过预设文件身份保存选择，不会因列表重新排序、界面语言切换或显示名修改而误选其它预设。
- 所选预设文件被删除或失效时，自动回退到内置默认。

Windows 和 Linux 均提供上述改进。

下载与更新说明：[AWJimage Releases](https://github.com/Dominic485649/AWJimage/releases)。

相关增强已随新版本发布，本 issue 将关闭。感谢反馈！
