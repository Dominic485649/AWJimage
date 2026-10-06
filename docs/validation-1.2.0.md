# AWJimage 1.2.0 发行验收

验收日期：2026-10-07。正式发行源码以 Git tag `1.2.0` 为准，依赖由该 tag 中的锁定文件指定。两平台程序从同一源码构建；精确提交、归档及成员哈希记录在发行说明和签名更新清单中。

## 构建与回归

| 平台 | 构建 | 已执行结果 |
| --- | --- | --- |
| Windows x64 | VS 2026 / MSVC 19.51，Release，静态运行库，AVX2，IPO | Release 编译通过；独立 Validation 配置 53/53 通过 |
| Windows Explorer | 真实本机 Explorer，独立菜单测试 | 1/1 通过；测试备份并恢复 AWJ 所属注册表项 |
| Linux x64 | Ubuntu 24.04 WSL 原生目录，GCC 16.1，x86-64-v3，IPO | Release 编译通过；独立 Validation 配置 34/34 通过 |

正式候选使用 `BUILD_TESTING=OFF`；测试在独立构建目录运行。Windows 测试排除了实时屏幕捕获测试 `raw_wgc_stdin_core`，Explorer 菜单测试单独串行运行。未将一次 CLI 或单元测试通过等同于全部系统查看器兼容。

回归包含 GIF disposal 2/3、局部帧/色表、APNG SOURCE/OVER 与 BACKGROUND/PREVIOUS、独立 poster、RGBA16、透明、WebP blend/dispose、有限/无限循环、VFR、单帧退化、10/12-bit AVIF、PQ/BT.2020、时长量化、资源预算、取消与坏 CRC。配置、预设和菜单参数有序列化及命令回归；400 强制灰度、alpha 保留和 UI 中没有动画调优/关键帧控件有回归检查；Windows/Linux Studio 软件渲染启动检查通过。

## Gain Map 与 HDR JPEG

- HDR JPEG 通过独立 libjpeg-turbo 普通 JPEG 解码器读取 SDR 主图，并通过 libultrahdr 重建线性 FP16 HDR。1×1、3×3、65×65 常量 RGB 基准的平均相对误差分别为 **0.1519%、0.2894%、0.4143%**。这些是小型数值回归，不是所有照片的质量保证。
- 小型 Gain Map 数值回归覆盖 ISO gamma、offset、headroom、不同尺寸、alpha 与无效 fraction/gamma/ICC，以及 Apple legacy 的 headroom、缺 metadata、非灰度拒绝；8 种 Exif 方向和归一后不重复旋转均通过像素断言。
- PQ、HLG 与 FP16 输入测试通过。HDR JPEG 的视觉质量 100 请求、强制 HDR 处理 SDR、内存不足及取消均明确失败；失败不会写出最终文件。
- 使用锁定的 [libavif v1.4.2 测试数据](https://github.com/AOMediaCodec/libavif/tree/v1.4.2/tests/data)：`apple_gainmap_old.jpg`、`apple_gainmap_new.jpg`、`seine_sdr_gainmap_srgb.avif`、不同尺寸的 `seine_sdr_gainmap_big_srgb.avif`、HDR base 的 `seine_hdr_gainmap_small_srgb.avif` 增强后输出 HDR JPEG 成功。`draw_points_idat_progressive.avif` 按静态图转换，没有误判动画。
- `paris_exif_xmp_gainmap_bigendian.jpg` / `seine_sdr_gainmap_srgb.jpg` 的 RDF 数组 XMP，以及缺少合成颜色标签的 `color_grid_gainmap_different_grid.avif`，按已知边界明确失败。
- HEIF 原始实样 `IMG_1159.HEIC` 关联不同 Apple/ISO map，明确拒绝多 map；`IMG_1626.heif`、`IMG_1632.heif` 使用 ISO alternate ICC，明确拒绝该未支持合成空间。
- Apple legacy 的实样派生验证：复制 `IMG_1626.heif`，只将 offset 1533 的 item type `tmap` 四字节改为 `free`，保留 primary 49、Apple auxiliary 63、原始压缩像素和 XMP/Exif。成功经 `libheif-apple-gain-map-enhanced` 输出 `jpegli-ultrahdr-iso`。**这是用于隔离 legacy 路径的派生文件，不声明原始照片在默认 ISO 路径成功。** 原始文件不修改。

样本、哈希、日志与浏览器证据保存在本地 `build/preparation-1.2.0/`，不把用户/上游实样加入产品归档。HEIF 样本来源：[IMG_1159.zip](https://github.com/user-attachments/files/15864812/IMG_1159.zip)、[HEIF-images.zip](https://github.com/user-attachments/files/16734152/HEIF-images.zip)。

## 动画播放与真实 CLI

GIF、APNG、WebP 和 AVIF sequence 均经过真实 AWJ CLI；自动保留动画，无需传入保留开关。默认预设和显式 10-bit 路径均有覆盖。已有 FFprobe 仅用于独立检查，没有成为产品依赖：四帧输出的 color/alpha sequence tracks、10-bit、10/20/30/40ms 时长和总时长 100ms 得到确认。

在 **Chrome 154.0.8037.98** 中实际显示四帧 10-bit、4:4:4、带透明通道的动画 AVIF，用 CDP 分时截图得到四个不同帧。浏览器使用临时独立 profile，不读取用户 profile。验证文件每帧 500ms；截图覆盖两秒播放。没有使用 `canvas.drawImage()` 的静态默认图结果判断动画是否播放。

尚未实测 Safari、Edge、Windows Photos 的动画/HDR 显示，也未进行实体 HDR 显示器亮度测量。这些查看器与显示设备的兼容性不在本轮已验证范围内。

## 性能与边界

- Apple legacy 大图在 `--threads 1 --max-jobs 1` 下 HDR JPEG 总耗时约 **306.65 秒**，其中解码约 3.24 秒、编码约 303.40 秒。CPU 色调映射是当前高分辨率 SDR base 生成的性能限制；该耗时不是动画编码速度基准。此轮保持现有 spline/perceptual 颜色策略。
- 自有逐帧、逐行处理检查取消；第三方单次编解码调用仍可能要等该调用返回。压缩帧数据和合成画布纳入资源预算。
- JXL Gain Map、ISO alternate ICC、Adobe RDF 数组 XMP、AVIF sequence 每帧 Gain Map 尚不支持。多帧视觉质量搜索 1–99 不支持；非 AVIF 动画输出仅首帧并提示。
- Explorer 普通排序已验证没有 `Position` 强制值，不能保证在任意机器上恰好紧邻 7-Zip。

## 交付与复现

发行程序位置：`bin/1.2.0/`；Windows 可执行程序为 `AWJ.exe` / `AWJ.com`。归档使用 `AWJ_Win.7z`、`AWJ_Linux.7z`，仅包含程序、LICENSE 与 NOTICE.txt，无用户配置。

依赖身份和补丁锁定在 [codec-dependencies.json](../cmake/codec-dependencies.json)。Git tag `1.2.0` 固化程序源码；Windows 与原生 Linux 使用同一 tag 构建。发行验收检查归档完整性、精确成员、解压后 SHA-256、版本及帮助命令；发布后再次下载核对资产。结果记录在本地 `bin/1.2.0/release-evidence.json`。

历史 release notes 和历史 changelog 保持原样。正式发行新增 1.2.0 tag、Release 及递增 sequence 的签名归档更新清单，兼容别名保持相同字节；不改历史 Release 资产。真实自动更新安装及实体 HDR 显示器验收未在本轮执行。

## 正式发行资产

发布时间（UTC）：10/06/2026 16:51:40。源码提交：`587c0745d9df31a7f81d8b2dec85d27a31a84b97`。签名归档更新清单 sequence **12**，发布密钥 `release-2026`，有效期 `10/06/2026 16:49:39` 至 `02/03/2027 16:49:39`。公开下载重新计算 SHA-256 并通过 7z 完整性检查。

| 归档 | 字节数 | SHA-256 |
| --- | ---: | --- |
| AWJ_Linux.7z | 19455350 | `dde97141f9bf111a7c6bc072f3a212369c6017349eb210d335f89757232c8985` |
| AWJ_Win.7z | 11934920 | `6b97c5699b584d7eb632bb36337f8d359ff3fdd1ae9a93e7635507a9cebffb21` |

| 包 | 成员 | 字节数 | SHA-256 |
| --- | --- | ---: | --- |
| Windows | AWJ.com | 450048 | `4a71f0c99cf35b79f7084fc3dddc6ec2651b3e5b899793eefe813305c7a3ace3` |
| Windows | AWJ.exe | 42322432 | `ae2bd8cfdd8a63c66112beda58f2426ca05c44091ca22efc09a6eb57fbc729b1` |
| Windows | LICENSE | 35184 | `6f1e622c82a380075843bb084a7ec3b1f1d12a4a02526d75e78b0924a860aa75` |
| Windows | NOTICE.txt | 142168 | `fe068ded1d9ac895da6b741500d228b0d598bfade14bcf9f7a1f4eb807ca012d` |
| Linux | AWJ | 67516784 | `2db767160c05569d168619e9441ef6167b82c07ab2a45299246847c85dce094c` |
| Linux | LICENSE | 34523 | `8486a10c4393cee1c25392769ddd3b2d6c242d6ec7928e1414efff7dfb2f07ef` |
| Linux | NOTICE.txt | 139580 | `ae5ede84a8a9f464113986e3353b39b9f7b166a26d2e540d29d6fb69729b58cb` |
