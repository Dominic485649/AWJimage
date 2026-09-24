# 1.1.0 开发与验证记录

状态：已形成当前唯一未签名候选，等待用户审核；本文不代表发布许可。

## 基线（2026-09-20）

- 工作分支：`codex/1.1.0`。
- 起点：远程 master `ef5f3afa8111f5c135cad1557f5ef35240b624ce`。
- 计划记录的 `572642c` 与此起点只有 README 差异，工作树在开始时干净。
- 现有 Windows `AWJ.exe`：43,161,600 bytes，版本 1.0.15。
- 现有发行配置：MSVC 19.51.36257.0，IPO OFF、AVX2 OFF。
- 重新运行既有 Release 二进制：update_archive、update_model、
  update_security_state、update_keyring、ui_smoke 均退出 0。
- 既有队列检查：1k 行/500 次更新 0.528 ms；10k 行/5k 次更新 39.539 ms；
  均通过页切换和模型释放检查。这是现有模型测试，不是导入或启动转换 benchmark。
- 以上旧二进制测试仅作比较基线，不能用于声明新代码已验证。

本轮原始 Windows 记录在 `build/evidence/1.1.0/`。

## Slint 1.18 迁移

- 精确锁定 1.18.0：`bd20dab8529add087b5cbc81aec70bf30861ae4c`。
- 删除上游已合入的 AccessKit component-lifetime backport。
- 保留仍有源码依据的静态导入、焦点重入、FilterModel 修复。
- 合并补丁入口；每一处替换都验证完整 before/after 锚点。
- FetchContent 显式源码覆盖也应用同一补丁，避免跳过 PATCH_COMMAND。
- Winit 按窗口与事件轮次收集原生 PathBuf，通过 `set_file_paths` 分发。
  回调前释放 RefCell 借用和窗口列表借用。
- Linux 队列、输入和输出拖放使用 `file_paths()`，去掉换行分隔与 UTF-8 往返。
  Windows 保留既有 OLE 文件导入链路。
- 测试输出改为遵守 `AWJ_RUNTIME_OUTPUT_ROOT`，隔离构建不覆盖基线测试二进制。
- UI smoke 增加中文、空格、换行及 Unix 非 UTF-8 路径的 DataTransfer 往返检查；
  队列测试扩展为 1k/10k/100k。

已验证：补丁在精确版本干净源码上应用成功；重复应用 SHA-256 不变；
破坏 Winit 锚点时明确失败。命令：

```text
cmake -DSLINT_SOURCE_DIR=<Slint checkout> -DTEST_DIR=<scratch directory> -P tests/slint_patch_tests.cmake
```

开发构建已通过（2026-09-21）：

| 项目 | Slint 1.18 未裁剪 | Slint 1.18 精简 |
| --- | ---: | ---: |
| Windows AWJ.exe bytes | 43,277,824 | 41,819,648 |
| Windows runtime Cargo packages | 268 | 217 |
| Linux runtime Cargo packages | 363 | 312 |

Windows 同配置精简减少 1,458,176 bytes（约 1.39 MiB，3.37%）。
两次开发构建均 testing ON、AVX2 OFF、IPO OFF；不能直接当作最终发行版体积。
精简样本 SHA-256：`200BFCBDFCA10AC3929DDE1A4BB31F67F6B95A34E80FDD2F9241FA509324AB1D`。
关闭 tray/live-preview/interpreter/实验性 Vello，保留 accessibility、software、FemtoVG；
仅移除 C++ runtime 的 image-default-formats，保留 PNG/JPEG/SVG。
依赖图用 `cargo tree --edges normal`，不计 host compiler 与测试依赖。

- Windows：主程序编译通过，3/3 CTest（patch、UI、1k/10k/100k 模型）通过；
  `winit-software`、`winit-femtovg` 原生窗口 smoke 均退出 0。
- Linux：完整 AWJ 编译通过；独立 UI harness 2/2 CTest 通过，
  两个 renderer 在 WSLg 的原生窗口 smoke 均退出 0。
- 精简后的 smoke 实际加载 PNG/JPEG/SVG，并覆盖中英文页面切换和 teardown；
  几何/参数矩阵/焦点/FilterModel 检查复用既有测试。
- 原生文件载荷已验证字节往返；真实桌面拖放、picker 用户交互尚未全部验收。
- 没有截图或屏幕捕获；原生窗口测试不等于完整人工 UI 验收。

## 构建环境与证据限制

- 原 Windows CMake cache 引用的 vcpkg 安装目录已不存在。
  开发构建把现有 `build/dependency-cache/1.0.13/packages` 中的包复制到独立安装布局；
  不将该开发构建宣称为 clean Release。
- Linux 使用 `/home/dominic/awjimage-1.1.0-dev` 独立开发目录，复制现有依赖布局，
  并补建 libarchive/libplacebo 及其缺失依赖。完整主程序已编译通过；
  Rust cargo 缓存与独立 UI harness 复用，这仍不是 clean Release。
- Windows 首次阶段 C 构建以 6 并行出现 MSVC C1060 堆空间不足；
  降至 2 并行后通过，未放宽代码编译检查。
- 不执行 F6、屏幕捕获、raw-WGC 或更新安装；不修改信任根或已签名清单。

## 队列、配置与平台进展

- Windows 开始转换直接从受保护的 UI 队列生成 ImageFile，不再复制完整 QueueImageItem。
  编码输入构建归入已有队列纯逻辑模块，供生产调用与直接测试共用。
- 原清空队列路径已通过 swap 释放队列、ID/run 索引和路径集合，无需重复修改。
- 两平台停止读取/持久化三个 v1 UI cache 字段；原 whitelist + 原子写自然清理旧键。
  v2 sequence/cache/keyring 字段仍保留；不修改独立防重放安全状态。
- 两平台更新安全、archive、keyring、用户预设回归均 4/4 通过。
  既有预设测试覆盖名称唯一性、默认项删除保护和菜单同步失败回滚。
- Windows 配置写盘复用已有 UniqueWin32Handle；失败路径先关句柄再删临时文件。
- Linux UI 使用 posix_spawnp argv 调用 fc-list、picker、目录/URL opener，
  捕获输出时检查退出状态，保留路径空白和原生字节；不再调用 system/popen。
  shell_quote 仍用于写入文件管理器的 shell 命令文本。
- Linux process 独立测试通过：特殊字符/非 UTF-8、1 MiB stdout、非零退出、
  信号退出、标准流关闭、无效命令及反复执行后的 fd/子进程回收。
- 发布脚本 v2 稳定版仅保留新目标；预发布额外保留最新 stable 声明，
  包括其 revoked 状态，避免稳定渠道失去入口或复活已撤销目标。
  离线选择与 JSON singleton-array 测试通过。旧已签名清单、v1 bridge、alias、keyring 不变。
- 配置写盘契约已通过：清理旧 UI 字段、保留 v2 字段、重复写入幂等；
  目标被锁定时拒绝替换，原文件字节不变且不遗留临时文件。
- 设置页增加手动检查更新，复用已有检查流程；检查期间按钮禁用。
  中英文 820/1220 宽度及 accessibility 回调检查通过。
- 预设迁移补充旧 optional 单元素数组、null、未知字段清理和重复保存幂等；
  非法数组/布尔类型拒绝加载且源文件不变。
- 上述修改后的相关 CTest：Windows 23/23（20.66 s），Linux 21/21（6.23 s）。
  这是原生路径修复前的结果，不能替代后续代码的验证。

## 队列输入构建 A/B

实际生产 `build_run_files`，legacy 模拟旧的完整 QueueImageItem 复制；
每组独立进程、同一数据。数字是单次观测，非稳定吞吐结论。

| 队列数量 | legacy peak private bytes | direct peak private bytes | legacy ms | direct ms |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 5,251,072 | 4,718,592 | 7.605 | 7.287 |
| 10,000 | 26,845,184 | 21,626,880 | 78.383 | 72.159 |
| 100,000 | 239,566,848 | 186,494,976 | 860.359 | 765.901 |

100k 峰值减少 53,071,872 bytes（约 50.6 MiB）。清空后 private bytes 分别
4,263,936 / 4,493,312，主要容器 capacity 为零。
此测量覆盖构建真实编码输入，不等于真实图片导入或完整 UI 常驻内存。
日志位于 `build/evidence/1.1.0/queue-{count}-{legacy,direct}.log`。

## 字体和 renderer

每次 200 次字体切换，150 ms 间隔，再恢复默认字体和空队列；独立进程采样。
Windows 同时采集 Working Set / Private Bytes，Linux 采集 RSS / private resident。
两种系统的 private 指标定义不同，不作跨系统绝对值比较。

| 平台 / renderer | 早期 private MiB | 后期 private MiB | 观察 |
| --- | ---: | ---: | --- |
| Windows software | 26.50（8–10s） | 27.31（30–32s） | WS 约 50.2→51.0 MiB |
| Windows FemtoVG | 170.58（10–20s） | 171.93（25–35s） | WS 约 123.52→123.57 MiB |
| Linux software | 68.11（8–10s） | 68.11（30–32s） | RSS 76.61 MiB |
| Linux FemtoVG | 216.32（8–10s） | 217.30（30–32s） | RSS 约 225.79→226.77 MiB |

本轮未证明无界增长，不增加强制缓存清理或 working-set trim。
200 次切换不能证明永不泄漏。Windows FemtoVG 首轮 50 s 超时，保留日志；
第二轮放宽至 120 s 后，32.58 s 完成。CSV/日志均在 `build/evidence/1.1.0/`。

## Linux 原生路径修复

原生拖放载荷之外，队列去重、输出命名及 UI 配置仍存在 wide/UTF-8 往返。
新增含 0xff/0xfe 文件名的测试在修改前真实失败，报 filesystem 字符转换异常；
改为平台原生 path::string_type 后，扫描、输出名和数字冲突后缀测试通过。
UI 缓存选择器/拖放的原生输入和输出路径，手工编辑才走文本归一化；
进度行按原生队列路径定位。补充中文手工路径、异常扩展名和真实运行清单编码验证。
Windows 公共路径修改编译通过，process/pipeline security/studio state 3/3 通过（3.26 s）。
Linux 完整 AWJ 编译通过；process/pipeline security/native path 3/3 通过（0.68 s）。
新增原生路径测试还执行真实 CLI JPEG→PNG 编码，输入和输出目录均含无效 UTF-8。
Linux CLI 使用内部 surrogate escape 保留非 UTF-8 参数字节，仅在路径边界恢复原生字节；
验证 0x80–0xff 全部字节、有效 Unicode、非法 UTF-8 surrogate 字节序列及 shell 多输入去重。
原有 Windows 专用短路径测试保留；可移植的 pipeline security 测试新增 Linux 构建。
相关日志为 `native-cli-full-{build,ctest}.log`、`native-argv-{build,ctest}.log`，
位于独立 Linux 开发目录；Windows 日志位于本地 evidence 目录。

## CPU / IPO（进行中）

- Windows Release preset 和 release.ps1 统一 AVX2 / IPO 默认 ON，
  保留 `-EnableLto:$false` 用于对照；显式请求 IPO 但工具链不支持时配置失败。
- Linux Release preset 原有 x86-64-v3 / IPO ON 保留；NOTICE 和双语 README 补充最低 CPU 要求。
- Release PowerShell、Linux shell 和 A/B Python 脚本语法检查通过。
- 已完成同源码 Windows baseline、AVX2、AVX2+IPO 及 Linux v3/no-IPO、v3/IPO 对照，结果见下。

## 当前候选（8107848）

- Windows clean Release 位于独立工作树 `D:/awj11-release`，Linux clean Release 位于 `/home/dominic/awjimage-1.1.0-candidate`；两者均从 `8107848b270ca4f3f8d042e5ae5de2c8e5a57cb2` 构建，版本均为 1.1.0。
- `scripts/package-release.ps1 -SkipManifests` 与 `scripts/package-linux-release.sh --candidate-head 8107848b270ca4f3f8d042e5ae5de2c8e5a57cb2` 的固定成员、归档完整性和解压哈希检查通过。完整产物和限制见 `build/evidence/1.1.0/candidate-report.md`。
- 当前源码测试构建 CTest 为 54/54；最终发行 cache 按计划关闭测试构建。未执行正式 tag、推送、签名 manifest 或公开发布。
- 真实跨窗口 Explorer 拖放仍缺人工验收；100k GUI 转换的短暂未响应和取消来源仍作为已知限制保留。
- 用户审核唯一候选后，才执行正式 tag、推送、签名 manifest 和发布。

## 补充静态分析

`cppcheck --enable=warning,performance,portability --language=c++ --std=c++23`
检查路径/进程涉及的 6 个文件，日志为 `build/evidence/1.1.0/cppcheck-paths.log`。
当前 Cppcheck 在三个 .ixx 的 `export namespace` 处报告解析错误，不能据此声称模块静态分析通过。
非模块部分仍只有既有 `remove_awj_thunar_actions(std::string xml)` 按值参数建议；
该函数有意修改并返回副本，未为消除告警改写 API。
系统头缺失信息被命令行排除，未在源码添加 suppress；C++ 模块以双平台编译和运行测试验证。

## 已完成的优化阶段验证

- Windows 同源码 AVX2 OFF / IPO OFF：41,829,888 bytes，
  SHA-256 `09f74155fea7ba6094d137f89a89b07a48bf2312b440bde24c19555d1364350f`。
- Windows AVX2 ON / IPO OFF：41,810,432 bytes，
  SHA-256 `a77820613b5326901d7ffedcc658b9ba451c2471b01a2878c592f70b330a4cb5`。
- Linux v3 / IPO OFF：66,644,592 bytes，
  SHA-256 `0a9ace62208c97361e6955a11c0658eaf5e21eb52ef5aefadb513268d8e28e9a`。
- Linux v3 / IPO ON：66,222,704 bytes，
  SHA-256 `666dc27eb70f01b7bead522d5babd7ef96e8b0b8a85ed025d553e4453fd20671`。
- Linux IPO 全部相关回归 32/32（11.33 s）；仅排除 raw_wgc_stdin_core。
  software / FemtoVG 原生 renderer smoke 均退出 0。
- Linux 实际链接含 `-O3 -flto=auto -static-libstdc++ -static-libgcc --gc-sections`；
  动态依赖仅 libvulkan、libm、libfontconfig、libc 和动态加载器。
- 百万像素 Visual Quality：Linux `vulkan-session` 实际 used=true、fallback_count=0；
  与禁用 GPU 的 CPU 路径均执行 5 个候选并选择 q95，输出 WebP SHA-256 相同：
  `ce0bc947a97b2203250e966bcc5f80f4092b47f699f75372c75ac39da9fe7969`。
  此高频样本 VQ90 未达标，两端均明确报告 closest-fallback；未将其误报为达标。
- 锁定的 libheif example.heic 和 with-alpha-512x512.heic 均实际转为 PNG。
  alpha 样本解码为 RGBA 512×512，alpha 极值 0..255，透明度未丢失。
- 以上为 testing ON 的测试构建；当前追加修复后的 clean 候选已完成，详见 `build/evidence/1.1.0/candidate-report.md`。
  编译期间的功能验证耗时不作性能结论。

## Windows IPO 与串行性能 A/B

Windows AVX2+IPO：41,592,320 bytes，SHA-256
`d7eed214b535731940e9700696d367c4cafa47cc4e0bb6734f687bb670defdf5`。
相对同源码 baseline 减少 237,568 bytes；实际编译记录含 `/GL /arch:AVX2`，
链接记录含 `/LTCG /OPT:REF /OPT:ICF`。
Windows IPO 相关回归 51/51（17.82 s），排除 raw-WGC 输入测试及会修改真实用户菜单的 Explorer 集成测试。
两个原生 renderer smoke 均退出 0；D3D11 session 实际 used=true、fallback_count=0，
与 CPU 及 Linux Vulkan 的样本输出 WebP 哈希一致。HEIC alpha 输出为 RGBA 512×512、alpha 0..255。

硬件：Intel Core Ultra 9 185H，系统报告 16 cores / 16 logical processors。
固定 1024×1024 RGB PNG，quality 70、4 threads；每组预热 1 次，测量 5 次，以下为墙钟中位数 ms。
无并行编译/编码干扰；Windows 首轮出现短样本波动，追加按格式交错、轮换版本顺序的复测，表内采用该复测。

| 平台/配置 | 启动 | PNG | WebP | AVIF | JXL | JPGLI |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows baseline | 107.24 | 279.39 | 231.99 | 966.89 | 172.51 | 171.93 |
| Windows avx2 | 108.91 | 289.90 | 232.95 | 978.95 | 173.09 | 203.32 |
| Windows ipo | 109.18 | 279.16 | 227.82 | 981.84 | 175.32 | 170.24 |
| Linux noipo | 54.16 | 205.48 | 104.08 | 912.35 | 104.34 | 53.49 |
| Linux ipo | 53.62 | 204.13 | 103.90 | 859.81 | 104.43 | 53.73 |

Windows 最终组合相对 baseline 变化约 -1.8%..+1.8%，采样区间重叠，未见稳定回退，也不宣称显著加速。
AVX2-only 的 JPGLI 样本偏慢，最终 IPO 组合恢复到 baseline 范围。
Linux 本次 AVIF 中位数降低约 5.8%，其余接近；短样本与桌面调度限制使该结果不适合推广为普遍收益。
每个平台内部五种格式输出均逐字节一致。

复现与原始数据：`build/evidence/1.1.0/benchmark-variants.py`（交错），
`benchmark-variants-sequential.py`（首轮），`benchmark-windows*/results.json`、`benchmark-linux-results.json`。
A/B 构建输入 SHA-256 保存在 `ab-source-hashes.json`，对照完成前复核无漂移。
此后仅为候选更新 VERSION/变更记录、vcpkg 项目版本、构建并行度和测试依赖声明。

## 2026-09-23 追加：更新残留、字体弹层和按钮布局

Windows 更新 helper 仅在新 `AWJ.exe` 发出启动健康信号且 3 秒内保持存活后写入
`committed` 状态并移除安装目录的事务指针。其后由安装目录内的新版本进程等待
旧 helper 退出，再清空 Known Folder `FOLDERID_LocalAppData` 下 `AWJimage`
目录的**全部内容**。清理前复核 stage 属于该目录、状态为 `committed`、
事务指针不存在、版本与当前二进制一致、旧 helper 的映像路径及登录会话匹配。
失败、回滚、恢复及普通启动均不进入清理入口。

从已发布 1.0.15 升级时，运行的仍是旧版 helper，它会在成功后删除
`state.txt`，因此新版本的健康启动进程还会为旧协议启动独立清理进程。
该进程先验证 `files-replaced` 状态、版本、事务指针及旧 helper 的路径和会话，
再等待旧 helper 以 0 退出、事务指针消失，随后清空目录；非零退出或回滚不清理。
新版 helper 在 stage 写入清理归属标记，避免新旧两条路径同时执行。

`update_windows_cleanup` 用 Windows 临时目录验证无状态、`prepared`、
`files-replaced`、`rolled-back`、`failed` 和提交但事务指针未移除的情况均不删除；
提交并移除指针后，旧 `shader_cache`、嵌套目录、任意文件和整个 `updates`
目录都被删除，`AWJimage` 根目录保留为空。旧协议回归还覆盖新 helper 标记、
helper 非零退出、回滚状态、事务指针仍存在与旧版成功删除状态文件的路径。
本机等价旧协议提交测试启动了新版 1.1.0 GUI，健康信号和 3 秒存活检查通过，
模拟旧 helper 以 0 退出后，真实 LocalAppData `AWJimage` 从 162 个文件、
2,230,940,168 bytes 变成空目录，事务指针消失。随后普通 `--version`
启动时，临时哨兵文件保留；移除哨兵后根目录仍为空。该测试覆盖实际新版本
启动与提交后清理，但不包含远端下载、正式签名验证或真实旧版 helper 换装。

`SoftComboBox` 将 hover 高亮与 ScrollView 滚动位置分离：打开时定位当前项，
鼠标移入/跨项不再改变滚动，键盘 Home/End/方向键才按需保持高亮可见。
长字体列表的 Slint 原生弹层回归实际分发鼠标移动、滚轮和点击事件，
并检查已选字体、多个项目、滚动位置、Home/End/Enter/Escape。
设置页 1440/1834 像素窗口下通过 `ui_smoke` 的 Slint accessibility geometry
核对“检查更新”按钮紧邻“更新后显示更新日志”且同高；窄窗口自动换行。

Windows `build/1.1.0-slint` 使用 x64-v3 与 IPO 完整 Release 构建；
本次 CTest 51/51 通过（包括更新清理和字体弹层回归），按计划未运行
raw-WGC 输入与两个真实 Explorer 菜单测试。原始记录在
`build/evidence/1.1.0/post-legacy-dev-ctest.log`。真实 Windows GUI 的字体列表
鼠标进入、跨项、返回当前项、滚轮和 Escape 已复现，未观察到悬停引发的
列表跳动或自行滚动。

## 2026-09-25 追加：合并参数与菜单设置入口

提交 `6483258` 将侧栏独立的“菜单参数”移除，保留一个“参数设置”入口，
并在页面顶部加入“编码参数 / 菜单参数”两个可访问模式按钮。两种模式继续
绑定各自的参数状态：普通用户预设不会写入菜单配置，菜单安装、移除、保存和
运行中禁用行为保持不变。中英文翻译、Tab/Shift+Tab、Enter、Home/End 和
菜单操作回调均加入 `ui_smoke` 回归。

Windows x64-v3/IPO `Release` 重新构建后，51/51 相关 CTest 通过；`ui_smoke`
和 `ui_queue_stress` 均通过。新 `AWJ.exe` 版本为 `1.1.0`，SHA-256 为
`863b325a092bd23905f2b6f4478a9a393bc175f192f22f22572ac504200da6db`，
`AWJ.com` SHA-256 为
`7b50831a0f38396111fb41575727695e5c47f0262d8d1fde04991d227d360304`；
两者已覆盖到 `bin/1.1.0`，并与构建输出逐项匹配。

实际新程序窗口已人工检查：侧栏只显示“参数设置”，页内模式切换、菜单按钮
显示、键盘焦点和返回编码模式均正常。跨窗口 Explorer 拖放仍未能在当前
Computer Use 工具中完成，因为该工具拒绝把拖动释放到另一个窗口；结构化路径
和 Win32 `CF_HDROP` 程序回归已通过，不能把这项写成完整人工验收。
