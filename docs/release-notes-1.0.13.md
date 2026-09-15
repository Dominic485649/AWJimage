# AWJimage 1.0.13 Release Notes

## 中文

1.0.13 完善尺寸控制、原生 HEIC 解码、JPEG 转 JXL、任务队列和更新兼容。

- 手动尺寸支持 1～100% 等比缩小，与最大宽、高、长边、短边限制共同生效。需要改变尺寸时，AVIF 直通和 JPEG→JXL 转封装会回退到正常缩放流程。
- 新增 libheif 1.23.4 + libde265 1.1.2 原生 HEIC/HEIF 解码，仅启用 HEVC 解码后端。保留源色度、位深和可用的颜色元数据，支持透明度及容器裁切、旋转、镜像；修正高位深样本范围、无透明通道图片的 PNG 输出和重复应用 EXIF 方向的问题。
- JXL 默认开启“JPG转换走无损”，可兼容的 JPEG 使用 libjxl 无损码流转换，其他情况自动回退到普通像素编码。
- 队列状态、耗时和日志按行更新，状态计数增量维护。“仅显示失败”使用过滤模型并保持原始任务映射；清空队列释放旧模型和索引。更新历史和系统字体按需加载。
- 队列六个命名选项保持等宽和最小间距，窄窗口自动换行。参数页可删除用户预设；内置默认预设和正在运行的任务受到保护。
- Windows 右键菜单保持当前用户 HKCU 注册，无需 UAC；参数及预设同步沿用事务和失败回滚。预设写入对短暂的文件占用进行有限重试。
- 根目录 `VERSION` 统一驱动程序版本，支持 `--version`。打包前和解压后核对实际二进制版本，防止旧程序被装入新版本归档。
- 更新器优先使用 `update-keyring.json` 和 `update-archive.json`，兼容旧入口及旧暂存名称。仅 canonical 文档或签名返回 404 时成对回退，安全校验失败不会降级。

本轮准备 Windows/Linux 本地候选及发行文案，不推送、不打远端标签、不发布 GitHub Release，也不签署 stable 更新清单。构建、回归、产物哈希和仍未完成的验收项目见 [验证记录](validation-1.0.13.md)。本版本不宣称未经实测支持的体积、内存或吞吐提升。

## English

Version 1.0.13 improves size controls, native HEIC decoding, JPEG-to-JXL conversion, the task queue, and updater compatibility.

- Manual Size supports proportional scaling from 1 to 100 percent together with width, height, long-edge, and short-edge limits. Resizing disables AVIF passthrough and JPEG-to-JXL bitstream shortcuts so the normal resize pipeline runs.
- Native HEIC/HEIF decoding uses libheif 1.23.4 and libde265 1.1.2 with only the HEVC decoder enabled. It preserves source chroma, precision, and available color metadata, and supports alpha, cropping, rotation, and mirroring. Fixes cover high-bit-depth sample expansion, PNG output without alpha, and duplicate EXIF orientation application.
- Lossless JPEG-to-JXL conversion is enabled by default. Compatible JPEGs use libjxl bitstream transcoding; other inputs fall back to normal pixel encoding.
- Queue status, duration, and logs update individual rows, with incremental counters. Failed-only filtering retains source-task mapping, and clearing the queue releases old models and indexes. Update history and system fonts load on demand.
- The six filename options retain equal widths and minimum spacing, wrapping on narrow windows. User presets can be deleted from the parameter page, with protections for the built-in default and active tasks.
- Windows shell integration remains per-user through HKCU without UAC. Parameter and preset synchronization retains transactional rollback, while preset writes tolerate bounded transient file-sharing conflicts.
- The root `VERSION` file drives the application version and `--version`. Packaging checks the actual binary version before archiving and after extraction to reject stale binaries.
- The updater prefers `update-keyring.json` and `update-archive.json` while retaining legacy endpoint and staging compatibility. Paired fallback occurs only when the canonical document or signature returns 404, never after security-validation failures.

This work prepares local Windows/Linux candidates and release copy. It does not push, create remote tags, publish a GitHub Release, or sign a stable update manifest. See the [validation record](validation-1.0.13.md) for builds, tests, artifact hashes, and outstanding acceptance work. No unsupported binary-size, memory, or throughput improvement is claimed.
