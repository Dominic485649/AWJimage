# AWJimage 1.0.14

正式版，2026-09-17。此版本累计包含 1.0.13；两个版本的更新日志独立保留。本轮按维护者要求不再运行测试，仅完成发行构建和产物核对；历史验证与未覆盖项目见 [验证报告](https://github.com/Dominic485649/AWJimage/blob/codex/1.0.14/docs/validation-1.0.14.md)。

## 发布概述

AWJimage 1.0.14 在 1.0.12 基础上加入原生 HEIC/HEIF 解码、百分比缩小和 JPEG 到 JXL 无损转换，修复队列交互、混合尺寸图片的内存调度及更新入口。Windows 新增可选的兼容性右键菜单，默认仍无需管理员权限。

## 主要变化

- **HEIC 与尺寸处理**：原生 HEVC 解码保留源颜色、位深、透明和容器变换；手动尺寸支持 1–100% 等比缩小。实际缩放会阻止 AVIF/JXL 的无损直通，确保输出满足尺寸要求。
- **JXL 无损 JPEG**：新增默认开启的“JPG无损转换”，符合条件时保留 JPEG bitstream；其他输入继续使用像素编码和原有质量设置。简化选项名称并移除右侧提示。
- **可选兼容菜单**：设置页在 WIC 兜底上方提供开关。未安装时延迟到下次注入，已安装时立即应用；机器项变更需要 UAC，取消或失败恢复原状态。原用户进程管理 HKCU，提权辅助进程只处理固定的机器项。
- **队列与关闭**：拖放提示使用半透明蓝；详情在松开后打开，拖动后不误触；列宽拖动和表头对齐修复。编码中关闭先确认，Windows 验证终止结果，Linux 等待取消清理。
- **资源与维护**：复用导入扫描和路径集合，按可准入工作回填内存预算，并保持同输出路径串行。Studio 按职责拆分；不以源码改动替代实测性能结论。
- **签名更新**：版本统一来自 VERSION，优先读取无版本后缀的更新入口；仅在入口缺失时成对回退，验签、损坏和防重放错误不触发降级。
- **界面细节**：活动标题栏与左侧导航同色，队列拖入蓝色提示保留圆角。菜单页移除顶部说明行；兼容菜单说明先展示用途，再以黄字说明自动提权，无需管理员启动 AWJ。普通菜单操作不显示提权遮罩。
- **菜单双向切换**：已安装或仅残留系统级菜单时，切换模式会清理并重建；未安装时只保存选择。修复管理员恢复快照的权限识别，保留失败回滚及恢复日志。

## 依赖与构建

源码提交 `33dfae4482e31bcbdd9a3b125984d0035333230c` 已完成 MSVC / GCC Release 构建；Windows 与 Linux 发行构建均关闭测试接口，并保留渲染回退和无障碍。Windows 为 x64、IPO 开启、`AWJ_ENABLE_X64_V3=OFF`；Linux 为 x86_64 原生文件系统构建。本轮未运行测试套件或实机 UI 验收。

## 发行归档

归档已通过 `7z t`、解压成员比对、解压哈希、版本/帮助检查；Linux 解压后的 `AWJ` 保留可执行位。

| 归档 | 精确内容 | 大小 | SHA-256 |
| --- | --- | ---: | --- |
| AWJ_Win.7z | AWJ.exe、AWJ.com、LICENSE、NOTICE.txt | 12,031,942 | `372680fd4d49cc6f12fd86e97d06f7510c95936ef7e9b8e1047ec70b46950287` |
| AWJ_Linux.7z | AWJ、LICENSE、NOTICE.txt | 19,446,096 | `4dd51c237087b4f67f951846595a6784e39dc4b839c72a9e09ba8c1c4dbbf439` |

## 二进制哈希

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| Windows `AWJ.exe` | 42,872,320 | `2f09c084c219d965687124bd7f946dd6e3abaf5e6c2c8ef2e539da3cf15484d5` |
| Windows `AWJ.com` | 450,048 | `833a7bb11b9b98f1d3ce18a0d4a920e8ec5bd8b71ba97c5bc32c1e44f963c1f7` |
| Linux `AWJ` | 67,639,760 | `0a02a5577c7687ee013b093d599f53a856fc56cb25cf38c36f00daa07f440f39` |

---

# AWJimage 1.0.14 (English)

Stable release, 2026-09-17. This release includes 1.0.13 cumulatively, with separate changelog entries. At the maintainer's request, this final revision was built and packaged without rerunning tests. See the [validation report](https://github.com/Dominic485649/AWJimage/blob/codex/1.0.14/docs/validation-1.0.14.md) for historical evidence and remaining coverage gaps.

## Overview

AWJimage 1.0.14 adds native HEIC/HEIF decoding, percentage scaling, and lossless JPEG-to-JXL conversion to 1.0.12, with fixes for queue interaction, mixed-size memory scheduling, and updater endpoints. Windows gains an optional compatibility context menu; default registration remains elevation-free.

## Main Changes

- Native HEVC decoding preserves source color, bit depth, alpha, and container transforms. Manual Size supports 1–100% scaling; actual resizing disables lossless passthrough so size limits are honored.
- The default-on lossless JPEG option preserves the JPEG bitstream in JXL where supported; other inputs retain pixel encoding and existing quality controls.
- Compatibility registration is selected above WIC fallback. The choice is deferred until installation when no menu exists, and migrates an installed menu immediately. UAC cancellation or failure retains the original state. The original user manages HKCU; the elevated helper handles fixed machine entries only.
- Translucent drop feedback, release-based queue selection, and corrected column dragging improve queue interaction. Closing during conversion requires confirmation and verified termination or cancellation cleanup.
- Import reuses scan results and path sets. Runtime memory admission fills available capacity while keeping identical output paths serial. Studio code is split by responsibility; performance claims require measurements.
- Versioning comes from VERSION. Canonical update endpoints fall back as a pair only when absent; signature, corruption, and replay failures never downgrade validation.
- The active title bar matches the navigation background, queue drop feedback has rounded corners, and the JXL option is now "Lossless JPG conversion" without an adjacent hint. Compatibility help explains automatic elevation below its purpose text. Ordinary menu operations do not show the elevation overlay.
- Switching either way rebuilds installed menus, including machine-only remnants; an uninstalled menu only saves the preference. Fixed permission validation for machine recovery snapshots while preserving rollback and recovery logs.

## Dependencies and Build

MSVC/GCC Release builds completed from commit `33dfae4482e31bcbdd9a3b125984d0035333230c`. Distribution builds disable test interfaces and retain rendering fallback and accessibility. No test suite or interactive UI acceptance was run for this final revision.

## Archives and Binary Hashes

The tables above contain final archive sizes, SHA-256 values, and binary hashes. Archive integrity, exact members, extracted hashes, version/help output, and Linux executable permissions were checked.
