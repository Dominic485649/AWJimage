# 1.3.0：SDR/HDR 呈现、ISO Gain Map 与动画 AVIF

本页描述 1.3.0 的呈现合同。历史 [1.2.0 验证记录](validation-1.2.0.md) 和各次开发候选的证据保留原样；编码验证与第三方查看器、实机显示验证分别记录。实际运行范围以对应版本验收记录为准。

## 按真实呈现与目标格式自动编码

位深、宽色域、浮点载体、显示提亮和“曾来自 Gain Map”的来源标记，都不独自证明当前存在 SDR/HDR 两个端点。普通单层保持单层；原图没有可重建 HDR 的有效信息时，不凭空给 SDR 增加 HDR。

| 输入 | JXL / AVIF | PNG | JPG auto/hdr | JPG sdr / WebP |
| --- | --- | --- | --- | --- |
| 单 SDR，包括零 HDR 提升的 SDR→SDR | SDR 单层，无 Gain Map | SDR 单层 | 普通 SDR JPEG | SDR 单层 |
| 单 HDR | HDR 单层，无 Gain Map | HDR 单层 | 自动高光滚降生成 SDR base，再用 ISO Gain Map 保留 HDR | 自动高光滚降为 SDR 单层 |
| 原图真实 SDR/HDR 两端，底图方向不限 | HDR 主图＋重建原 SDR 的反向 Gain Map | 所选较高 HDR 单层 | 原 SDR base＋HDR Gain Map | 直接复用原 SDR，不重新 tone-map |
| 只有 HDR/HDR 两端 | 所选较高 HDR 单层，不保留另一 HDR 为备用 SDR | 所选较高 HDR 单层 | 从较高 HDR 自动生成 SDR base＋HDR Gain Map | 从较高 HDR 自动滚降为 SDR |

只有实际持有有效、不同动态范围的两端，JXL/AVIF 才输出 Gain Map；相同范围端点不产生伪双呈现。JPEG 因其普通主图不能单层存储 HDR，保留自动生成 SDR base 的例外。`--jpeg-hdr hdr` 遇到 SDR-only 输入也保留普通 SDR，而不是失败或只改 HDR 标签。

“最佳”是按真实动态范围选择已有或正确重建的端点，不新增 AI 增强或审美评分，也不以峰值阈值把暗 HDR 降成 SDR。正常有效输入不因目标只能存 SDR 而失败；损坏输入、无效颜色、功能性元数据、预算不足、明确不支持的参数组合和取消仍保留既有错误处理。

### 高光滚降与颜色

缺少可复用 SDR、且目标需要 SDR 时，复用 libplacebo spline tone mapping 和 perceptual gamut mapping，按有效亮度分析映射到 100-nit SDR sRGB；不是把高光硬截到整数范围。已有原 SDR 时优先复用该呈现，不从 HDR 再创造不同的 SDR。

- 原 HDR 主图的合法 PQ/HLG、P3/BT.2020 解释保持不变；必须重建或从线性工作像素编码时，执行真实的像素转换，不仅改标签。PNG 默认 SDR 8-bit、HDR 16-bit，显式 8/16 只改变精度，不隐含 SDR tone-map。
- JPEG Gain Map 重建的线性 FP16 以 **203 nits** 为参考白，不是 80-nit scRGB。默认取有效 metadata 允许的较高 HDR headroom。
- ISO gamma、min/max、offset、headroom、灰度/多通道及 map 重采样由保留的 libultrahdr（JPEG）或 libavif（AVIF/HEIF/JXL）路径完成；重新计算的双呈现不宣称整对逐样本无损。
- 保留主图 alpha；必要时先将 premultiplied alpha 转为 straight。辅助图 ICC 不充当主图颜色配置。借用源像素保持只读，转换使用独立工作存储。
- JPEG Exif 方向、AVIF clap/irot/imir、HEIF 主图/辅助图变换进入像素处理，归一 Exif orientation，避免重复旋转；辅助图无独立变换时共用主图方向。
- 原 ICC 只在仍描述当前像素时保留；已验证等价的 ICC 可归一到 CICP。不能把旧 Gain Map 色彩/XMP 标签贴给重建像素。strip 仅去可选 metadata，不移除解释主图、另一呈现或增益图所必需的信息。

## ISO-only：协议与引擎分离

AWJ 仅接受 **ISO 21496-1 Gain Map**，保留固定版本 libultrahdr 作为 JPEG 数值和封装引擎，不改变上游版本、许可证、头文件或来源署名。`UHDR_WRITE_ISO=ON`、`UHDR_WRITE_XMP=OFF` 控制输出；`cmake/patch_libultrahdr_iso_only.cmake` 删除底层 `parseGainMapMetadata()` 的 XMP decoder fallback，不能只凭输出开关宣称 ISO-only。

补丁匹配完整原始或已补丁函数；未知、部分修改、重复函数或额外 XMP fallback 拒绝配置。FetchContent、缓存和源码覆盖使用同一合同，Windows/Linux 不分叉。线程预算补丁保持独立，锁文件记录补丁及 CRLF→LF 归一化 SHA-256。

| 输入 | 实现入口 | 边界 |
| --- | --- | --- |
| JPEG/JPG | 有界 APP 候选扫描 → libultrahdr probe/decode | ISO 数值和合法性由补丁后的库解析，不另写 JPEG ISO parser；普通文件只检查短 pre-SOS 头 |
| AVIF | libavif gain-map content → 公共 ISO 呈现选择 | 有效 headroom、明确 CICP 或支持且已验证的 ICC 运算空间 |
| HEIC/HEIF | libheif item/reference API → 公共 ISO 呈现选择 | version 0 tmap 与两个有序 dimg；坏 fraction、含糊关系、不支持的 alternate profile 明确失败 |
| JXL | 标准 jhgm／brob bundle → 公共 ISO 呈现选择 | 读取主图、增益图和另一呈现颜色；支持重新编码 HDR 主图与反向 Gain Map |
| 旧 Adobe/Google/Apple Gain Map | 最小拒绝特征 | 不重建旧私有协议，不静默输出 SDR 主图 |

### 路由与失败合同

- 纯 ISO 正常重建；ISO＋旧 XMP 只按 ISO 解释；坏 ISO＋有效旧 XMP 仍失败，不能借旧协议成功。
- legacy-only 是终止性“不支持旧 Gain Map”，不是 not-my-format。文件、memory、直接 codec、registry 和尺寸探测共用 gate；`Gain Map:` 前缀阻断普通 JPEG/WIC 回退。
- 命名空间声明本身不是 legacy HDR。普通 XMP、ICC、Exif、普通 MPF/MPO 不应被误拒绝。MPF 是候选而非 HDR 证据，ISO 必需的 MPF 不删除。
- Apple auxiliary URN 可能标记同一有效 ISO tmap 引用的辅助图：先判断 ISO 关系，同图按 ISO；缺少 ISO 关系的 legacy auxiliary 明确拒绝，不同 map 的歧义也失败。
- 普通图片和 PQ/HLG/scRGB 不因 ISO-only 规则被全局禁用，不全局删除普通 XMP、ICC 或 Exif。

## HDR JPEG 输出

`--jpeg-hdr auto|sdr|hdr` 默认 auto。Studio、预设和右键保持自动策略。auto/hdr 对真实 HDR 优先取原 SDR；缺少 SDR 时自动高光滚降，再由 JPEGli 编码、解码真实 SDR 样本作参考。内部 `UltraHdr::generateGainMap()` 生成增益与 ISO metadata，JPEGli 编码辅助图，最后由 libultrahdr C encoder 封装 ISO＋MPF。sdr 仅输出已有或滚降得到的 SDR 单层；SDR-only 输入在三种模式下都不伪造 HDR。

对外 codec/integration 为 `jpegli-iso21496-1`，metadata 为 `iso21496-1-gain-map`。底图是真实 8-bit SDR sRGB，HDR reference 为 FP16/203-nit。实际 SDR base 的 RGB/RGBA/未知/灰度 auto 使用三分量 444；已知 YUV 采样沿用原规则，显式 400 拒绝。辅助图使用 444、baseline，关闭 XYB/自适应量化。必要 ICC 在 strip 下仍保留，旧 Gain Map XMP 不复制。

Gain Map 默认缩小倍率 4，小尺寸降低倍率；质量跟随 quality。HDR Gain Map JPEG 暂不支持 visual-quality，q100 不声明 JPEG 无损。headroom 依据 canonical 实际 RGB 分量峰值，不用 MaxCLL 或 CIE Y 代替；capacity_min=1，最小正 headroom 为 1/1024 stop，生成和 ISO 重读均验证 metadata。超过引擎范围仍明确报告，不能夹掉高光后冒称完整 HDR。

## GIF / APNG / WebP / AVIF → 动画 AVIF

动画 AVIF 维持既有合同，与静态图的双呈现策略独立。GIF/APNG/WebP/AVIF 序列输出 AVIF 时自动保留动画；其他格式只处理首帧并提示。历史 `--preserve-animation` 仍接受但无需传入，首页不显示动画开关。

- 使用所选 AVIF 预设：默认 AOM、质量 70、速度 5、自动位深（8-bit GIF/APNG 通常 10-bit，16-bit APNG 限制 12-bit），RGB/RGBA auto 444，灰度 APNG auto 400。透明 alpha 保留，除非明确移除。
- 多帧不支持 visual-quality 1..99，明确失败；100 保持既有无损量化语义。单帧输出静态 AVIF。
- `AnimationReader` 顺序提供完整画布，像素借用至下一次 next；Gain Map 仍为静态解码，不复用动画时序抽象。
- GIF 使用 giflib 的局部帧、色表、交错、BACKGROUND/PREVIOUS；Plain Text/user-input 扩展失败。APNG 校验 CRC/顺序号/画布/frame rect，处理 SOURCE/OVER/NONE/BACKGROUND/PREVIOUS，默认 PNG poster 不作为动画帧。
- GIF 零/缺失 delay 使用 10ms；APNG 分母 0 按 100，分子 0 使用 1ms；WebP 零 delay 使用 1ms。有限/无限循环跟随输入，AVIF 未知 loop 信息按播放一次处理。
- timescale 取分母最小公倍数并限制为 1,000,000；长帧进一步约束为 AVIF 32-bit sample delta，按累计时间舍入。单帧退化、多帧 8/10/12-bit/alpha 保持。
- ICC/CICP 保留，PNG gAMA/cHRM 缺少完整配置时按既有规则生成 RGB ICC。PNG 透明 OVER 在编码样本空间处理，颜色变换在完整画布之后。
- 动画不使用静态 Grid；尺寸限制在完整画布之后逐帧应用。超出单帧 AV1 限制且未缩小时失败。
- 每动画串行解码/编码，文件间仍用原调度。仅保留压缩输入、canvas、当前帧和 PREVIOUS，计入帧索引、合成压缩帧、libavif packet/sample buffers 及 Finish/输出复制。
- libaom 预算为保守估计，不是第三方 allocator 硬上界。帧间可取消，同步 codec 调用结束后再响应取消；播放兼容性与编码正确性分别验收。
- CLI 高级参数 `--avif-animation-tune auto|ssim|psnr`、`--avif-animation-keyframe N` 保留；Studio、预设、右键不保存。速度 7–10 用实时编码，10 对应 CPU 9。

## 依赖与定向验证

依赖身份仍由 `cmake/codec-dependencies.json` 固定，源码压缩包按原始字节校验，补丁/overlay 文本按 UTF-8、CRLF→LF 归一化；本次策略不更换依赖。不绕过来源 HEAD、补丁 hash 或配置门禁。

| 验收 | 入口 | 范围 |
| --- | --- | --- |
| 自动呈现策略 | `rendition_policy_core` | 跨平台真实内存／文件闭环，单层、双呈现、零提升、HDR/HDR、JPEG 例外与 WebP 滚降，含只读、坏端点和取消 |
| ISO 解码合同 | `iso_gain_map_core` | ISO/旧协议 gate、两种 base 方向、ICC、HEIF/AVIF 和零 headroom |
| HDR 数学／headroom | `jpeg_hdr_iso_core`、`hdr_tonemap_cpu` | 数学 oracle、峰值、预算、取消、有限显示模型和 ISO 重读 |
| JXL／AVIF 双呈现 | `jxl_codec_core`、`avif_gain_map_core` | HDR 原主图、SDR 重建、strip、Grid、alpha 和借用端点不改写 |
| JPEG 兼容性 | `jpegli_codec_core` | 独立 libjpeg-turbo 可读 SDR 主图，不代表旧协议成功 |
| C ABI 内存编码 | `sdk_native_encode` | 单帧五格式、FP16/PQ/HLG、JPEG Gain Map 和 SDR 自动滚降，不改变 ABI v1 |

仅按实际改动运行相关目标，不例行重跑不相关协议传输、更新器或全量动画测试。Windows/Linux 在各自原生工具链验证，开发候选附源码指纹、实际日志与未验证项。根目录 `bin/x64/` 是保护目录，不作为构建、测试或暂存输出。

## 上游依据

- [libultrahdr v2.0.2 API](https://github.com/google/libultrahdr/blob/v2.0.2/ultrahdr_api.h)、[ISO decoder 接入点](https://github.com/google/libultrahdr/blob/v2.0.2/lib/src/jpegr.cpp)。保留引擎，依赖补丁去除旧 XMP fallback。
- [libavif v1.4.2 API](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/include/avif/avif.h)、[Gain Map 数学](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/src/gainmap.c)。
- [PNG 第三版 APNG 规范](https://www.w3.org/TR/png-3/#11APNG)；仍使用普通 libpng，不引入 APNG patch、Exiv2、spng 或 FFmpeg。
