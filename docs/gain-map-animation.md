# 1.2.3：ISO 21496-1 Gain Map 与动画 AVIF

本页描述当前协议合同。历史 [1.2.0 验证记录](validation-1.2.0.md) 不代表 1.2.3 已完成构建或运行验收。本轮并行修改期间只执行隔离的补丁契约和静态检查；下述 C++/实际转换验收须在源码稳定后另行运行。

## ISO-only：协议与引擎分离

AWJ 仅接受 **ISO 21496-1 Gain Map**，保留固定版本的 **libultrahdr** 作为 JPEG 数值和封装引擎。没有换库、改动上游版本或删除其许可证、头文件与来源署名。`UHDR_WRITE_ISO=ON`、`UHDR_WRITE_XMP=OFF` 控制输出；另以 `cmake/patch_libultrahdr_iso_only.cmake` 删除底层 `parseGainMapMetadata()` 的 XMP decoder fallback，不能只靠编码开关声称 ISO-only。

补丁匹配完整的原始或已补丁函数，未知、部分修改、重复函数或额外 XMP fallback 均拒绝配置。首次 FetchContent、已填充缓存和源码覆盖目录都会执行同一合同；Windows/Linux 不分叉。独立的线程预算补丁保留，可按任意顺序组合。锁文件同时记录两个补丁及 CRLF→LF 归一化 SHA-256；其他依赖 pin 不变。

| 输入 | 实现入口 | 当前边界 |
| --- | --- | --- |
| JPEG/JPG | 有界 APP 候选扫描 → libultrahdr `uhdr_dec_probe()` / `uhdr_decode()` | ISO 数值与合法性由补丁后的库权威解析，不另写 JPEG ISO parser；普通文件只检查短 pre-SOS 头 |
| AVIF | libavif `AVIF_IMAGE_CONTENT_GAIN_MAP` / `avifRGBImageApplyGainMap()` | ISO 21496-1；要求明确 CICP，暂不支持 ICC 合成空间 |
| HEIC/HEIF | libheif item/reference API → libavif 合成 | version 0 `tmap` 和两个有序 `dimg` 引用；未知版本、坏 fraction、含糊关系、ICC alternate profile 明确失败 |
| 旧 Adobe/Google/Apple Gain Map | 仅保留最小拒绝特征 | 不重建、不解析其私有 XMP 数值，不静默输出 SDR 主图 |
| JXL Gain Map bundle | 尚未实现 | 不提供 bundle 重建能力 |

### 路由与失败合同

- **纯 ISO** 正常增强；**ISO + 旧 XMP** 只由 ISO 解释，旧元数据不影响像素；**坏 ISO + 有效旧 XMP** 失败，绝不借旧协议成功。
- legacy-only 是明确“不支持旧 Gain Map”的终止错误，不是 not-my-format。文件、memory、直接 JPEG/JPEGli codec、registry 与尺寸探测使用同一 gate；`Gain Map:` 错误前缀阻断普通 JPEG/WIC 回退，对外格式名为 ISO 21496-1。
- 命名空间声明本身不是 legacy HDR。普通 XMP（包括未使用的 HDR 命名空间）、ICC、Exif、普通 MPF 和普通 MPO 不应被误拒绝。MPO 可正常读取主图；MPF 是候选而非 HDR 证据，ISO 所需 MPF 不删除。
- Apple auxiliary URN 可能标记同一个有效 ISO `tmap` 引用的辅助图。先判断 ISO 关系，同图按 ISO 合成；没有 ISO 关系的 Apple legacy auxiliary 明确拒绝。不同 map 引起的歧义仍失败。
- 普通图片、PQ/HLG/scRGB 输入不因删除旧 Gain Map 协议而被全局禁用。普通 metadata 的保留/剥离继续遵守原有选项，不全局删除 XMP、ICC 或 Exif。

### 像素、颜色与几何

增强后输出普通 HDR 像素，不输出独立 Gain Map/sidecar；HDR JPEG 从增强像素重新生成 ISO Gain Map。

- JPEG 的线性 FP16 以 **203 nits** 为参考白，不是 80-nit scRGB。默认使用所写 metadata 允许的最大 HDR headroom。
- ISO gamma、min/max、offset、headroom、灰度/多通道与 map 重采样由保留的 libultrahdr（JPEG）或 libavif（AVIF/HEIF）完成。项目旧 Apple composer 和私有 XMP parser 调用已删除。
- 保留主图 alpha；必要时在编码空间先将 premultiplied alpha 转为 straight。辅助图 ICC 不充当主图颜色配置。
- JPEG Exif 方向、AVIF clap/irot/imir、HEIF 主图/辅助图变换进入像素处理。Exif orientation 归一为 1，避免重复旋转；辅助图没有独立变换时，主图变换在重建后统一应用。
- 原主图 ICC 和旧 Gain Map XMP 不复制到重建像素，避免贴错色彩标签；适用的 Exif 保留。普通 SDR 解码 metadata 不受影响。
- ISO AVIF/HEIF 重建为 BT.2020/PQ，保留适用的 CICP/CLLI；剥离可选 metadata 不抹去这些必要解释。不能通过标签覆盖把同一像素伪装成其他原色/传递函数。

| 输出 | 行为 |
| --- | --- |
| AVIF | 增强图默认 10-bit，可显式 8/10/12-bit；BT.2020/PQ 与适用 CLLI |
| JXL | 现有 BT.2020/PQ 高位深像素编码，不携带 Gain Map bundle |
| JPG | 8-bit SDR base + JPEG Gain Map，ISO 21496-1 + MPF；base 始终标为 SDR sRGB，而非伪装 PQ。auto 按已知源 YUV420/422/444 保留，RGB/RGBA/未知/灰度使用三分量 444；显式 400 拒绝 |
| WebP | 现有 libplacebo tone/gamut 路径转换至 100-nit SDR sRGB、8-bit |
| PNG | 增强图默认 SDR sRGB 8-bit；`--bit-depth 16` 在线性/浮点 tone 后直接量化至 16-bit；普通非 Gain Map PNG 位深规则不变 |

```powershell
.\bin\1.2.3\Windows\AWJ.com -i "photo.jpg" -o "output" -f avif
.\bin\1.2.3\Windows\AWJ.com -i "photo.heic" -o "output" -f png --bit-depth 16
```

## HDR JPEG 输出

`--jpeg-hdr auto|sdr|hdr` 默认 auto。Studio、预设、右键固定 auto，不展示此开关。HDR 输入先由 libplacebo tone/gamut mapping 生成 SDR base，再由 JPEGli 编码、解码真实样本作参考。内部 `UltraHdr::generateGainMap()` 生成增益与 ISO metadata，JPEGli 使用 444、baseline、关闭 XYB/自适应量化编码辅助图，最后由 libultrahdr C encoder 封装 ISO + MPF。

对外 `codec_name` / `integration` 为 `jpegli-iso21496-1`，`metadata` 为 `iso21496-1-gain-map`。内部库名、API 和许可证保留。主图为真实 8-bit SDR sRGB，HDR reference 为 FP16/203-nit；不能通过改标签宣称 HDR。

Gain Map 默认缩小倍率 4，小尺寸降低倍率；质量跟随 quality。必要的主图/alternate ICC 即使 strip 仍保留，旧 Gain Map XMP 不复制。HDR JPEG 拒绝所有 visual-quality 请求，q100 不声明 JPEG 无损；显式 sdr 执行 tone mapping，显式 hdr 无有效 HDR 输入时报错。

headroom 使用 canonical 实际 RGB 分量峰值，不以 MaxCLL 或 CIE Y 代替；`capacity_min=1`，最小正 headroom 为 1/1024 stop，生成与 ISO 序列化重读均验证 metadata。超过 10000 nits 明确失败，不夹高光或悄悄转 SDR。精确数学和性能验收以专门 HDR 测试及后续真实样本结果为准。

## GIF / APNG / WebP / AVIF → 动画 AVIF

动画 AVIF 维持既有合同，与本轮删除旧 Gain Map 协议无关。GIF/APNG/WebP/AVIF 序列输出 AVIF 时自动保留动画；其他格式只处理首帧并提示。历史 `--preserve-animation` 仍接受但无需传入，首页不显示动画开关。

- 使用所选 AVIF 预设：默认 AOM、质量 70、速度 5、自动位深（8-bit GIF/APNG 通常 10-bit，16-bit APNG 限制 12-bit），RGB/RGBA auto 444，灰度 APNG auto 400。透明 alpha 保留，除非明确移除。
- 多帧不支持 visual-quality 1..99，明确失败；100 保持既有无损量化语义。单帧输出静态 AVIF。
- `AnimationReader` 顺序提供完整画布，像素借用至下一次 `next()`；Gain Map 仍是静态解码，不复用动画时序抽象。
- GIF 使用 giflib 的局部帧、色表、交错、BACKGROUND/PREVIOUS；Plain Text/user-input 扩展失败。APNG 校验 CRC/顺序号/画布/frame rect，处理 SOURCE/OVER/NONE/BACKGROUND/PREVIOUS，默认 PNG poster 不作为动画帧。
- GIF 零/缺失 delay 使用 10ms；APNG 分母 0 按 100，分子 0 使用 1ms；WebP 零 delay 使用 1ms。有限/无限循环跟随输入，AVIF 未知 loop 信息按播放一次处理。
- timescale 取分母最小公倍数并限制为 1,000,000；长帧进一步约束为 AVIF 32-bit sample delta，按累计时间舍入，不能表示时失败。单帧退化、多帧 8/10/12-bit/alpha 保持。
- ICC/CICP 保留，PNG gAMA/cHRM 缺少完整配置时按既有规则生成 RGB ICC。PNG 透明 OVER 在编码样本空间处理，颜色变换发生在完整画布之后。
- 动画不使用静态 Grid；尺寸限制在完整画布之后逐帧应用，不能缩放局部 rect 后误合成。超出单帧 AV1 限制且未缩小时失败。
- 每动画串行解码/编码，文件间仍用原调度。只保留压缩输入、canvas、当前帧与 PREVIOUS；计入帧索引、合成压缩帧及 libavif packet/sample buffers，Finish/输出复制额外预留。
- libaom 预算是保守估计，不是第三方 allocator 硬上界。帧间可取消，一次同步 codec 调用完成后再响应取消。浏览器/系统播放兼容性与编码正确性分别验收。
- CLI 高级参数 `--avif-animation-tune auto|ssim|psnr`、`--avif-animation-keyframe N` 保留；Studio、预设、右键不保存。速度 7–10 用实时编码，10 对应 CPU 9。

## 依赖与验证入口

`cmake/codec-dependencies.json` 保留本轮既有版本/tag/完整 commit、源码 SHA-256、baseline、overlay 和许可身份，应用版本同步 1.2.3，仅新增 ISO-only 补丁/hash。源码压缩包哈希按原始字节，本地补丁/overlay 文本按 UTF-8、CRLF→LF 归一化。CMake 检查版本、baseline、源覆盖 HEAD 与补丁 hash，失败即停止。

| 验收 | 入口 | 范围 |
| --- | --- | --- |
| 无构建补丁合同 | `tests/ultrahdr_iso_patch_tests.cmake` / CTest `ultrahdr_iso_patch_contract` | 首次、重复、7 种未知/破损形状拒绝、线程补丁双序组合；真实依赖只读，修改其 sandbox 副本 |
| ISO 解码合同 | `tests/iso_gain_map_tests.cpp` / `iso_gain_map_core` | JPEG ISO/混合旧 XMP/坏 ISO/legacy、普通 XMP/ICC/Exif/MPO、底层 probe+decode、文件/memory/direct/registry/回退屏障；ISO HEIF 与 Apple 同图、普通 alpha；ISO AVIF/alpha/坏 fraction |
| HDR 数学/headroom | `jpeg_hdr_iso_core`、`hdr_tonemap_cpu` | 固定正确 oracle、边界/预算/取消、headroom/有限显示模型与生成后重读 |
| 兼容性 | `jpegli_codec_core`（保留 `tests/jpeg_hdr_compat.cpp`） | 独立 libjpeg-turbo 可读 SDR 主图，不是旧协议成功测试 |
| 原有几何/动画 | `avif_aom_codec_core`、`native_pipeline_core` | 8 种 Exif 方向、crop、ISO gamma/offset/alpha、203-nit 参考白、VFR/loop/位深等 |

源码稳定且允许构建后，在隔离输出中运行，例如 Windows：

```powershell
cmake --preset windows-msvc-x64-release -B build/iso-contract -DBUILD_TESTING=ON -DAWJ_RUNTIME_OUTPUT_ROOT="$PWD/build/iso-test-output"
cmake --build build/iso-contract --config Release --target awj_iso_gain_map_tests awj_jpeg_hdr_iso_tests awj_avif_aom_codec_tests
ctest --test-dir build/iso-contract -C Release -R '^(ultrahdr_iso_patch_contract|iso_gain_map_core|jpeg_hdr_iso_core|avif_aom_codec_core)$' --output-on-failure
```

不要将当前并行编辑阶段的静态检查写成 C++ 编译/实机转换已通过。Linux 在原生工作区独立配置/验证，不跨 Windows/WSL 构建；根目录 `bin/x64/` 是保护目录，不作为本轮构建、测试或暂存输出。

## 上游依据

- [libultrahdr v2.0.2 API](https://github.com/google/libultrahdr/blob/v2.0.2/ultrahdr_api.h)、[ISO decoder 接入点](https://github.com/google/libultrahdr/blob/v2.0.2/lib/src/jpegr.cpp)。保留引擎，依赖补丁去除旧 XMP decoder fallback。
- [libavif v1.4.2 API](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/include/avif/avif.h)、[Gain Map 数学](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/src/gainmap.c)。
- [PNG 第三版 APNG 规范](https://www.w3.org/TR/png-3/#11APNG)；仍使用普通 libpng，不引入 APNG patch、Exiv2、spng 或 FFmpeg。
