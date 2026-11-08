# libaom 帧内工具与 IQ：1.3.5 参数决定

日期：2026-11-09。适用于本项目锁定的官方 **libaom 3.15.3 + libavif 1.4.2**，不是 SVT-AV1 或 psy 分支。

## 采用决定

保留静态有损 `color:tune=iq`，不新增 `auto-intra-tools-off=1`，不冗余设置四个 `enable-…-intra=1`，不为启动 auto 将 IQ 的 deltaq 从 6 改成 3。没有改变生产质量、速度、动画调优或公开 C ABI。

完整编码器构建中，以下四项默认已是 1，auto 默认是 0。允许工具不等于所有速度档均完整搜索；speed features 仍会剪枝。

| 参数 | 控制内容 |
|---|---|
| `enable-smooth-intra` | 平滑帧内预测 |
| `enable-paeth-intra` | Paeth 帧内预测 |
| `enable-cfl-intra` | 从亮度预测色度（CFL） |
| `enable-diagonal-intra` | 对角方向帧内预测 |

## 自动关闭的实际机制

`auto-intra-tools-off=1` 既不是打开这四项，也不是无条件关闭。Wiener 预分析同时满足 `qindex < 128`、估计 bpp > 2、重建失真/像素 < 4 时，才将这四项一起置零；它不关闭所有帧内预测，也不涉及 palette/filter-intra 等其他工具。

在当前版本中，分析入口要求 `deltaq-mode=3` 且未进入 non-RD；IQ 设置 `deltaq-mode=6`。因此两开关可同时被接受，但普通 IQ 不执行该自动关闭分析，并非 parser 明示互斥报错。覆盖 deltaq=3 已偏离完整 IQ 配方，不能把它带来的体积变化归因于 auto。

auto=1 还使该 Wiener 预分析使用串行路径。显式四项=1 不能阻止稍后的自动置零，也不能取消速度档剪枝；高速度 non-RD 路径不执行该分析。官方 3.14.0 源码已有同样函数与阈值，但其更新日志没有单列此开关，本报告不据此断言首次引入版本。

libavif 对适用的静态图自身也可能默认 IQ，故非 IQ 实验必须显式选择 PSNR/SSIM，不能只关闭 AWJ 的 IQ bool。选项按实际插入顺序调用，实验限定 `color:` 前缀，不改变 alpha 的调优。

## 对照与结果

最终独占矩阵共 **1542 次**：1532 次成功编码并由 dav1d 解码，10 次精确预期拒绝，零意外失败；另有 14 次预检。每次编码前后核对输入、工具及依赖摘要；重复设置的码流/画质不一致为零。

主矩阵包含六种照片、截图、插画和纹理样本，speed=5、Q70/90、8-bit 420、最多四线程，每设置三次交错重复；边界覆盖较慢档、speed6/8、单/多线程、1024 级图像、8/10/12-bit、420/444/400、透明与无损。真实照片来自 libavif 测试数据，包括狗/草地、Paris 和 Kodak；JPEG 来源不称为未压缩原始采集。

以下为**同质量参数**的体积几何变化，不是等视觉质量压缩率。

| 对照 | 主矩阵体积变化 | 结论 |
|---|---:|---|
| IQ/deltaq6，auto0→1 | 0.00% | 12/12 设置完整 AVIF 与画质相同 |
| IQ/default，四项显式1 | 0.00% | 12/12 设置同码流；默认已允许四项 |
| IQ，四项全部关闭 | +5.29% | 最大 +16.28%；不是通用压缩优化 |
| IQ，deltaq6→3，auto仍0 | −11.44% | Kodak Q90 PSNR −2.308 dB，不是 auto 的收益 |
| IQ→deltaq3，auto0→1 | +0.70% | 12 组中 9 组同码流；触发不保证变小 |

逐项关闭 smooth、Paeth、CFL、diagonal 的主矩阵体积变化分别为 +0.27%、+1.15%、+3.01%、+1.07%。高质量单线程真正触发的 auto 对照体积范围为 −1.10%～+7.10%；例如 noise256/Q99 从 102703 B 增至 103136 B（+0.42%）。少量 Q 曲线匹配 PSNR 后，deltaq6→3 的体积估计仅 −0.09%～+3.11%，不支持等质量两位数收益。

### 计时边界

首轮 477 次计时与维护计算重叠，只作信息。追加 240 次聚焦复测，239 次未观察到其他 WSL 计算干扰，79/80 设置有完整三次；另一次受干扰计时排除。

- 聚焦八组手动全0的观测编码耗时约 −20.19%，但体积 +4.48%、PSNR 中位 −0.130 dB。
- Q95/99、单线程、IQ→deltaq3 的八组 auto 对照观测耗时约 −17.48%，逐组 −31.83%～+3.57%，仍有体积和质量代价。
- 同码流 IQ 负对照仍有明显时间波动，未绑核或观测宿主频率/调度。这些是本批观测，**不是确定、普适的因果加速承诺**。

## 预设与回归

- 普通静态有损保持 IQ/deltaq6，工具沿用编码器默认1。
- quality=100 或渐进末层100 继续按最终质量走无损路径，不显式设置 IQ。强制 IQ Q100 的 chroma-deltaq/lossless 拒绝属于预期负例。
- 18 次生产式无损对照的目标 YUV/alpha 一致，identity 样本 RGB MSE=0；不外推为所有颜色转换均逐样本无损。
- 动画 auto/SSIM/PSNR、Grid、Gain Map 与渐进层不增加全局工具开关；复杂动画/分层的独立测量仅为 smoke。
- [AOM 回归](../tests/avif_aom_codec_tests.cpp) 实际编码、解码并比较 IQ 默认、auto0/1、四项显式1，覆盖 8/10/12-bit、420/444/400 与 alpha。完整版本的跨平台验收另行记录。

## 证据与复现范围

完整输入、工具、原始结果和回执保留于原生 `~/AWJimage/results/intra-tools-1.3.5/final/` 与 `~/AWJimage/tools/intra-tools-1.3.5/final/`；Windows 必要统计位于项目本地 `results/intra-tools-1.3.5/`。复制不代表 Windows 重跑实验或重新验证原生 inode/mtime 绑定。

最终检查覆盖 3215 个交付文件、1545 个 AVIF 摘要和 1237 个旧文件原始摘要。早期并发改写输入产生的假差异已作废、原文保留，未混入结论。真实 HDR、完整4K语料、主观盲测及应用整批吞吐不在结论范围内。

完整最终报告 SHA-256：`bfc1d19aee9fd9f2a118b26f0827c22ce23936ae413bdf2e4d3a95bfe5d48f2c`。

## 上游依据

- [3.15.3 参数与 IQ 设置](https://aomedia.googlesource.com/aom/+/refs/tags/v3.15.3/av1/av1_cx_iface.c)
- [3.15.3 自动分析](https://aomedia.googlesource.com/aom/+/refs/tags/v3.15.3/av1/encoder/allintra_vis.c)
- [3.15.3 调用入口](https://aomedia.googlesource.com/aom/+/refs/tags/v3.15.3/av1/encoder/encoder.c)
- [3.15.3 速度剪枝](https://aomedia.googlesource.com/aom/+/refs/tags/v3.15.3/av1/encoder/speed_features.c)
- [3.14.0 自动分析](https://aomedia.googlesource.com/aom/+/refs/tags/v3.14.0/av1/encoder/allintra_vis.c)
- [3.14.0 参数](https://aomedia.googlesource.com/aom/+/refs/tags/v3.14.0/av1/av1_cx_iface.c)
- [3.14.0 更新日志](https://aomedia.googlesource.com/aom/+/refs/tags/v3.14.0/CHANGELOG)
