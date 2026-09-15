# 1.0.13 本地候选验证

日期：2026-09-16。源码在 `D:/Code/Cpp/AWJimage-1.0.13-completion` 的 `codex/1.0.13` 分支完成；主工作区的 1.0.14 拆分由另一任务维护。本任务只交付本地产物，不推送、发布、创建远端标签或签署 stable 清单。

构建源码提交：Windows `5dc91860b38ce9fea232c55447fb627015415488`，Linux `1ca9d6018ac5a866fd3f68b3eb7fa5734ae26a8f`。两提交的 191 个文件 Git blob 全部一致；后续只更新本验证文档。证据：`build/evidence/source-parity.json`。

## 构建与回归

| 项目 | 本轮证据 |
| --- | --- |
| Windows Release 测试构建 | MSVC，IPO/LTO，48/48 CTest 通过，45.07 秒；`build/ctest-windows.log` |
| Windows 正式构建 | MSVC Release、IPO/LTO，`BUILD_TESTING=OFF`、`SLINT_FEATURE_TESTING=OFF`；`build/build-release-final.log`，最终版本输出 1.0.13 |
| VERSION 普通构建联动 | 临时改为 1.0.99，仅执行 `cmake --build` 后输出 `AWJimage 1.0.99`；已恢复源码为 1.0.13；`build/version-rebuild-check.log`、`build/version-rebuild-result.log` |
| Linux Release 测试构建 | GCC 16.1，29/29 CTest 通过，3.33 秒；`build/evidence/ctest-linux.log`。文案同步后刷新嵌入日志，内容一致性回归通过 |
| Linux 正式构建 | GCC 16.1 Release、IPO/LTO，`BUILD_TESTING=OFF`、`SLINT_FEATURE_TESTING=OFF`；版本为 1.0.13，`ldd` 未发现缺失库；`build/evidence/build-release-final.log`、`build/evidence/ldd-final.log` |
| 拒绝旧二进制打包 | 保存的 1.0.12 被打包脚本在归档/清单写入前拒绝；`build/evidence/package-version-negative.log` |

48 项 Windows 回归覆盖配置、预设、尺寸、资源规划、codec、native pipeline、安全、取消、更新器、CLI、Shell 注册/Explorer、导入、拖放、UI 和队列压力。测试构建与关闭测试功能的发行二进制分别验证。

## 本轮补齐

- Windows 队列任务 ID/执行序号索引、状态计数和行级更新；共享完整日志，清空队列释放旧模型与索引。
- 失败过滤模型及原始行映射；修正固定版本 Slint FilterModel 插入/删除索引和通知错误。
- 更新历史按需生成，Linux 普通队列事件增量计数、按执行序号优先定位。
- 1k/10k 队列创建、过滤、变更、150 次页面切换及模型释放回归；这些是功能压力检查，不等于三版本真实窗口内存矩阵。
- JPEG→JXL→JPEG 字节回转，JXL 开关，以及实际缩放阻止 JXL/AVIF 直通的回归。
- HEIC 10/12 位样本扩展至 16 位管线范围、解码线程预算、取消、无 alpha 图片的有效位数和 EXIF 方向归一化。
- 打包前及解压后的实际版本检查；双语文案移除旧产物哈希和未经验证的完成结论。

## HEIC 证据

原图 SHA-256：`023B6CD60B3479BEA9D104C2B08F3BAB0AC0CCCF6203BD2FB2C8EC7CDCA13E79`。私人样片不提交到仓库。

- 原图编码尺寸 4032×3024，EXIF Orientation=6，容器 Rotation=3；应用容器方向后的目标尺寸为 3024×4032。
- `clap_cropped.heic`、`conformance_window_padding.heic`、`rainbow-451x461.heic`、`with-alpha-512x512.heic` 原生解码通过，尺寸依次为 64×64、1×1、451×461、512×512。证据：`build/evidence/decoder-fixtures.log`。
- 10/12/16 位端点、损坏容器、提前取消与 EXIF 方向归一化已纳入 `decoder_registry_core`。高位深端点是像素范围单元测试，不冒充真实 HDR 样片验证。
- 最终 Windows 发行二进制的六组转换全部通过：四个补充样本转 PNG、原图转 PNG/AVIF。原图两种输出均为 3024×4032，AVIF 为 `yuv420p10le`、`pc` 全范围、EXIF Orientation=1；源色度报告为 420。源无 NCLX 范围字段时不将输出的全范围反写成已探测的源范围。证据：`build/evidence/heic-delivery/results.json`。
- Linux 最终发行二进制的原图 AVIF/PNG 转换和元数据检查均通过，两种输出分别与 Windows 结果逐字节相同。证据：`build/evidence/heic-linux-final.json`。
- 第一次批量检查中原图 AVIF 进程曾提前退出，未生成 summary；该次未记录子进程退出码，原因尚未确定。之后完整批次通过，另做 12 次独立转换，退出码均为 0，summary 均为 ok、420、全范围。异常日志和重复运行证据分别保存在 `build/evidence/heic-verified/photo-avif/conversion.log`、`build/evidence/heic-repeat.csv`；不将此记为已修复的缺陷。

## 性能

以保存的 1.0.12、上一轮 1.0.13 和最终 1.0.13 串行比较，每种格式使用三张 1536×1024 生成图、q80、4 线程、2 GiB 预算，AVIF/WebP/JXL speed=6。每个候选预热一轮后采样五轮；表内为包含进程启动的批次耗时中位数，单位秒。

| 候选 | AVIF | WebP | JXL | JPGLI | PNG |
| --- | --- | --- | --- | --- | --- |
| 1.0.12 | 0.545 | 0.263 | 0.305 | 0.235 | 0.298 |
| 上一轮 1.0.13 | 0.534 | 0.293 | 0.320 | 0.240 | 0.289 |
| 最终 1.0.13 | 0.547 | 0.283 | 0.315 | 0.238 | 0.325 |

各候选在独立目录中启动五次，窗口句柄出现后稳定两秒采样。这里的启动指标是句柄出现时间，不是首帧呈现时间。

| 候选 | 启动 ms | 私有内存 MiB | 工作集 MiB | 句柄 |
| --- | --- | --- | --- | --- |
| 1.0.12 | 102.30 | 121.11 | 97.75 | 391 |
| 上一轮 1.0.13 | 102.59 | 124.41 | 100.89 | 535 |
| 最终 1.0.13 | 107.11 | 124.32 | 101.24 | 535 |

三份 EXE 体积依次为 41,299,456、42,725,376、42,769,408 bytes。最终版没有净体积下降，私有内存也没有下降，因此不宣称整体性能、内存或体积改善。最终版相对 1.0.12 的小批次耗时约增加 0.4%～9%，不同轮次存在明显波动；本组不足以证明吞吐回归或提升。原始数据、输入/候选哈希及测量脚本保存在 `build/evidence/performance-delivery` 和 `build/evidence/measure-candidates.ps1`；三个被测 EXE 留存在该目录的 `idle-baseline`、`idle-features`、`idle-final` 中。

这三个现存候选不等于严格控制变量的“同一源码优化前后”构建，因此只能用于候选间观察，不能单独证明某项优化的因果收益。

## 本地产物

双平台已重新打包，通过 `7z t`、解压后逐文件 SHA-256 比对、成员集合检查及实际版本/帮助检查。Linux 原生解压保留可执行位。Windows 目录只包含 `AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt`；Linux 归档只包含 `AWJ`、`LICENSE`、`NOTICE.txt`。

| 文件 | Bytes | SHA-256 |
| --- | ---: | --- |
| AWJ_Win.7z | 12001737 | `AA50E830C1EA71D577AFE1EE20C200261E1BDE83E06EC50022ED577C9A9A0DA4` |
| AWJ_Linux.7z | 19424281 | `3CE2F2D6104282CC5E717E566D0B3B3303B820291FDA9D06144434A9320573EA` |
| Windows AWJ.exe | 42769408 | `463D6C944964EFE5500CFDD02890C766B6026693820F66EEDF420F5EC53F5B7C` |
| Linux AWJ | 67533264 | `A7DB5F055E300937FF66543A1059265DFB1DA728367E2479F859CF3A0574E768` |

已复制至 `D:/Code/Cpp/AWJimage/bin/1.0.13`：两个归档位于该目录，Windows 四文件程序位于 `release` 子目录。复制后再次核对全部哈希、精确成员集合，`AWJ.com --version` 输出 `AWJimage 1.0.13`。原工作区仍为 `codex/1.0.14`，本任务只更新其上述发行产物。

本地证据：`build/evidence/package-final.log`、`build/evidence/archive-hashes.json`、`build/evidence/artifact-members.json`、`build/evidence/delivery.json`。双语 Release 文案为 `docs/release-notes-1.0.13.md`。

## 未完成的完整计划验收

- Directory Opus 实机菜单验收未执行；目前证据是 Shell 结构、注册和 Explorer 回归。
- 更新器真实安装、替换与回滚 E2E 未执行；已有更新器自动测试不能代替安装验收。
- 严格三版本的真实窗口 1k/10k 队列内存矩阵、Rust ThinLTO 候选比较未完成；不据此宣称内存或二进制体积改善。
- F6、截图和截屏路径不在本轮验证中。

本地候选构建和归档验证不代表上述完整计划验收已经全部完成，也不代表公开发布审核已完成。
