# AWJimage 1.2.0

## 发布概述

JPEG、AVIF、HEIC/HEIF 中受支持的 Gain Map 会先与主图合成增强像素，再进入输出编码。JPEG 输出自动保留 HDR；GIF/APNG/WebP/AVIF 动画转 AVIF 自动保留帧时长、循环和透明，无需首页开关。

## 主要变化

- **HDR JPEG 自动输出**：普通 SDR 输入使用 JPEGli；HDR 输入生成 8-bit SDR base + 多通道 Gain Map，两部分均由 JPEGli 编码，采用 ISO 21496-1 和 MPF 封装。内部 HDR 保持 FP16 精度；普通 JPEG 解码器可读取 SDR 主图。界面、预设、右键固定 auto，CLI 可用 `--jpeg-hdr sdr` 强制 SDR 或 `hdr` 要求有效 HDR。HDR JPEG 只接受常规 quality，不支持视觉质量搜索。
- **Gain Map 增强**：支持 JPEG Ultra HDR、AVIF ISO Gain Map，以及支持范围内的 HEIF Apple legacy / ISO tmap。识别出损坏或暂不支持的 map 时明确失败。增强输出默认 10-bit HDR AVIF；WebP 和增强 PNG 执行 SDR tone mapping。JPEG 的新 Gain Map 不产生独立文件或 sidecar。
- **动画 AVIF**：先按 GIF disposal、APNG blend/dispose 和 WebP 动画规则合成完整画布，再顺序编码。AVIF 使用 sequence tracks 判定动画，避免将 progressive 或辅助图项当成动画。默认质量 70、速度 5；时长与循环跟随输入。UI、预设和右键自动处理动画；仅 CLI 提供参数 `--avif-animation-tune auto|ssim|psnr`、`--avif-animation-keyframe N`（默认 0）。
- **YUV auto 与菜单**：自动色度参照 avifenc，保留 420/422/444、灰度使用 400、RGB 和未知源使用 444；新增可强制灰度的 AVIF 400 选项，保留 alpha；正常及兼容菜单不再强制置底，采用 Windows 普通排序。具体相邻项目顺序由 Explorer 决定。

## 依赖与构建

- 新增 libultrahdr 2.0.2；libaom 更新为 3.15.1、libheif 为 1.23.6、libpng 为 1.6.59、Slint 为 1.18.1。继续复用 libavif、giflib、libwebp、JPEGli、libjxl 和 lcms，不新增 FFmpeg、Exiv2、spng。
- 依赖选择遵循“随 AWJ 发版更新、按 AWJ 版本锁定”：本版准确来源和本地补丁身份见 [codec-dependencies.json](../cmake/codec-dependencies.json)。构建不动态解析 latest/main。
- Windows 使用 MSVC 静态运行库、AVX2、IPO/LTO；Linux 使用原生 GCC 16.1、x86-64-v3、IPO/LTO。具体构建与测试结果以验证记录为准。

## 已知边界

- JXL Gain Map bundle 重建留待后续版本。AVIF/HEIF ISO alternate ICC 合成空间和 AVIF 序列逐帧 Gain Map 尚不支持，会报错。
- Adobe JPEG Gain Map 的 RDF 数组 XMP 暂不支持；部分没有明确合成色彩标签的 Gain Map 也会报错，不退回普通主图转换。
- 动画输出限于 AVIF；其他目标仅输出首帧，并提示动画未保留。多帧视觉质量搜索 1–99 不支持；100 使用现有无损量化语义。
- 速度 7–10 使用 libavif 的实时编码路径，速度 10 对应 AOM CPU 9。播放兼容性只报告实际验证的查看器。

## 发行归档

提供 `AWJ_Win.7z`（AWJ.exe、AWJ.com、LICENSE、NOTICE.txt）和 `AWJ_Linux.7z`（AWJ、LICENSE、NOTICE.txt）。归档不含用户配置；精确成员、大小、SHA-256 和签名更新清单以正式发行验收为准。
