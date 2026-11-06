# AVIF 渐进式与 CLI 分层

渐进式是同一张静态图的多个 AV1 层，不是动画、线程分割或 Grid。支持的查看器可以在文件尚未收齐时先显示预览层，再显示最终层；其他查看器可能只显示最终图。普通 AVIF 支持不代表渐进呈现支持。

## 自动模式

只需增加 `--avif-progressive`。默认关闭，`--no-avif-progressive` 明确保持普通单层编码。

自动模式沿用 libavif 的 avifenc 策略：两层，首层横纵各缩小到 1/2、颜色质量 10，最后一层恢复完整输出尺寸与请求质量；alpha 质量不会跟随首层自动降到 10。目标颜色质量低于 10 时自动模式明确拒绝，可改用手动层参数。

```bash
AWJ --input input.png --output output --avif-progressive
```

自动模式不进行内容分类、额外质量搜索或体积阈值筛选。通常会增加体积、耗时和内存；应根据实际输入和分发环境评估。

## 手动模式

- `--avif-layers 2..4` 指定总层数，序号从 1 开始。
- `--avif-layer-size 序号:宽x高` 指定该层像素尺寸。
- `--avif-layer-scale 序号:分子/分母` 按最终输出尺寸向上取整缩放，与同层 size 互斥。
- `--avif-layer-quality 序号:1..100` 指定颜色质量。
- `--avif-layer-alpha-quality 序号:1..100` 指定 alpha 质量。

预览层至少明确指定尺寸、比例或质量；未指定尺寸的质量层采用完整尺寸，允许同分辨率的质量渐进。未指定质量沿用普通请求质量。各层尺寸不能递减或超过最终尺寸；最后一层必须保持经过普通几何处理后的完整输出尺寸。指定像素尺寸时按该尺寸重采样，不额外填充画布。

```bash
AWJ --input input.png --output output --avif-layers 3 --avif-layer-scale 1:1/4 --avif-layer-quality 1:10 --avif-layer-scale 2:1/2 --avif-layer-quality 2:35
```

```bash
AWJ --input input.png --output output --avif-layers 2 --avif-layer-size 1:640x480 --avif-layer-quality 1:20 --avif-layer-alpha-quality 1:80
```

自动与手动模式不能同时启用；层参数需要明确总层数，越界、重复、无效比例或混填均报错。手动不同尺寸输入使用锁定的上游预缩放层支持，不把 AOM 的有限 scalingMode 比例冒充任意分辨率。

`--visual-quality` 的搜索与指标针对最后一层；搜索 1..99 时不能同时固定手动最后一层的颜色质量。q100 仍只沿用目标 AVIF 像素的无损量化语义，不承诺原 RGB 码流无损。

## 组合边界

渐进式目前用于普通静态 AVIF。Gain Map、多帧动画和现有 Grid 路径的组合明确报错，不会悄悄丢弃增益图或动画。请求渐进后不再进行原始 AVIF 码流直通。输入分层 AVIF 做普通转换时读取最终层，不能把各层当作动画帧。

当前 CLI 已有完整控制，但尚未添加 UI 控件。是否在 Studio 展示，取决于样图及实际使用评估；默认编码行为不变。

CSV 的 `avif_layer_count` 和 `avif_layer_trace` 记录实际采用的层计划。trace 中的尺寸为编码计划尺寸；解码器可能将首层上采样到最终画布，因此逐层解码的呈现尺寸不一定是底层编码尺寸。

## 内存与依赖

渐进编码会保留跨层参考、状态与前层码流，不能把单层内存除以层数。内存计划 v2 分别记录必需像素、转换、codec 余量、跨层保留和输出容量；解码后按实际 alpha/采样等精化，预算仍是准入预算而不是第三方 allocator 硬限制。

1.3.2 使用 AOM 3.15.3；libavif 以稳定 1.4.2 为基线，回补上游固定提交 `768b3dfe30da63265eb31dad6f79fc7662f65b42` 的 AOM 库预缩放分层支持。源码、回补与完整文件指纹由依赖锁固定，不在构建时追随 main。

## 上游依据

- [libavif 1.4.2 avifenc](https://github.com/AOMediaCodec/libavif/blob/v1.4.2/apps/avifenc.c)
- [预缩放分层支持提交](https://github.com/AOMediaCodec/libavif/commit/768b3dfe30da63265eb31dad6f79fc7662f65b42)
- [渐进呈现互操作提案](https://github.com/web-platform-tests/interop/issues/1459)
