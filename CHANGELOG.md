# 更新日志

## 1.0.11 - 2026-09-05

- prerelease：Windows Explorer 外部文件/文件夹拖放改为原生 OLE `IDropTarget` + `CF_HDROP`，并把文件选择、文件夹选择、Explorer 拖放与命令行入口统一到同一 C++ import pipeline；同时按职责拆分 Slint 组件/主题与 C++ 的导入、路径选择和平台桥接，保持队列去重、排序、碰撞与编码语义一致。
- Studio 的视觉质量提示改为“建议留空”：留空时继续由普通质量参数直接控制；若确实需要启用视觉质量搜索，建议从 50 左右开始，并明确 GMSD / MS-SSIM 的反复编码搜索会显著增加耗时；visual_quality 的 1–100 范围、默认空值、搜索算法和编码行为均未改变。
- 规范化 Windows classic shell 右键菜单为 HKCU-only、无需提权的静态级联注册，统一 vendor-qualified 命令与菜单顺序、perceived-type fallback、旧 schema/可选命令清理、事务回滚和 drift 验证；最终级联采用在当前 Windows Shell 实机验证可工作的 `ExtendedSubCommandsKey` REG_SZ shared-tree pointer 兼容布局，并保留其与当前 Microsoft Learn 文档布局存在差异的兼容性说明。
- embedded CHANGELOG 生成改为 UTF-8-safe 的分块字面量，避免 MSVC C2026 超长字符串，同时验证中英文嵌入内容可逐字节重建，并覆盖多字节 UTF-8 落在分块边界附近的情况。
- 修复 1.0.9→1.0.10 Windows 自动更新在 health-check 阶段错误依赖可变 `pending_update_version` 缓存而触发安全回滚的问题。新二进制的 readiness 现在绑定 helper 已验证的目标版本与当前 build version；签名 keyring/manifest、资产与成员哈希、transaction/session、30 秒 event、3 秒 stay-alive 和 rollback 安全门槛保持不变。1.0.11 仍为 prerelease，并保持现有 updater 兼容下限以允许已部署的 1.0.9/1.0.10 直接选择更新。
- 本版本发行验证继续遵守安全边界：未执行 F6、截图、截屏、capture 或 raw-WGC 功能测试；Directory Opus 不属于本轮验收范围。

## 1.0.10 - 2026-09-01

- prerelease：重做 Studio 悬浮提示与输出格式选择交互，避免 tooltip 生命周期反复触发重绘；队列输出顺序统一为 AVIF、AVIF.png、WebP、JXL、JPGLI、PNG，并通过显式 UI→后端格式映射保持既有格式索引兼容。参数设置加入 PNG，PNG 保持固定无损语义；队列操作按钮在单行/双行布局下保持等宽，并补充视觉质量推荐值 50 等界面文案调整。
- 回移 Slint/Winit AccessKit 组件生命周期保护：窗口组件已经销毁时，accessibility tree rebuild 不再强制解包已不存在的 component，而返回空 TreeUpdate，修复已定位的 Windows FAST_FAIL_FATAL_APP_EXIT 崩溃路径。受任务安全约束，本版本未执行 F6 或任何截屏功能测试。
- 修复 FP16/scRGB 16-bit 输入经 AOM 编码 AVIF 时 applied bit depth 的回归，确保该路径按预期使用 12-bit，并增加对应回归测试。

## 1.0.9 - 2026-08-30

- prerelease：修复 7z 自动更新对合法目录 entry 的误判。更新器现在只跳过由签名 manifest 文件路径必然隐含的安全目录，同时继续拒绝额外目录、路径穿越、链接、特殊成员、重复成员和未签名文件；补充真实目录 entry 的通过/拒绝回归测试，恢复旧归档（包括 1.0.8）的兼容提取。
- 固定 Windows 发行归档为严格扁平的 4 文件结构：`AWJ.exe`、`AWJ.com`、`LICENSE`、`NOTICE.txt`；包内不再携带 `.sha256`、`BUILD_INFO.txt`、`THIRD_PARTY_NOTICES.txt` 或 `THIRD_PARTY_LICENSES/`。归档及成员的大小/SHA-256 继续由签名 v2 manifest 绑定。
- `NOTICE.txt` 统一收纳构建信息、第三方 notices 和发行所需的第三方完整许可证文本；Linux 归档同步改为严格扁平的 `AWJ`、`LICENSE`、`NOTICE.txt` 三文件结构，并在打包脚本中加入精确成员校验。

## 1.0.8 - 2026-08-30

- prerelease：AVIF 新增显式颜色表示模式：默认 `yuv` 始终使用非 Identity matrix；`source` 按源图的 YUV/RGB Identity 表示跟随；`rgb` 强制 RGB(A)/GBR(A) Identity + 4:4:4，并在自动编码器模式下选择 AOM。CLI、预设、Studio 与编码诊断保持一致。
- AVIF 可选追加 `.png` 形成完整 `.avif.png` 文件名，实际内容仍为 AVIF；碰撞编号、同目录 `-converted` 命名和 Studio worker manifest 都将双后缀作为整体处理。
- 修正 AVIF 无损颜色语义和码流直通判定：符合条件的 YUV 420/422/444 AVIF 可保持原始码流；YUV 不再因 q100/4:4:4 隐式切换 Identity，BT.2020 源的无效/未知 matrix 回退为 BT.2020 NCL，其余回退为 BT.709。
- Studio 参数页新增 AVIF 颜色表示与 `.avif.png` 兼容选项；预设保存/加载新增字段，队列拖放和多目标扫描统一为确定性排序，并补强队列右键操作、失败筛选与 prerelease 更新日志标记。

## 1.0.7 - 2026-08-24

- prerelease：功能与 1.0.6 完全一致，仅用于 1.0.6→1.0.7 的归档自动更新测试；普通用户请继续下载 1.0.6。

## 1.0.6 - 2026-08-24

- prerelease：更新器新增双 root 授权的 release keyring、key ID、签名 `issued_at` / `expires_at`、撤销标志和最多 180 天有效期。v1、v2 与 keyring 的 sequence 和原始 SHA-256 通过跨进程锁及原子状态文件持久化；回退、同 sequence 篡改或状态损坏均 fail-closed，helper 会再次验证 keyring 与 v2 清单。
- 预设兼容加载历史的 `[null]` / `[整数]` `visual_quality` 写法，并在下次保存时写成正确标量；参数页和队列可再次预览 `测试.jsonc`。CLI 新增 `--list-presets`，动态列出同目录有效预设及无效文件原因。
- Windows CLI 新增 `--stdin-wgc-rgba16f <宽>x<高>`，可从管道读取一帧精确长度的 `DXGI_FORMAT_R16G16B16A16_FLOAT` 线性 scRGB；输入会以安全临时文件走原有 HDR 路径，短/长帧、无尺寸或未知裸 RAW 均拒绝。
- Windows 11 风格圆角应用图标；队列三个文件时间选项移到“写入运行日志”后，窄窗口自动换行。更新日志恢复正常向下滚动；设置页自动更新三项横向排列。
- 修复左侧“发现新版本”悬浮卡片：改为不透明、裁切受控的紧凑提示，完整发布说明仍在更新日志页，长摘要不再穿透卡片或遮挡导航。
- 发布工具链固定签名 keyring/v2 manifest 的 LF 原始字节，修复单条旧 manifest 续签时的 PowerShell 数组处理；`AWJ_Linux.7z` 改由原生 Linux 打包并验证可执行位，Windows 端只复核其内容和哈希。

## 1.0.5 - 2026-08-24

- prerelease：功能与 1.0.4 完全相同。本版本仅提供 1.0.3→1.0.5 的 Windows 自动更新桥接测试所需裸 `AWJ.exe` / `AWJ.com`；普通用户请下载 1.0.4 的归档资产。

## 1.0.4 - 2026-08-21

- prerelease：修复更新日志卡片正文被固定高度裁切、相互覆盖的问题；页面直接嵌入并显示从最早版本开始的完整历史，英语界面对尚未翻译的旧条目保留中文原文。左栏标题仍居中，版本号和上方线段组合改为向左偏移 16 px。
- 编码队列现在独立选择 AVIF、WebP、JXL、JPGLI 或 PNG 及用户预设，并在开始时快照整批任务；普通队列默认 AVIF、保留元数据且不保留 Windows 创建/修改/访问时间。参数页的格式只切换正在编辑的五组参数，默认内置参数不会写入根配置。
- 输入和输出路径接受一对完整双引号包住的路径；Slint 原生拖放同时接受文件和目录，输入不受文件/目录下拉框限制，输出拒绝多个目标。队列右键菜单不再改变详情选择。
- 参数页新增可执行文件同目录 `preset/*.jsonc` 用户预设：支持五种格式的完整参数、Unicode 安全文件名、严格 schema 校验、缺失字段默认回退，以及带名称和多行简介的全窗口浮层保存。
- `AWJ.jsonc` 改为白名单保存设置、右键菜单参数和更新安全状态；普通队列、路径、窗口和普通参数不再持久化。视觉质量未达标时兜底输出默认开启；AVIF 默认 speed 改为 5，空 speed 会恢复该默认值。
- Windows CLI 增加创建/修改/访问时间保留选项；成功提交输出后才尝试写回时间，失败只记录警告而不丢弃有效图片。Linux CLI 不显示也拒绝这些参数。
- HDR→SDR 改用 libplacebo 的 CPU-only spline 色调映射与 perceptual gamut mapping；PQ、HLG 和 FP16 scRGB 按明确 HDR 语义处理，目标为 100-nit BT.709/sRGB SDR，并保留非色彩元数据。
- 更新器采用签名的 v2 归档清单：Windows/Linux 下载 `AWJ_Win.7z` / `AWJ_Linux.7z`，逐成员验证大小与 SHA-256，严格拒绝路径穿越、链接、额外成员和解压炸弹；Windows 与可写 Linux 安装目录均进行原子替换、健康检查和回滚。
- 更新 Slint、vcpkg baseline 与发布锁定依赖；新增 libplacebo CPU-only overlay、libarchive 7z 提取和相应第三方通知。

## 1.0.3 - 2026-08-19

- prerelease：修正 Windows 更新请求中当前用户代理配置的生命周期，手动代理和 PAC/自动代理解析结果在整个请求期间保持有效；没有可用代理发现时继续安全回退到直连。
- prerelease：修正更新渠道切换后的焦点状态，焦点环保持与下拉框同宽并使用统一的蓝色样式，不再残留黑色边框。

## 1.0.2 - 2026-08-10

- prerelease：自动代理（WPAD/PAC）解析不可用时回退到 WinHTTP 默认路由，避免没有 WPAD 的直连环境被错误拒绝；手动代理仍优先生效。
- prerelease：Windows 更新请求现在读取当前用户的手动代理、PAC 和自动检测配置；无法读取用户配置时保留 WinHTTP 默认代理，不把代理地址写入日志。
- prerelease：更新渠道在自动检查期间保持下拉框焦点但禁止交互，检查结束后焦点不再跳到“显示更新日志”复选框，不会出现黑色焦点边框。
- prerelease：修正 Windows 更新 staging 位于 LocalAppData、安装目录位于其他卷时的跨卷替换；先复制到安装目录同卷临时文件再原子替换，保留失败回滚语义。
- prerelease：修正发布脚本续签已有 manifest 时的版本数组解析，确保后续 sequence/版本校验不会把字段说明误当作版本号。
- prerelease：将全部测试可执行文件隔离到 `bin/x64/tests/<配置>`，避免跨平台构建和发布清理互相覆盖测试产物。
- prerelease：更新日志页改为展示签名 manifest 中按版本排序的完整发布历史；移除手动检查入口，更新检查继续由程序自动触发。
- prerelease：更新日志页改为内嵌中英文大版本历史，并与已验签 manifest 条目合并；删除说明文字和空状态提示。
- prerelease：将最近一次已验签的 manifest 原文与签名随现有配置原子保存，启动时重新验签恢复历史，未到下一次自动检查间隔时也能离线查看完整日志。

## 1.0.1 - 2026-08-09

- 新增签名自动更新基础设施。客户端只接受内置 Ed25519 公钥验证通过的静态 `update-manifest.json`，签名覆盖确定性 UTF-8 原始字节；manifest 使用递增 sequence 防重放，版本严格按三段整数比较，并按 stable / prerelease 渠道过滤。
- Studio 左侧新增可点击版本号、持久红点和与队列/参数/设置同级的更新日志页；设置页可选择更新渠道、隐藏日志页与悬停摘要，并查看自动检查状态和上次成功时间。查看日志、切换页面、重启或检查失败都不会清除红点。
- Windows 使用 WinHTTP 在线程中执行有限超时、响应大小上限和主机白名单检查；资产下载前重新获取并验签 manifest，随后核对声明大小和 SHA-256。编码任务运行时拒绝更新，安装目录不可写时不申请提权并回到 Release 页面。
- Windows 自更新 helper 从父进程句柄推导安装目录，只能替换同目录 `AWJ.exe` / `AWJ.com`。它在 staging 中备份、写事务日志、替换两个文件并启动新版；健康检查未发信号、进程提前退出或任一步失败时终止新版并恢复整组旧文件，下次启动也能发现未完成事务并运行恢复 helper。
- `AWJ.jsonc` 更新字段和常规 Studio 设置统一由 UI 线程提交；Windows 写入改为同目录临时文件、`FlushFileBuffers` 和原子替换，避免定时保存与更新线程相互覆盖。整数配置扩为 64 位，消除 Unix 时间戳的 2038 年溢出。
- 修正 AVIF q100 / visual-quality 100、CICP 和 clamped grid 的帮助与文档，使其与 source-aware auto 色度、HDR 元数据优先级和较小边缘 cell 的实际实现一致；benchmark 的版本目录、标题和 profile 改为从 `VERSION` 派生。
- 修正 Windows/Linux 公开构建与打包说明，发行归档纳入 `LICENSE`、`THIRD_PARTY_NOTICES.txt` 和 `BUILD_INFO.txt` 并要求校验清单；补充可移植 Linux preset 和维护者 wrapper 预设的边界说明。
- 修正 `zenravif 0.1.3` 的 AGPL-3.0-only / 商业双许可证声明，并把更新器新增的 libsodium、nlohmann JSON 纳入第三方通知。发布脚本现在要求中英文 changelog、确定性 manifest、显式 sequence、签名 seed 与匹配的内置公钥。

## 1.0.0 - 2026-07-30

- Studio 新增中文/English 即时切换，翻译随 Slint 资源编入可执行文件，不依赖 gettext、`.mo` 或额外运行时文件；配置保存界面语言，切换后无需重启。
- 修正参数页布局：标签、输入框与下拉框按行垂直居中；彩色结论与详细说明在窄窗口自动换行，不再相互覆盖；两段彩色文字只保留 4 px 间距，设置页约束为视口宽度，避免为阅读说明文字横向滚动。
- 补齐 Clang/MSVC 模块依赖声明，并让 Windows 宏定义对所有 Windows 编译器生效；删除 LibRaw 内嵌数组的恒假空指针判断，严格 Windows Release 构建可通过。
- 审查崩溃处理器的异常对象生命周期，在 catch 作用域内立即记录 `what()`，不再把异常对象内部指针带出作用域。
- 修正 benchmark 对 0.10.5 以来 `--chroma auto` 的过期假设：现在按 `summary.csv` 的源色度校验 420/422/444 映射，并在中英文文档中明确 AWJ 使用 source-aware auto、ffmpeg 严格对照固定使用 420。
- vcpkg builtin baseline 更新到 2026-07-30 的 `c1d80d9c`。libavif `v1.4.2`、Slint `v1.17.1` 与 `zenravif 0.1.3` 已是最新稳定版，Jpegli 和 `svt-av1-hdr` 的固定提交分别等于当前上游 `main`；Rust 锁文件同步到 Rust 1.95 可用的最新传递依赖，并把 `Cargo.lock` 纳入 CMake 依赖，确保锁文件变化会真实重建 zenravif 静态库。没有为了追逐版本号改用未发布的 libavif/Slint 开发分支。
- 修正 `svt-av1-hdr` 当前上游 `noise_generation.c` 的重复 Cr 赋值：配置阶段把第二个目标恢复为 Cb，并在上游源码变化导致修正无法验证时直接停止配置，消除 GCC `-Wsequence-point` 与胶片颗粒色度缩放点未初始化问题。

- 新增崩溃留痕：`wmain` 最先安装 `std::set_terminate` 与 `SetUnhandledExceptionFilter`，异常逃逸时向 exe 同目录（不可写则退到 `%LOCALAPPDATA%`）的 `AWJ-crash.log` 追加一行阶段、异常码与 `what()`。处理函数只用 Win32 原语、不分配内存，日志路径在进程健康时预先解析；SEH 过滤器返回 `EXCEPTION_CONTINUE_SEARCH`，Windows 错误报告的 minidump 照旧生成。Studio 与右键转换窗口两条 GUI 入口进 catch-all，异常返回退出码 3 而不是静默 abort。此前从资源管理器启动时进程会无提示消失，只在系统里留下 `0xC0000409` / `FAST_FAIL_FATAL_APP_EXIT(7)`。
- 队列监控与编码监控两个 worker 线程体包进 catch-all。此前 `try` 只包住 `std::jthread` 的构造，仅能接住"创建线程失败"，线程体内部（管道拼接、事件解析等分配点）逃出的异常会直接 `std::terminate`。错误处理不跨线程写 Slint 属性：带锁清理状态并终止仍在运行的子进程，界面提示走 `post_to_ui`。
- Studio 关窗回调补上异常护栏。该回调在事件循环内执行且会经 `capture_studio_config` 分配三十余个字符串，异常会穿回 Rust 侧 `panic="abort"` 的栈帧；现在无论配置保存成功与否都放行关窗。
- 修复两处句柄判断错误：`CreateFileW`（右键队列写入）与 `CreateMailslotW`（leader 建通道）失败返回 `INVALID_HANDLE_VALUE`，而 `unique_ptr::operator bool` 只与 `nullptr` 比较，失败被当成成功——前者对无效句柄发起 I/O，后者让 leader 持废句柄静默收不到任何输入。新增 `adopt_win32_handle` 统一归一化。
- 自动内存上限改为 `min(总内存 80%, 当前可用 50%)`，修正此前两个系数写反的问题；同步更新 CLI `--memory-limit` 帮助与 README。回归测试从 1 条断言扩到 4 条，覆盖可用内存偏紧、机器空闲与两个单项回退分支。
- 移除设置页的"右键编码预设"下拉及其全部支撑代码（属性、`changed` 处理、callback、两对 capture/apply、两个预设应用函数、两处回调注册与配置读写）。配置按 key 查找、忽略多余键，旧配置里残留的 `menu_*_preset_index` 不会报错。
- `src/ui/main.cpp` 剩余的 C 风格 `std::fprintf` 输出改为 `std::println`，与 CLI、config、pipeline 既有风格一致。

## 0.10.5 - 2026-07-25

- Studio 主参数页恢复右侧彩色说明，移除主页面的“预设”和“大图优先”选择；右键菜单预设和 CLI `--large-image-priority` 保持可用。Studio 的大图自动链固定为 `zenrav1e` 优先、失败后回退 `grid`。
- AVIF `--chroma auto` 现在保留 YUV 源的 420/422/444，RGB/RGBA 转为 YUV 444，灰度或未知源使用 420。解析后的采样会在编码器选择前传入，避免 422/444 源图误走 SVT 的 420-only 路径；显式 SVT 仍只接受 420。
- 参数说明按真实资源规则更新：自动线程在 >=12、5-11、2-4 逻辑线程时分别预留 4、2、1 个线程；自动内存上限为总内存 50% 与当前可用内存 80% 的较小值，缺少任一数据时使用另一项。
- 修正 AVIF AOM 直接编码路径的 auto chroma 回退与源格式映射，并补充 YUV422、YUV444、RGB、RGBA 和无源元数据的回归覆盖。
- 依赖基线更新到当前 vcpkg registry；libyuv 升至 1951、libavif 升至 v1.4.2，并刷新静态 SVT AV1 HDR 源。AOM 3.14.1、dav1d 1.5.4、Jpegli 和 Slint 1.17.1 已确认处于当前上游版本。

## 0.10.4 - 2026-07-21

- Windows Release 改为静态链接 Slint；vcpkg baseline 更新，AOM 升级至 3.14.1、dav1d 升级至 1.5.4，并以项目内 overlay 固定 AOM、dav1d 和 libyuv 的构建输入。
- AVIF 自动 bit-depth 至少使用 10-bit，保留 10/12-bit 源图精度；`auto` chroma 固定为 YUV 420，只有显式 422/444 才改变采样。非不透明 alpha 自动保留并随请求质量编码，不再强制整图 q100/444；源图 CICP PC/full 与 TV/limited range 默认保持。
- `summary.csv` 增加 AVIF RGB->YUV、AddImage、Finish 和输出复制耗时；基准脚本校验当前默认 AVIF 参数，并将旧 q80/8-bit 历史数据与当前协议分开。
- 参数页移除右侧问号提示，导航已选项不再显示额外边框；键盘导航和可访问角色保持不变。
- `bin/` 改为仅本地构建/打包输出，不再进入 Git 历史；正式二进制继续通过 GitHub Release 归档提供。

## 0.10.3 - 2026-07-15

- 不调整编码器功能或参数语义：quality、speed、色度、位深、透明 AVIF AOM 整图无损、dav1d 优先解码与自动回退保持 0.10.2 行为；自动线程预算和“超过 12 张时每图单编码线程”规则保持不变。
- 新增可重复 Windows Release benchmark：固定输入指纹、AOM/q80/speed6/420/8-bit、电源方案和构建/commit，每组预热 1 次、测量 5 次，报告墙钟/CPU/P95/峰值内存/吞吐量/版本/依赖版本及 decode/prepare/encode/write 阶段耗时；读取峰值工作集前刷新 .NET `Process` 缓存，避免长批次复用首次采样值。
- 0.10.3 按 CLI-only 约定完成 1/4/12/13/613 张、透明与不透明矩阵；613 张中位墙钟 162.660 s、P95 166.395 s、进程 CPU 中位 1381.844 s、峰值工作集中位 2807.7 MiB、吞吐 3.769 img/s、encode 占 97.0%。编码路径未改，因此不把更快观测归因于 UI 改动，不触发 ETW/perf，也未实施无证据的 AOM 微优化。
- 完善 SoftComboBox、SoftButton、左侧导航、帮助提示和队列右键菜单的 Tab 焦点、清晰焦点环、Enter/Space、方向键、Home/End、Esc，以及可访问角色、名称、状态与默认动作。
- 字体下拉框继续最多显示 10 行，保留滚轮和滚动条，不恢复搜索或手动输入；参数页改为常用参数、资源限制、格式高级选项分组，危险警告常驻，其余长说明移入帮助提示。
- 统一导航青色与操作蓝色的层级，压平队列重复边框并扩大列表空间；无窗口 component smoke 覆盖 820×560、100%/150%/200% scale、长文件名/字体名、深浅色、页面切换与键盘操作。
- 队列新增待处理、处理中、成功、失败计数、仅看失败、重试失败项和选中项详情；详情显示完整错误、输入/输出路径、编码器、线程数及四阶段耗时，避免表格省略关键信息。
- Studio CLI worker 协议新增版本化 `DETAIL` 记录；新增纯 CLI Windows smoke，验证成功/失败 manifest、普通命名事件取消返回 130、Job Object 强制终止和进程清理，不启动 Studio 或 UI Automation。
- 修复测试对 MSVC 间接模块可见性、Windows 路径字符类型、WIC/JXR 和 AWJ RAW 平台能力的隐式假设。Windows MSVC Release 31/31、Linux GCC 16.1 Release 16/16 通过；Linux ELF 同时验证 `-O3`、LTO、`x86-64-v3` 和静态 `libstdc++`/`libgcc`。
- 发布 GitHub Release `0.10.3`：`AWJ_Linux.7z` 只含 Linux `AWJ`，`AWJ_Win.7z` 只含 `AWJ.exe` 与 `AWJ.com`。两包均用单线程、最高级别 LZMA2（`-m0=lzma2 -mx=9 -mmt=1 -mf=off`）打包并通过 `7z t`；归档 SHA-256 见 README 与 `RELEASE_NOTES_0.10.3.md`。

## 0.10.2 - 2026-07-13

- 修复 Windows 进程 ID 获取的无限递归，解决 Studio 和右键转码在编码后表现为卡住、速度异常慢的问题；未更改编码 speed 参数、默认值或映射。
- 修正队列的线程预算与并发规划，保留现有自动线程限制：硬件线程数 >=12 时预留 4 线程、5..11 时预留 2 线程、2..4 时预留 1 线程、单线程仍使用 1 线程；编码器线程数与 CPU 并发上限的乘积严格等于总预算，图片数超过 12 时每张图片固定单编码线程并发执行，内存并发限制独立生效。
- Studio 主队列恢复为独立 worker 进程；“强制终止”现在会立即终止活动进程树，而普通取消仍使用合作式取消。
- 需要保留透明通道的 AVIF 固定走 AOM 整图无损：颜色与 alpha 都使用 q100/4:4:4，并保持用户 speed 不变；`alpha=off` 仍按请求质量有损编码，`zenrav1e` 因无法保证逐字节无损而不再接收透明输入。AVIF 解码优先使用 dav1d，dav1d 不可用或解码失败时自动回退 AOM。
- `summary.csv` 新增 `awj_version` 列；日志在旧文件合法时追加，发现无效 UTF-8、格式错误或截断内容时清空重建，并通过跨进程锁避免多个右键转码进程互相覆盖。
- 队列 manifest 改为版本化二进制格式，并补充路径、文件大小、记录数量和输出覆盖等输入边界校验。
- 单个损坏或无法探测尺寸的输入现在只记录为该项失败，不再中止其余 AVIF 批处理任务。
- 右键多选在 Slint 初始化前完成合并，由固定等待 650 ms 改为初始收集窗口加消息静默窗口，并在关闭通道前锁定发送端、最终排空队列，避免末尾请求漏转；Windows 编码 worker 恢复普通调度优先级，避免在系统有其他负载时被长期饿住。
- 主界面与右键窗口统一为固定顶部表头和连续列表，进行中的项目立即显示“正在转码”；完成、失败和运行行不再因不可拖动而整体淡化，右键窗口在高 DPI 下会连同标题栏和边框约束到当前工作区。
- 1000 万像素以上、仍处于 AOM 普通单图范围内的图片继续放在尾部阶段，避免单张大图的内存估算压低整个小图队列并发；总批次超过 12 张时，普通、延后和大图阶段仍全部使用每图单编码线程，不修改 quality、speed 或内存上限。
- 更新 vcpkg baseline、TIFF 4.7.2、Slint 1.17.1 与 Rust 传递依赖；固定 libavif 1.4.2、AOM 3.13.3、dav1d 1.5.3 及当前编码器构建 pin。

## 0.10.1 - 2026-07-11

- 修复 AVIF grid 的非整数倍尺寸：右列和底行使用实际剩余尺寸，输出保持原始宽高；奇数尺寸自动回退 4:4:4，显式不兼容的 4:2:0/4:2:2 会明确报错。
- 动图和多图容器统一只转换合成后的第一帧，包括 WebP、GIF、APNG、JXL、TIFF、AVIF sequence、WIC 多帧和 JPEG MPF；不能可靠提取时直接报错，不保留其余帧。
- 右键菜单移除视觉质量，仅保留固定编码质量预设；精简右键转换窗口，文件名居中、目录以较小字号显示在下方，并合并冗余状态列。
- Windows Explorer 对多选图片/文件夹分别发起的右键进程会在启动期合并到一个窗口和同一转换队列，不再为每个选中项保留独立 AWJ 进程。
- 右键转换窗口新增“强制终止”；本体与右键窗口点击右上角关闭时会先立即终止全部活动任务，再关闭界面。
- Studio 新增已安装字体下拉选择，首项可恢复系统默认；Windows 将 `ui_font_family` 写入 `AWJ.jsonc`。
- Linux GCC Release 启用静态 `zenrav1e`，Nautilus/Thunar 对齐五种格式及其质量、位深、速度、尺寸限制和格式专属菜单参数；WIC/JXR 等 Windows 专属功能仍不提供。

## 0.10.0 - 2026-07-10

0.10.0 是一次把 Linux/Vulkan 首版并入主线、统一 Windows/Linux 共用 core，并落地超大图自动处理链路的版本。

- 合并 Linux GCC/Vulkan 构建到主线：`linux-gcc-x64-debug` / `linux-gcc-x64-release` 预设可用；Linux 只生成单个 ELF `AWJ`，不生成 `AWJ.com`。
- Linux Release 默认使用 GCC 16 side-by-side、`-O3`、IPO/LTO、`-march=x86-64-v3`，并静态链接 `libstdc++` / `libgcc`，降低跨发行版运行时依赖。
- Linux visual_quality GPU 指标路径改为 Vulkan compute；失败、小图或资源不足时自动回退 CPU，日志记录 `vulkan-session` / `cpu-fallback`。
- Linux 不启用、不展示 WIC 兜底；JXR/WIC/`AWJ.com`/Windows 注册表 shell 仍仅限 Windows。Linux 提供用户级 Nautilus Scripts 与 Thunar UCA 右键入口。
- 超过 AOM 单图上限后自动走大图链路：默认 `zenrav1e` 优先并回退 `grid`，参数页可改为 `grid` 优先；两条路径都失败或触达内存/输入上限时明确报错。
- Studio 大图页保留状态展示，并可对单项强制指定 `zenrav1e` / `grid`；手动强制路径不会静默改路。
- 新增会话内“解除 20 GiB 输入/运行时上限”（UI 设置页 / `--unlock-max-input-file-bytes`），默认关闭、红字警告 OOM 风险，且不写入 `AWJ.jsonc`。
- 参数设置页编码参数保持“本次运行内有效、不写入 jsonc”的策略；主题/模板/菜单参数等既有持久化项不变。
- 补强 grid 失败诊断：不可整除且需要 padding 时，明确提示当前版本未启用安全裁切，并建议改用可整除尺寸、experimental clamped padding 或 `zenrav1e`（边长 <= 65536）。
- Windows 与 Linux 共用 core/pipeline/codecs；平台差异仅保留 Win32 shell、WIC/JXR、D3D11、`AWJ.com` 与 Linux Vulkan/POSIX 入口。
- 同步中英文 README / 迁移文档，并提交 Linux Release `AWJ` 产物。

## 0.9.1 - 2026-07-06

0.9.1 是一次以 Windows 右键菜单转换体验、菜单参数持久化、图片边长限制和 HDR 色彩修正为核心的小版本更新。

- 右键菜单转换改为由 `AWJ.exe` 启动轻量 Slint 队列窗口，多选图片会进入同一个队列，不再为每个文件打开独立窗口或命令行窗口。
- 右键转换窗口适配深色模式和高 DPI，队列条目使用圆角卡片，进度条从左到右显示，长文件名自动省略，完成后可按菜单参数自动关闭。
- 右键菜单参数迁移到左侧“菜单参数”页，支持安装右键菜单、移除右键菜单、保存参数，并在注册表菜单与当前版本/参数不一致时提示移除旧菜单。
- 右键菜单预设独立于 Studio 参数页持久化保存，AVIF、WebP、JXL、JPGLI、PNG 均可分别配置菜单参数；PNG 仍只影响右键菜单/CLI 输出，不重新加入 Studio 输出格式下拉。
- 新增图片边长限制：支持自动、无限制和手动模式；手动模式可同时设置最大宽、高、长边和短边，并按最严格限制缩放。
- 新增 `suffix-number` 输出重名策略，按 `name(1)`、`name(2)` 递增，避免重复编码时出现 `name(1)(1)` 一类文件名。
- 修复右键单文件 WebP 输入识别/解码路径，优先使用 native WebP 解码，允许 WIC 作为兜底解码器。
- 改进 JXR/WIC scRGB HDR 解码到 BT.2020 + PQ 16-bit 的色彩路径，但 WebP、PNG、JPGLI 的 HDR 输出仍为异常/不可靠场景。
- 重要提醒：如果源图是 HDR，请不要选择 WebP、PNG 或 JPGLI 作为目标格式；HDR 源图建议优先使用 AVIF 或 JXL。
- 修复窗口底部下拉框向下展开空间不足时显示不全的问题，仅在空间不足时改为向上展开。
- AVIF 大图模式阈值改为真实单图编码上限：AOM/libaom 允许 65536 边和 `2^30` 像素内继续普通编码，`svt-av1-hdr` 标记为 16384×8704 上限；1000 万像素以上但未超限的图片只延后到普通队列尾部。
- Studio 主页队列改用新版 Slint `DragArea`/`DropArea`，未开始项目可直接拖动排序；同时修复“下移”排序插回原位的问题。
- 保留普通 AVIF 的单任务多线程编码路径，大图阈值调整不会把 1000 万像素级图片强制切到 grid/大图 worker。

## 0.9.0 - 2026-07-04

0.9.0 是一次以编码器 preset/speed 语义修正、Studio 输出格式行为调整和 JXL JPEG 无损转封装为核心的版本更新。

- 修正默认质量策略：AVIF 默认 q70，JXL 默认 q85，WebP 默认 speed4，AVIF/JXL 默认 speed6。
- 调整 JXL speed 到 effort 的映射为 `speed0 => effort10`、`speed6 => effort4`、`speed10 => effort1`，覆盖 libjxl 当前 effort 1..10 范围。
- 明确 AOM/AVIF 默认路径：不设置不存在的 `--good`，保留 libavif/AOM 自身 still image / all-intra / speed 逻辑；AVIF auto 文案同步为默认 AOM。
- 显式选择 SVT-AV1-HDR 编码 AVIF 时，对 lossless/q100、需要保留 alpha、显式 444/422 和 >10-bit 等不支持场景直接报错，避免隐式降级。
- JXL 对 JPEG 输入默认优先使用 JPEG bitstream 无损转封装；遇到 strip metadata 或色彩/HDR 元数据冲突时回退普通 JXL 有损编码。
- Studio 输出格式下拉移除 PNG，PNG 保留 CLI 与右键菜单支持；Studio 的 speed 输入仅对 AVIF/WebP/JXL 显示和传入。
- JPEGli 关闭伪 speed 支持：Studio 不展示 speed，CLI 显式传入 `--speed` 时直接报错。
- 修复 Studio 切换输出格式时质量值跨格式污染的问题：各格式在本次运行内独立记忆质量，重启后恢复内置默认值。

## 0.8.5 - 2026-06-18

0.8.5 是一次以 AVIF/JXR 位深兼容、默认质量策略调整和深层编码性能优化为核心的版本更新。

- 修复 JXR 等 16-bit 源图输出 AVIF 时直接失败的问题；继承源图位深超过当前 AVIF 编码器上限时，现在限制到编码器支持的最高输出位深，用户显式请求不支持位深仍会明确报错。
- 调整默认质量：AVIF 默认 q70，JXL 默认 q85，WebP 与 JPGLI 默认值保持不变。
- 优化 AVIF 10/12-bit 编码准备路径，取消 AWJ 侧 8-bit 到高位深 RGB 临时扩展，交由 libavif 官方 RGB/YUV depth rescale 处理。
- 并行化 AVIF Grid tile 准备，并通过可重复 CMake patch 为保守 still color grid 场景启用 libavif tile 编码并行。
- 改进超大图队列调度，在内存预算、线程预算和输出路径冲突约束下允许不同输出路径并行处理。
- 细化编码线程资源规划，按 AOM/zenrav1e、SVT-AV1-HDR、JXL 和其他编码器分别设置默认线程上限。
- 更新 FetchContent 依赖到当前可用上游版本，并在 Release BUILD_INFO 中记录对应 pin。

## 0.8.2 - 2026-06-09

0.8.2 是一次以 Studio 队列行为修复、窗口状态记忆和默认常量统一管理为核心的小版本更新。

- 修复 Studio 队列中已编码图片不能删除的问题；清空队列现在会清理全部普通队列和大图队列条目。
- 增强未清理队列时的再次编码行为：已完成、已跳过或正在锁定输出的条目不会被重新编码，只有待编码、失败和已取消的条目会进入下一次运行。
- Studio `AWJ.jsonc` 新增 `window_width` / `window_height`，关闭窗口时会刷新当前窗口大小，下次启动按上次尺寸恢复。
- 将编码默认值、metadata 上限、JXL/WebP 缓冲区、输出临时文件重试和 visual_quality 评分常量集中迁移到 `src/core/encoding_defaults.ixx`；Studio UI 运行时常量独立放入 `src/ui/studio_defaults.ixx`。
- 参数页仅居中“大图处理”下拉框文本，其他下拉框保持原有对齐。
- Studio 字体仍优先使用系统已安装的鸿蒙黑体 / HarmonyOS Sans SC；建议用户安装鸿蒙黑体以获得最佳中文界面显示。
- 更新版本到 0.8.2，并保持 Release 输出为 `AWJ.exe`、`AWJ.com` 及对应 `.sha256` 校验文件。

## 0.8.1 - 2026-06-09

0.8.1 是一次以多路径性能优化、Studio 运行时配置持久化和系统字体回退为核心的小版本更新。

- 优化 native 后端文件复制缓冲、WebP/JXL 编码输出缓冲预估，以及 visual_quality 搜索候选解码路径，减少重复 metadata 拷贝和候选指标开销。
- Studio 新增同目录 `AWJ.jsonc` 运行时配置；启动时先使用程序内默认值，再由 `AWJ.jsonc` 覆盖，用户修改过的设置会自动写入对应配置项。
- Studio 不再嵌入字体文件，默认优先使用系统已安装的鸿蒙黑体 / HarmonyOS Sans SC，未安装时回退微软雅黑等系统字体；建议用户安装鸿蒙黑体以获得最佳界面显示。
- 更新版本到 0.8.1，并保持 Release 输出为 `AWJ.exe`、`AWJ.com` 及对应 `.sha256` 校验文件。

## 0.8.0 - 2026-06-09

0.8.0 是一次以 JPGLI 高级编码选项、BMP/JXR native codec、Visual Quality GPU 指标优化和四文件发布清理为核心的版本更新。

- JPGLI 新增 progressive level、Huffman 优化和 XYB 相关配置；CLI、Studio、config 校验、summary 和回归测试同步支持。
- 新增 BMP 与 JXR native codec 注册、WIC 解码路径和对应 codec 测试，扩展输入格式覆盖范围。
- 优化 Visual Quality GPU 指标链路，改进 downsample、luma、GMSD 与 MS-SSIM shader 的计算稳定性和诊断输出。
- 改进 Studio 交互和任务状态展示，补充格式、质量、视觉质量、取消和内存限制相关提示。
- Release 构建清理规则收敛为 `AWJ.exe`、`AWJ.com` 及两个 `.sha256` 文件；测试可执行文件改输出到独立 tests 目录，避免污染发布目录。
- 清理旧设计文档残留，并补充本轮严格审查记录。

## 0.7.3 - 2026-06-07

0.7.3 新增 JPGLI/Jpegli native codec 路径，并同步 Studio/CLI 入口、诊断和文档。

- 新增 `AWJ_ENABLE_JPEGLI` CMake 选项，默认拉取并静态链接 google/jpegli 的 `jpegli-static`。
- CLI 新增 `--format jpgli` / `--format jpegli`，Studio 格式下拉新增 `JPGLI`；输出扩展名默认保持 `.jpg`，但 UI、命令、summary 和日志均显示 `JPGLI` / `jpegli`。
- 新增 `JpegliImageEncoder` 与 `JpegliImageDecoder`，支持 encode/decode、`decode_memory`、质量与速度映射，以及 ICC/EXIF/XMP metadata 保留/剥离。
- JPEG 兼容输入优先尝试 Jpegli decoder，失败后回退 libjpeg-turbo/WIC。
- `summary.csv` 新增显式 `encoder_id` 列，并保留旧 `encoder_selected` 列；JPGLI 输出记录 `format=JPGLI`、`encoder_id=jpegli`。
- 改进 Studio JPGLI 相关默认质量、固定 8-bit 位深、拖拽队列提示和任务列表格式显示。
- 补充 JPGLI codec、config、decoder registry、native pipeline、资源规划和 summary 安全回归测试。

## 0.7.2 - 2026-06-05

0.7.2 修复部分终端中 `AWJ.com --help`、`AWJ --help` 无输出的问题。

- `AWJ.com` 不再只把当前标准输出句柄继承给 GUI subsystem 的 `AWJ.exe`，改为通过 stdout/stderr pipe 捕获子进程输出后写回当前终端。
- 保留 `AWJ.com` 的等待和退出码透传语义，`--help` 返回 0，未知参数返回非 0。
- `AWJ.exe` 仍保持 Windows GUI subsystem，双击启动 Studio 不弹出命令行窗口。

## 0.7.1 - 2026-06-05

0.7.1 修复双击启动 `AWJ.exe` 时短暂弹出命令行窗口的问题，并保留命令行入口的等待、输出和退出码语义。

- 将 `AWJ.exe` 改为 Windows GUI subsystem，双击无参数启动 Studio 时不再创建命令行窗口。
- 新增 `AWJ.com` console shim；命令行输入 `AWJ ...` 时由 shim 转发到同目录 `AWJ.exe`，等待结束并透传 stdout/stderr 与退出码。
- Studio 内部 worker 仍自举 `AWJ.exe`，不依赖 `AWJ.com`，取消、强制终止和 Job Object 隔离语义保持不变。
- 更新构建脚本、CLI smoke 脚本、README、迁移文档和帮助文本，说明 GUI 主程序与 console shim 的分工。

## 0.7.0 - 2026-06-04

0.7.0 是一次以单一 `AWJ.exe` 发布物、Studio 自举 worker 和 CLI/Studio 合体体验为核心的版本更新。重点是取消 Studio 对独立命令行可执行文件的依赖，让 UI、命令行与内部编码 worker 都收敛到同一个可执行文件。

- 合并 CLI 与 Studio 发布目标，Release/Debug 日常构建只生成 `AWJ.exe`，不再生成旧 CLI/Studio 双可执行发布物。
- 新增统一入口：无参数启动 Studio UI，带 CLI 参数时进入 headless 转换路径，`AWJ.exe --help` 显示命令行帮助并保持可靠 stdout/stderr 与退出码。
- Studio 转换改为自举同目录 `AWJ.exe` worker 子进程，继续使用 Job Object 管理生命周期；“取消”通过事件请求协作式停止，“强制终止”和运行中关闭窗口只终止编码 worker，不结束 Studio 本体。
- 保留现有 native codec pipeline、visual_quality GPU 指标路径、summary/log 诊断、AVIF 大图模式和实验编码器能力；UI 与 CLI 共用同一套参数解析与批处理服务。
- 更新脚本、帮助文本、README、迁移文档、manifest/resource 和发布清理规则，面向用户的构建目标、命令示例、输出文件与校验文件统一为 `AWJ`。
- 日志文件名从 `log\awj-cli.log` 调整为 `log\awj.log`，与统一发布物命名保持一致。

## 0.6.2 - 2026-06-04

0.6.2 是一次以 visual_quality GPU 指标链路、Studio 任务隔离和交互修复为核心的小版本更新。

- 将 visual_quality Direct3D 11 shader 改为构建期预编译并内嵌 bytecode，Release 运行时不再依赖 `d3dcompiler` 或 `.cso` sidecar。
- 扩展 Visual Quality Search 的 GPU 诊断，summary 与日志可区分 GPU requested/used/path/fallback reason/fallback count。
- 优化 visual_quality 搜索为预测窗口内 lower-bound 二分，lossy q 限定在 1..99，减少 1 次早停和 8/9 次探测之间的跳变。
- Studio 转换改为启动同目录统一 AWJ worker 子进程，使用 Job Object 管理生命周期；“取消”通过事件请求协作式停止，“强制终止”和运行中关闭窗口只终止编码 worker，不结束 Studio 本体。
- 修复 Studio 窗口拖动：除 Slint 标题区域回调外，增加 Win32 `WM_NCHITTEST` fallback，将左侧标题区域识别为 `HTCAPTION`。
- 修复 Studio 先选择输出路径、再选择输入路径时自动覆盖已有输出路径的问题；已有输出路径会保持用户手动指定的内容。
- 清理旧 AVIF/AOM helper 源码和 Studio 同进程 worker 残留代码，转换路径继续收敛为 native codec 与统一 AWJ 发布目标。

## 0.6.1 - 2026-06-03

0.6.1 是一次以 Visual Quality 默认体验、Studio 任务控制和批处理资源规划为核心的小版本更新。

- 重构默认预设策略，Studio 默认从固定编码质量切换为 visual-quality 观感目标，平衡、快速、急速等预设更清楚地区分质量、体积与耗时取舍。
- 完善 Visual Quality Search，补齐 GPU/CPU 切换、未达标候选兜底、候选复用、JXL RGB 输入缓存和更细的编码/解码/指标耗时诊断。
- 强化取消与强制终止路径，CLI、Studio worker、批处理线程和候选搜索增加更细粒度 stop 检查，Studio 运行中支持强制终止并安全回收后台 worker。
- 优化批处理资源规划，根据图像尺寸、输出格式、visual-quality 状态和内存预算区分普通任务、延后任务、内存超限任务与大图模式任务。
- 改进 AVIF 大图提示，按当前构建能力识别 AOM grid 与实验 zenrav1e，队列、日志和 summary 中输出更明确的尺寸、原因与可用处理方式。
- 扩展 summary.csv 与日志诊断字段，记录 visual-quality 分数、GMSD/MS-SSIM、候选次数、GPU/内存回退、速度参数、编码线程、内存预算和阶段耗时。
- 调整 Studio 参数页与设置页说明，补充质量、视觉质量、内存、速度、WIC 兜底、GPU 加速和实验编码器等选项文案，并改用 HarmonyOS Sans SC 字体资源。
- 更新构建版本到 0.6.1，同步 Debug/Release 脚本构建参数，继续保持 native codec 主线和统一 AWJ 发布目标。

## 0.4.0-rc1 - 2026-05-25 (Pre-release)

- 全面转向 Native 编码核心，彻底清理旧版依赖残留，重组为轻量高效的原生 codec 转换架构。
- 重构色彩空间与元数据透传管道，支持 Primaries、Transfer、Matrix、Range、ICC 及 HDR 元数据全链路透传检测。
- 引入 AlphaModePolicy 策略选项，提供针对 Alpha 通道的安全处理机制（保留/丢弃/智能检测）。
- 完善 Studio 级大图转换队列、AVIF grid/zenrav1e 调度入口，防止 AVIF 无损路径静默降级。
- 优化 Studio UI 表现，根据实际构建能力动态置灰未实现后端，限制事件积压，降低常驻内存。
- 强化 helper 进程、写入安全、Raw 校验以及质量搜索路径的安全边界，极大优化取消处理表现，并补齐回归测试。

## 0.3.1 - 2026-05-21

- 重写 Studio 为 WinUI 3 风格左侧导航布局，拆分编码队列、参数设置、大图模式和设置页。
- 编码队列改为默认入口，输入输出和开始/取消/清空等操作集中到队列页。
- 调整导航、下拉框和页面卡片样式，移除多余说明文字与默认下拉黑色焦点边框。
- 大图模式保持轻量占位，不加载图片、不缓存预览资源，页面切换继续使用条件实例化降低常驻内存。
- 记录项目构建约束：日常构建只构建 AWJ，不构建测试可执行文件。

## 0.3 - 2026-05-21

- 移除内置 ImageMagick/MagickWand 后端、构建脚本与运行时复制逻辑，转换路径收敛为 native codec。
- 保留内置 libavif/AOM、实验 zenrav1e、WebP、JXL；Magick 与 ffmpeg 仅作为未来外部集成方向。
- 将 zenrav1e 隔离到内部 AVIF helper，CLI/Studio 主进程不再直接链接 Rust bridge。
- 新增 `--experimental-encoders` 与 UI 实验编码开关，默认 auto 不选择实验编码器。
- 清理 Release 输出，只保留用户可启动的 CLI/Studio，并将 AVIF helper 放入内部目录。

## 0.2.1 - 2026-05-17

- 新增 JXL 输出格式，CLI 支持 `--format jxl`，UI 格式下拉支持 JXL。
- JXL 默认质量为 q95，输出扩展名为 `.jxl`，任务列表和日志显示为 `JXL`。
- JXL 使用当时的 MagickWand 编码、批处理和质量搜索流程。
- JXL `--speed 0..10` 映射为 ImageMagick `jxl:effort`；用户可通过 `--define jxl:effort=...` 或其他 `jxl:*` define 覆盖。
- JXL 下禁用手动 chroma sampling；位深留空保持原图语义，显式设置时透过 `MagickSetImageDepth`。
- 启动编码前检测 ImageMagick runtime 是否支持 JXL，不支持时提示重新构建带 JPEG XL delegate 的 runtime。
- 更新 ImageMagick 构建脚本、CLI smoke 脚本和文档，说明最小 runtime 构建包含 JXL coder。
- 修复静态 Release CLI 在部分环境首次输出前访问冲突的问题。

## 0.2 - 2026-05-16

- 增强 Release 构建复现性，默认使用自编译 ImageMagick runtime 与静态依赖组合。
- 改进 Studio UI 渲染稳定性、字体与 DPI 表现。
- 改进转换诊断、日志、summary 和错误提示。
- 增强输入路径选择、拖放处理和批处理稳定性。

## 0.1 - 2026-05-16

- 初始 C++23 / Slint / MagickWand 迁移版本。
- 提供 AVIF/WebP CLI 批处理和 Studio 桌面 UI。
