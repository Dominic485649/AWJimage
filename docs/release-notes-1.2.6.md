# AWJimage 1.2.6

> 发布说明草稿，待审查。本文件不表示已经签名、推送或发布。

本版集中改进 HDR / Gain Map 转换、内存与线程调度，并针对 Windows 跨屏缩放和恢复后的重绘问题修复相关代码路径。

## HDR、Gain Map 与色彩

- **按目标格式选择正确呈现**：正向 Gain Map 输入以重建后的 HDR 为基准，反向 Gain Map 输入以原 HDR 主图为基准；输出 SDR 时优先使用输入已有的 SDR 端点，避免再次 tone-map。
- **保留 HDR / SDR 双呈现**：输入同时具有两端时，AVIF / JXL 重新编码 HDR 主图与用于恢复 SDR 的反向 Gain Map；HDR JPG 使用已有 SDR 端点生成 8-bit SDR 主图与 HDR Gain Map，并依据最终 JPG 主图的回解结果重算增益图。仅有 HDR 的输入转 AVIF / JXL 时，不额外生成原本不存在的 SDR 端点。
- **修复 HEIC Gain Map 的颜色信令兼容性**：正确区分输入 ICC / NCLX 与解码库补出的默认标签，避免合法 ICC 主图被误标或误判冲突；补充关键阶段诊断。损坏或无法安全解释的功能性增益元数据仍明确报错，不静默丢弃。
- **保留必要颜色信息**：清理普通元数据时，仍保留正确解码所需的颜色信息与功能性 Gain Map。重新生成增益图与有损编码不承诺两端逐像素无损。

## 位深与格式处理

- **统一重新编码的自动位深**：AVIF 使用 10-bit，参考呈现精度高于 10-bit 时使用 12-bit；JXL 使用 16-bit；PNG 的 SDR 使用 8-bit、HDR 使用 16-bit；JPG / WebP 使用 8-bit。显式位深及色度采样按格式能力执行，不静默忽略不支持的请求。码流直通与无损转封装保留原码流属性。
- **优先独立静态图**：APNG 有独立默认静态图时优先处理该图；AVIF 根据主图与序列样本的实际存储关联识别独立静态图。纯动画继续保留，无法安全确认的关联明确报错，不以动画兜底。

## 内存、线程与诊断

- **修复会话内存解锁的传递**：将工作集预算与输入、尺寸及缓冲区安全保护分开，正确传递到 Windows、Linux 和工作进程。解锁默认关闭，只对当前会话生效，不写入预设；不因此取消必要的输入安全保护。
- **按实际处理路径估算内存**：结合源画像、目标尺寸和同时存活的阶段，区分 SDR / HDR JPEGli，并补足 WebP / JXL 编码工作区、PNG 高熵输出、Gain Map 双呈现及缩图成本。
- **有界并行与可追溯日志**：改进 Gain Map 的并行计算和 HEIF 图块 / 编解码线程预算，避免线程数叠乘越界；完善阶段耗时、工作线程及 CSV 内存预算、估算来源和源 / 目标尺寸信息。线程预算不等同于 CPU 利用率，也不承诺所有输入等比例提速。

## Windows 跨屏与窗口重绘

- 针对 Windows 11 跨不同 DPI 显示器时旧显示器位置校正可能触发反复缩放的问题，回移 winit 上游修复；使用系统建议的目标位置，保留显式客户区尺寸和旧系统兼容逻辑，不锁定缩放倍数。
- 回移 Slint 对 Windows 系统失效区域的重绘修复，处理跨 DPI、最小化恢复后可能出现的旧画面或空白；没有系统失效区域时保留局部重绘。
- 窗口尺寸按逻辑像素保存，正确接受小于 100% 的合法正比例，并安全处理无效比例和尺寸边界。

## 依赖更新

更新并锁定 libheif **1.23.7**、libaom **3.15.2**、libjpeg-turbo **3.2.1**、LibRaw **0.22.3**、libarchive **3.8.10**、libplacebo CPU 子集 **7.387.0** 和 Linux curl **8.23.0**。Slint 保持 **1.18.1**、winit 保持 **0.30.13**，相关上游修复以项目内定点补丁接入，并记录源码与补丁身份。

## 验证与兼容性说明

- 已交付的本地候选完成 Windows **27 项**、Linux **23 项**定向测试，以及两平台各 **33 项**公开 CLI 转换验证。Linux 产物的独立 FFmpeg 检查在 Windows 执行，不表示运行了无关全量回归。
- 跨屏验证包含 **8 组受控系统切换**和 **8 组合成移动状态**；真人鼠标拖动原始场景、独立 DWM 画面和全部输入 / 拖放坐标尚未完成复验，不宣称所有显示器组合的问题均已根治。
- HDR 呈现仍取决于查看器、显示链与格式支持。本版本不承诺修复第三方查看器或截图工具的 HDR / SDR 自动选层及 tone-map，也不会以调暗 HDR 或修改必要颜色标签补偿外部显示。
- Windows 要求支持 **AVX2** 的 CPU，Linux 要求 **x86-64-v3**。Windows 图形界面和右键转换需要将 `AWJ.exe` 与 `AWJ.com` 放在同一目录。

正式发布资产拟沿用 `AWJ_Win.7z`、`AWJ_Linux.7z` 和 `SHA256SUMS`。当前交付仍为未发布候选，正式资产及其校验值以审查后按正式流程生成的结果为准。

<details>
<summary>English</summary>

This version improves HDR / gain-map conversion, memory planning and bounded parallelism, and addresses Windows cross-monitor DPI and repaint code paths.

### HDR, gain maps and color

- **Select the appropriate rendition for the destination**: use reconstructed HDR for forward gain maps and the original HDR main image for reverse gain maps. Prefer an existing SDR endpoint for SDR output instead of tone-mapping it again.
- **Preserve paired renditions**: when both endpoints exist, AVIF / JXL re-encode an HDR main image and a reverse gain map for SDR reconstruction. HDR JPG uses the existing SDR endpoint for its 8-bit base and recomputes the HDR gain map against the decoded final JPG base. HDR-only inputs do not acquire a synthesized SDR endpoint when converted to AVIF / JXL.
- **Fix HEIC gain-map color signaling compatibility**: distinguish original ICC / NCLX metadata from decoder-generated defaults, avoiding false labels and conflicts for valid ICC inputs. Improve stage diagnostics while continuing to reject corrupt or unsafe functional metadata explicitly.
- **Retain essential color data**: stripping ordinary metadata does not remove required color signaling or functional gain maps. Regenerated maps and lossy encoding do not promise sample-exact preservation of both endpoints.

### Bit depth and format handling

- **Consistent defaults for pixel re-encoding**: AVIF uses 10-bit, or 12-bit when the reference rendition exceeds 10-bit; JXL uses 16-bit; PNG uses 8-bit for SDR and 16-bit for HDR; JPG / WebP use 8-bit. Honor supported explicit depth and chroma requests, and reject unsupported requests rather than silently ignoring them. Bitstream passthrough and lossless transcoding retain the original stream properties.
- **Prefer independent still images**: select an independent APNG default image or an AVIF primary image identified through actual primary/sample storage relationships. Preserve pure animations and reject ambiguous relationships instead of silently falling back to motion.

### Memory, threading and diagnostics

- **Propagate the session memory unlock correctly** across Windows, Linux and worker processes, independently of input, dimension and buffer safety limits. It is off by default, applies only to the current session and is not stored in presets.
- **Plan memory from the actual processing path**, source profile, destination dimensions and simultaneously live stages. Account for SDR / HDR JPEGli, WebP / JXL workspaces, high-entropy PNG output, paired renditions and resizing.
- **Bound parallel work and improve diagnostics** for gain-map processing and HEIF tile/codec thread budgets. Expand stage timing and CSV budget/profile/dimension information. A thread budget is not CPU utilization, and proportional speedups are not promised for every input.

### Windows DPI and repainting

- Backport the winit Windows 11 fix for repeated scaling caused by old-monitor position correction. Use the system-suggested destination position while preserving explicit client sizes and legacy-system behavior, without fixing the scale factor.
- Backport Slint's Windows update-region invalidation fix for stale or blank content after DPI changes or restoration, retaining partial rendering when Windows has not invalidated a region.
- Persist logical window dimensions and handle valid sub-100% scales, invalid scale values and dimension limits safely.

### Dependencies

Update and pin libheif **1.23.7**, libaom **3.15.2**, libjpeg-turbo **3.2.1**, LibRaw **0.22.3**, libarchive **3.8.10**, the libplacebo CPU subset **7.387.0**, and Linux curl **8.23.0**. Slint remains **1.18.1** and winit **0.30.13**, with project-local, source-identified backports.

### Validation and compatibility

- The delivered local candidate completed **27 targeted Windows tests**, **23 targeted Linux tests**, and **33 public CLI conversion cases per platform**. Independent FFmpeg checks of Linux outputs ran on Windows. This is not a claim of a full unrelated regression run.
- DPI checks cover **8 controlled monitor-switch cases** and **8 synthetic move-state cases**. The original human mouse-drag scenario, independent DWM captures and all input/drop coordinates have not been revalidated; this does not establish that every monitor combination is fixed.
- Third-party viewers and screenshot tools still determine HDR / SDR rendition selection and tone mapping. This version does not claim to repair those external display paths or compensate by darkening HDR or changing required color tags.
- Windows requires **AVX2**; Linux requires **x86-64-v3**. Keep `AWJ.exe` and `AWJ.com` together for the Windows GUI and context-menu converter.

Planned formal assets remain `AWJ_Win.7z`, `AWJ_Linux.7z` and `SHA256SUMS`. Current deliveries are unpublished candidates; final assets and checksums must come from the formal release process after review.

</details>
