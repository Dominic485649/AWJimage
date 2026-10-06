# 1.2.0：Gain Map 增强与动画 AVIF

这是 1.2.0 功能与实现说明。实际构建与验收状态见 [验证记录](validation-1.2.0.md)。

## Gain Map 输出语义

转换时自动识别支持的 Gain Map，读取主图和辅助样本，依据元数据在线性光空间重建增强图像。不输出独立 Gain Map 或 sidecar；HDR JPEG 从增强像素重新生成嵌入式 Gain Map，供兼容解码器显示 HDR。

| 输入 | 实现入口 | 当前边界 |
| --- | --- | --- |
| JPEG/JPG | libultrahdr `uhdr_dec_probe()` / `uhdr_decode()` | ISO、标量 Adobe XMP、Apple 与 MPF 由上游解析；Adobe RDF 数组形式暂不支持并明确失败；普通 JPEG 仅额外扫描短文件头 |
| AVIF | libavif `AVIF_IMAGE_CONTENT_GAIN_MAP` / `avifRGBImageApplyGainMap()` | ISO Gain Map；当前合成要求明确 CICP，ICC 合成空间会报错 |
| HEIC/HEIF Apple | libheif auxiliary URN + libultrahdr Apple metadata parser + AWJ 合成 | legacy 8-bit 灰度 map；XMP version 与 XMP/Exif headroom 必须有效；支持 RGB ICC 或明确 CICP 主图 |
| HEIC/HEIF ISO | libheif item/reference API + libavif 合成 | version 0 `tmap`、两个有序 `dimg` 引用；未支持的 metadata 版本、ICC alternate profile、含糊引用会报错 |
| JXL Gain Map bundle | 后续实现 | 本轮升级 libjxl，但未增加 bundle 重建能力 |

- 无 Gain Map 时使用正常转换管线。已识别但无法正确重建时失败，不经过 WIC 普通解码回退，也不标记“已增强”。HEIF 关联多个不同 map 时失败；同一个辅助图同时具有 Apple 与 ISO 表达时优先 ISO。
- 默认使用元数据允许的最大 HDR headroom。libultrahdr 的线性 FP16 以 **203 nits** 为参考白；它不会被当作以 80 nits 为参考白的 scRGB。
- libultrahdr 的小型线程预算适配遵守当前文件 worker 的解码线程额度；不同文件使用 thread-local 配置，避免互相覆盖。
- ISO 的 gamma、min/max、offset、headroom、灰度/多通道及 map 重采样由 libavif 处理。Apple legacy map 先对编码样本进行双线性尺寸映射，再执行 Rec.709 逆传递与 `1 + (headroom - 1) * gain` 增强。
- 保留主图 alpha；在需要时先将编码空间中的 premultiplied alpha 转为 straight。合成不把辅助图 ICC 当作普通图像色彩配置。
- JPEG 的 Exif 方向、AVIF 的 clap/irot/imir、HEIF 的主图与辅助图变换进入像素处理，输出 Exif orientation 归一为 1，避免重复旋转。辅助图没有独立变换时，主图变换在合成后统一应用。
- 原主图 ICC 和 Gain Map XMP 不写入重建输出，避免给新像素贴旧色彩标签；保留适用的 Exif，`--strip-metadata` 仍控制输出元数据。
- HDR AVIF 重建的 BT.2020/PQ 标签及适用的 CLLI 保留，即使剥离可选 metadata。不能用 CICP 覆盖把这些像素简单重新标为其他原色/传递函数；这样的请求会报错。

| 输出 | 行为 |
| --- | --- |
| AVIF | 增强图默认 10-bit；重建输出为 BT.2020/PQ，写入适用的 CICP/CLLI；可显式选择 8/10/12-bit |
| JXL | 使用现有 BT.2020/PQ 高位深像素编码，不携带 Gain Map bundle |
| JPGLI | 默认 auto：SDR 普通 JPEGli，HDR 生成 8-bit SDR base + JPEGli Gain Map，封装 ISO 21496-1 + MPF；CLI sdr 强制 tone mapping |
| WebP | 通过现有 libplacebo spline + perceptual gamut mapping 转为 100-nit SDR sRGB、8-bit |
| PNG | 增强图默认 SDR sRGB 8-bit；可指定 `--bit-depth 16`，在浮点色调映射后直接量化为 16-bit；普通非 Gain Map PNG 维持现有位深规则 |

```powershell
.\bin\x64\Release\AWJ.com -i "photo.jpg" -o "output" -f avif
.\bin\x64\Release\AWJ.com -i "photo.heic" -o "output" -f png --bit-depth 16
```

## GIF / APNG / WebP / AVIF → 动画 AVIF

GIF/APNG/WebP/AVIF 序列输出 AVIF 时自动保留动画，CLI 和 Studio 均无需开关，首页不展示动画选项。早期开发命令中的 `--preserve-animation` 仍接受，但已无需传入。其他输出格式仅处理首帧，并提示该限制。

动画直接使用所选 AVIF 预设。内置默认：AOM、质量 70、速度 5、自动位深（8-bit GIF/APNG 通常输出 10-bit，16-bit APNG 限制为 12-bit），RGB/RGBA 画布自动选择 4:4:4，灰度 APNG 自动使用 4:0:0，颜色表示为 YUV；可选择 400 强制灰度 AVIF，透明通道保留，除非明确请求移除。动画 tune 默认 auto，不覆盖上游 tune；最大关键帧间隔默认 0（自动）；帧时长和循环次数跟随输入，不固定为某个 FPS。用户预设可覆盖质量、速度、位深、色度、颜色及 alpha 策略。

多帧动画不支持视觉质量搜索 1..99，会明确失败而不退化为单图；视觉质量 100 沿用现有无损量化语义。单帧输入仍输出静态 AVIF。

```powershell
.\bin\x64\Release\AWJ.com -i "animation.gif" -o "output" -f avif --bit-depth 10
.\bin\x64\Release\AWJ.com -i "animation.apng" -o "output" -f avif
```

- `AnimationReader` 顺序提供完整画布；帧像素借用到下一次 `next()` 为止。Gain Map 仍是静态图像解码操作，不复用动画时序抽象。
- GIF 继续用 giflib 解码局部帧，处理全局/局部色表、交错、透明、BACKGROUND 与 PREVIOUS。每帧释放 giflib 的 SavedImage/color-map 副本。Plain Text 与 user-input 图形扩展会明确失败。
- APNG 用小型 chunk 层校验 CRC、顺序号、画布与 frame rect，再将局部帧封装为内存 PNG 交给 libpng。处理 SOURCE/OVER、NONE/BACKGROUND/PREVIOUS；默认 PNG poster 不属于动画时不编码它。
- 保留正帧时长。GIF 的零时长或缺失 delay 使用 10ms；APNG 分母 0 按 100，分子 0 使用 1ms，结果提示会说明调整。不会把其他短 delay 自动变成浏览器的最小播放时长。
- GIF Netscape loop 按首次播放后的重复次数转换；APNG `num_plays` 转换为重复次数；无 GIF loop 为播放一次，0 为无限循环。超出 libavif repeat API 范围的数值报错。
- timescale 使用约分后分母的最小公倍数，上限 1,000,000；长帧时进一步降低上限，确保每帧时长适合 AVIF 的 32 位 `stts sample_delta`。超过上限时按累计时间舍入，避免逐帧误差累积。单帧退化为静态 AVIF；多帧支持 8/10/12-bit 与透明 alpha。
- 保留 ICC/CICP。PNG gAMA/cHRM 在缺少 ICC/cICP/sRGB 时生成 RGB ICC；只有 gamma 或 chromaticity 的文件使用 sRGB 原色或 gamma 2.2 补足未声明部分。未声明颜色的 GIF/PNG 沿用 sRGB 默认。透明 OVER 在 PNG 编码样本空间计算，颜色变换发生在完整画布之后。
- 动画不使用静态 Grid。尺寸限制在完整画布合成后逐帧应用，保证局部 rect/disposal 使用原始坐标。超出单帧 AV1 限制且未缩小时失败。
- 每个动画串行解码/编码；文件之间继续使用原有调度。帧进度进入 CLI 与 Windows/Linux Studio 回调，第一帧、最后一帧及每 16 帧更新一次。
- 保留压缩输入、canvas、当前局部帧与 PREVIOUS，避免保存所有原始帧。预算也计入帧索引、APNG 合成的压缩帧和 libavif 已积累的 packet/sample buffers；Finish 与输出复制预留额外空间。
- libaom 内部工作区使用保守估计，内存预算不是对第三方 allocator 的硬上限。解码/画布/帧间支持取消；正在执行的一次 codec 调用完成后才能响应取消。
- 浏览器或系统查看器可能只显示首帧、忽略有限循环或自行限制短 delay。编码正确性与播放兼容性需要分别验收。

## 依赖与复现

新增 **libultrahdr**；现有 libavif/aom/dav1d、libheif/libde265、JPEGli/libjpeg-turbo、giflib/libpng、libjxl/lcms 均保留。lcms 在 manifest 中从间接依赖改为显式依赖。没有引入 Exiv2、spng 或 FFmpeg。

`cmake/codec-dependencies.json` 记录本次选定的版本、tag、完整提交、实际源码包 SHA-256、补丁与 overlay 校验值。vcpkg 继续使用固定 baseline 与端口自身的 SHA-512。JPEGli 无稳定 release，保留已审核提交并记录例外及子模块；giflib 使用官方 release tarball 的校验值作为源码身份。

本地补丁/overlay 哈希对 UTF-8 文本先将 CRLF 归一为 LF，避免 Windows/Linux checkout 的换行差异导致假漂移；源码压缩包使用原始字节哈希。

每次准备 AWJ 新版本时：

1. 检查各库官方稳定发布，选择当时最新稳定版；不要让构建自动解析 `main/latest`。
2. 下载真实源码归档，确认不是 HTML/错误页，校验内容并记录 SHA-256；将 tag 解引用为完整 commit。不同 URL 的压缩归档不能互用校验值。
3. 更新 JSON、vcpkg baseline/必要 overlay、补丁及其 SHA-256；检查上游已合入的补丁并删除重复补丁。源码覆盖目录与子模块也必须来自锁定提交。
4. 编译并按下表验收。若最新稳定版不能通过构建/兼容性验收，在锁定文件记录版本暂缓原因。
5. 将已审核记录与源码变更一同存入 Git。后续从该 AWJ 提交重建，使用记录的版本，不重新选择最新依赖。

CMake 会检查 AWJ 版本、vcpkg baseline、缓存中的 Git revision、Git 源码覆盖目录的 HEAD 与本地补丁校验值是否一致。锁定文件变更会触发重新配置。正式发行从同一干净的 1.2.0 tag 构建两平台程序，归档校验通过后发布签名更新清单。

开发版本继续以 `VERSION` 为源码版本、`vcpkg.json` 为 manifest 版本；此次均为 1.2.0。历史 release notes、签名更新清单、历史 archive manifest 不随开发版本修改。Windows/Linux 打包脚本从版本锁定文件读取 pin，发行状态见验证记录。

## 验收入口与范围

现有 `tests/avif_aom_codec_tests.cpp` 已增加小型回归检查：203-nit Gain Map 参考白与 80-nit scRGB 的区别，长 APNG 时长的 timescale、四帧 VFR、有限/无限循环、10-bit、首帧不透明后续透明和帧进度。`tests/native_pipeline_tests.cpp` 覆盖不传动画开关、内置默认预设下两帧 GIF 自动输出两帧 AVIF。运行结果与真实文件验收见验证记录。

检查入口（先以 `BUILD_TESTING=ON` 配置，再构建对应目标）：

```powershell
cmake --preset windows-msvc-x64-release -DBUILD_TESTING=ON
cmake --build build/x64/Release --config Release --target awj_avif_aom_codec_tests awj_native_pipeline_tests
ctest --test-dir build/x64/Release -C Release -R '^(avif_aom_codec_core|native_pipeline_core)$' --output-on-failure
```

| 范围 | 必须覆盖 | 判据 |
| --- | --- | --- |
| Gain Map | JPEG ISO/XMP、AVIF、Apple HEIF、ISO tmap；灰度/RGB、不同尺寸、gamma/offset/headroom | 与对应上游重建参考对齐；输出没有辅助 map；HDR 标签与像素一致 |
| 几何/颜色 | 8 种 Exif 方向、irot/imir/clap、独立辅助图变换、ICC、alpha | 仅变换一次、尺寸正确、alpha 保留；未支持色彩空间明确失败 |
| 降级/损坏 | 无 map、多 map、坏 XMP/ISO fraction、缺数据、未知版本 | 无 map 普通转换；坏/未支持 map 不伪装增强、不走普通回退 |
| GIF | disposal 2/3、局部矩形、局部色表、交错、透明、有限/无限 loop、零 delay | 逐帧画布与参考渲染相同；loop/时长符合上述策略 |
| APNG | 所有 blend/dispose、poster、局部帧、RGBA8/16、分数 delay | 帧数不包含独立 poster；画布、alpha、VFR 与参考一致 |
| AVIF sequence | 单帧、多帧 10-bit、ICC/CICP、首帧不透明后续透明 | 单帧正常退化；后续 alpha 不丢失；累计时长误差不超过 1 tick |
| 资源/异常 | 超帧数、CRC/sequence 错误、越界 rect、截断、超预算、取消、Grid 冲突 | 明确失败或取消，无错误最终输出；工作区与压缩包增长有预算限制 |

## 上游依据

- [libultrahdr API](https://github.com/google/libultrahdr/blob/v2.0.2/ultrahdr_api.h)、[`getMetadataFromXMP`](https://github.com/google/libultrahdr/blob/v2.0.2/lib/src/jpegrutils.cpp)。Apple metadata parser 是私有 C++ 接口，升级需要复核。
- [libavif API](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/include/avif/avif.h)、[Gain Map 实现](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/src/gainmap.c)、[sequence 编码示例](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/apps/avifenc.c)。
- [Apple HDR effect](https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos)。
- [PNG 第三版 APNG 规范](https://www.w3.org/TR/png-3/#11APNG)；使用现有普通 libpng，不依赖 APNG patch。

## HDR JPEG

`--jpeg-hdr auto|sdr|hdr` 默认 auto。Studio、预设和右键固定 auto，不显示此开关。
HDR 输入先由 libplacebo tone mapping 生成 SDR 主图，再由 JPEGli 编码、解码实际样本作为参考。`UltraHdr::generateGainMap()` 生成多通道数值及元数据；JPEGli 用 444、基线 JPEG、关闭 XYB 和自适应量化编码 Gain Map，再通过 `uhdr_enc_set_compressed_image()` / `uhdr_enc_set_gainmap_image()` / `uhdr_encode()` 封装 ISO 21496-1 + MPF。

主图为 8-bit SDR，内部 HDR 保持 FP16、203-nit 参考白。Gain Map 默认缩小倍率 4，小尺寸降低倍率；质量跟随 quality。必需的主图与 alternate ICC 即使 strip 仍保留，旧 Gain Map XMP 不复制。HDR JPEG 拒绝所有 visual-quality 请求；q100 不声明 JPEG 无损。显式 sdr 执行 tone mapping；显式 hdr 在无有效 HDR 输入时报错。

动画高级参数 `--avif-animation-tune auto|ssim|psnr` 与 `--avif-animation-keyframe N` 仅在 CLI 提供；Studio、预设和右键不保存这些选项，动画默认自动。速度 7–10 使用实时编码，10 对应 CPU 9；WebP 零 delay 使用 1ms，AVIF 未知 loop 信息按播放一次处理。
