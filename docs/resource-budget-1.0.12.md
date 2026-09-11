# 1.0.12 资源调度实测

Windows x64 本机执行，线程预算 8；每格一次观察。桌面与构建负载存在变化，以下用于核验预算和线程分配，不作为稳定速度排名。CPU 核数为进程 CPU 时间 / 墙钟时间；峰值 commit 使用 Windows PeakPagedMemorySize64，工作集使用 PeakWorkingSet64。

样本：large 为四张 4096² RGB；mixed 为四张 2048×1536 加两张 4096²；grid 为 65537×64 与 65539×64；stress 为两张 4096² 全噪声 RGBA（8/16-bit 各一张）。原始输入、可执行文件 SHA-256 与逐任务日志保存在对应 build/evidence/1.0.12 目录。

| 编码 | 样本 | 预算 MiB | 成功/拒绝 | 每图线程 | 秒 | 平均忙核 | 峰值工作集 MiB | 峰值 commit MiB |
|---|---|---:|---|---|---:|---:|---:|---:|
| AVIF q50/speed8 | large | 512 | 0/4 | — | 0.242 | 0.39 | 14.6 | 4.2 |
| AVIF q50/speed8 | large | 3072 | 4/0 | 8 | 2.703 | 1.88 | 792.6 | 1133.1 |
| AVIF q50/speed8 | large | 6144 | 4/0 | 4 | 1.816 | 2.97 | 1564.6 | 2254.6 |
| AVIF q50/speed8 | large | 12288 | 4/0 | 2 | 1.435 | 4.15 | 3101.1 | 4497.0 |
| AVIF q50/speed8 | mixed | 512 | 4/2 | 8 | 0.726 | 1.53 | 172.1 | 228.9 |
| AVIF q50/speed8 | mixed | 3072 | 6/0 | 2/8 | 1.578 | 2.33 | 792.7 | 1133.2 |
| AVIF q50/speed8 | mixed | 6144 | 6/0 | 2/4 | 1.122 | 3.17 | 1564.2 | 2254.8 |
| AVIF q50/speed8 | mixed | 12288 | 6/0 | 2/4 | 1.264 | 3.15 | 1564.3 | 2254.6 |
| AVIF q50/speed8 | grid | 128 | 0/2 | — | 0.194 | 0.40 | 13.6 | 4.2 |
| AVIF q50/speed8 | grid | 1024 | 2/0 | 8 | 0.701 | 1.67 | 126.1 | 140.3 |
| AVIF q50/speed8 | grid | 2048 | 2/0 | 4 | 0.482 | 2.56 | 233.2 | 272.8 |
| AVIF q50/speed8 | grid | 4096 | 2/0 | 4 | 0.501 | 2.47 | 233.5 | 272.8 |
| AVIF q50/speed8 | stress | 512 | 0/2 | — | 0.565 | 0.72 | 14.0 | 4.2 |
| AVIF q50/speed8 | stress | 3072 | 2/0 | 8 | 14.176 | 3.96 | 887.3 | 1200.5 |
| AVIF q50/speed8 | stress | 6144 | 2/0 | 4 | 14.684 | 4.12 | 1680.3 | 2328.9 |
| AVIF q50/speed8 | stress | 12288 | 2/0 | 4 | 11.209 | 5.43 | 1679.3 | 2325.6 |
| PNG q100 | stress | 256 | 0/2 | — | 0.505 | 0.68 | 14.5 | 4.2 |
| PNG q100 | stress | 1024 | 2/0 | 1 | 9.588 | 0.94 | 315.5 | 349.6 |
| PNG q100 | stress | 2048 | 2/0 | 1 | 5.933 | 1.25 | 386.0 | 428.6 |
| PNG q50 | stress | 256 | 0/2 | — | 0.492 | 0.73 | 14.0 | 4.2 |
| PNG q50 | stress | 1024 | 2/0 | 1 | 9.812 | 0.94 | 315.6 | 349.6 |
| PNG q50 | stress | 2048 | 2/0 | 1 | 6.337 | 1.32 | 366.8 | 399.8 |

低预算格仅允许预期的内存拒绝，脚本同时校验任务计数、输出状态和峰值 commit 未超预算。修复前 AVIF 512 MiB 预算曾出现约 2254 MiB commit，PNG 256 MiB 预算曾出现约 472 MiB；本次估算调整后，相关大图均在解码前拒绝。

AVIF 估算为每像素 128 bytes 加每图 32 MiB，PNG 为每像素 32 bytes 加 32 MiB，均使用饱和算术。它们是实测加余量的调度估计，并非操作系统强制内存上限。其他格式未据此扩大性能声明。PNG 编码器本身为单线程，并发按整个任务预算安排。无需加入额外 AVIF 线程上限；逐图 token 调度保留到有新测量需求时。

数据文件 SHA-256：

- `build/evidence/1.0.12/resource-measurements-3/metrics.csv`：`d7684e66cc9b5df15ade4878ddd17fca8c5672eea57f0e94b3161cc6636f5564`
- `build/evidence/1.0.12/png-resource-q100-fixed/metrics.csv`：`5b15cf81fe05d03d7c073e2fe6eab5698002a780509274c328b72ac4528bbd55`
- `build/evidence/1.0.12/png-resource-q50-fixed/metrics.csv`：`fc3f53f6a6dcc85adc20fb6d4d19e3f9e66991fbbaec8e7ccd3bb60189b0d766`
