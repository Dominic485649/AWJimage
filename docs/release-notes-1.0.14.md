# AWJimage 1.0.14

本地联合发布候选，尚未发布。此版本累计包含 1.0.13；两个版本的更新日志独立保留。验收状态与未完成门槛见 [验证报告](validation-1.0.14.md)。

## 发布概述

AWJimage 1.0.14 在 1.0.12 基础上加入原生 HEIC/HEIF 解码、百分比缩小和 JPEG 到 JXL 无损转换，修复队列交互、混合尺寸图片的内存调度及更新入口。Windows 新增可选的兼容性右键菜单，默认仍无需管理员权限。

## 主要变化

- **HEIC 与尺寸处理**：原生 HEVC 解码保留源颜色、位深、透明和容器变换；手动尺寸支持 1–100% 等比缩小。实际缩放会阻止 AVIF/JXL 的无损直通，确保输出满足尺寸要求。
- **JXL 无损 JPEG**：新增默认开启的“JPG转换走无损”，符合条件时保留 JPEG bitstream；其他输入继续使用像素编码和原有质量设置。
- **可选兼容菜单**：设置页在 WIC 兜底上方提供开关。未安装时延迟到下次注入，已安装时立即应用；机器项变更需要 UAC，取消或失败恢复原状态。原用户进程管理 HKCU，提权辅助进程只处理固定的机器项。
- **队列与关闭**：拖放提示使用半透明蓝；详情在松开后打开，拖动后不误触；列宽拖动和表头对齐修复。编码中关闭先确认，Windows 验证终止结果，Linux 等待取消清理。
- **资源与维护**：复用导入扫描和路径集合，按可准入工作回填内存预算，并保持同输出路径串行。Studio 按职责拆分；不以源码改动替代实测性能结论。
- **签名更新**：版本统一来自 VERSION，优先读取无版本后缀的更新入口；仅在入口缺失时成对回退，验签、损坏和防重放错误不触发降级。

## 依赖与构建

待最终冻结源码的 MSVC/GCC Release 构建记录回填。发行构建关闭测试接口，并保留渲染回退和无障碍。

## 发行归档

待最终打包校验后回填实测体积和 SHA-256。

| 归档 | 精确内容 | 大小 | SHA-256 |
| --- | --- | ---: | --- |
| AWJ_Win.7z | AWJ.exe、AWJ.com、LICENSE、NOTICE.txt | 待验证 | 待验证 |
| AWJ_Linux.7z | AWJ、LICENSE、NOTICE.txt | 待验证 | 待验证 |

## 二进制哈希

待最终产物实测，不沿用旧版本哈希。

---

# AWJimage 1.0.14 (English)

Local release candidate, not published. This release includes 1.0.13 cumulatively, with separate changelog entries. See the [validation report](validation-1.0.14.md) for outstanding acceptance gates.

## Overview

AWJimage 1.0.14 adds native HEIC/HEIF decoding, percentage scaling, and lossless JPEG-to-JXL conversion to 1.0.12, with fixes for queue interaction, mixed-size memory scheduling, and updater endpoints. Windows gains an optional compatibility context menu; default registration remains elevation-free.

## Main Changes

- Native HEVC decoding preserves source color, bit depth, alpha, and container transforms. Manual Size supports 1–100% scaling; actual resizing disables lossless passthrough so size limits are honored.
- The default-on lossless JPEG option preserves the JPEG bitstream in JXL where supported; other inputs retain pixel encoding and existing quality controls.
- Compatibility registration is selected above WIC fallback. The choice is deferred until installation when no menu exists, and migrates an installed menu immediately. UAC cancellation or failure retains the original state. The original user manages HKCU; the elevated helper handles fixed machine entries only.
- Translucent drop feedback, release-based queue selection, and corrected column dragging improve queue interaction. Closing during conversion requires confirmation and verified termination or cancellation cleanup.
- Import reuses scan results and path sets. Runtime memory admission fills available capacity while keeping identical output paths serial. Studio code is split by responsibility; performance claims require measurements.
- Versioning comes from VERSION. Canonical update endpoints fall back as a pair only when absent; signature, corruption, and replay failures never downgrade validation.

## Dependencies and Build

Pending final MSVC/GCC Release evidence. Distribution builds disable test interfaces and retain rendering fallback and accessibility.

## Archives and Binary Hashes

The final archive sizes, SHA-256 values, and binary hashes will be filled from verified artifacts in the tables above.
