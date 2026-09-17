# 1.0.15 修复验证

状态：2026-09-18 由维护者授权发布正式版。本轮按维护者要求只运行受影响范围的测试，
完整测试套件、更新器端到端与实机 UI 验收见文末“未执行与残余风险”。

## 闪退根因（已定位到具体指令）

现象：Studio 首页导入图片后点击「开始转换」，1～2 秒内界面进程直接消失。

证据链：

1. `bin/x64/Release/AWJ-crash.log` 留下 `unhandled-seh stage=studio-ui code=0xc0000005`。
2. 崩溃转储 `%LOCALAPPDATA%\CrashDumps\AWJ.exe.40404.dmp` / `AWJ.exe.41668.dmp`（对应 2026-09-18 02:30、02:06 两次）：
   - 异常码 `0xC0000005`，异常参数为 **读地址 `0x0`**；
   - `rip = AWJ.exe+0x1996641`，寄存器 `rcx = 0`。
3. `build/1.0.14-dist/AWJ.map`（Timestamp `6aab63ca`，与已发布的 1.0.14 `AWJ.exe`
   SHA-256 `2f09c084…84d5` 一致）反查符号：
   - `0x141996641` = `strlen + 0x31`（libucrt）；
   - 调用点 `0x14090B0EE` = **`awj::studio::begin_queue_conversion_run` + 0x9BE**
     （`studio_encode_dispatch.obj`），反汇编为
     `xor ecx,ecx; call strlen`，紧邻的指令序列是
     `std::string::_Assign(ptr, 12)`（写入 `"等待编码"`）→ `slint_shared_string_drop`
     → `slint_shared_string_from_bytes(nullptr, 0)`。
4. 对应源码：`src/ui/studio_encode_dispatch.cpp:227` 的 `item.log_text = {};`，
   该行位于队列开跑前的重置循环内，`QueueImageItem::log_text` 的类型是
   `slint::SharedString`。

机理：`slint::SharedString` 同时提供 `operator=(const SharedString&)` 与
`operator=(const char*)`（后者把参数当 null 结尾的 C 字符串）。空花括号对
`const char*` 是恒等转换，优于拷贝赋值所需的用户定义转换，因此 `= {}` 选中
`operator=(const char*)` 并传入 `nullptr`；其实现经 `std::string_view(const char*)`
调用 `strlen(nullptr)`，界面进程随即在读地址 0 处崩溃。

该写法自 1.0.13 起存在（1.0.13 的 `src/ui/main.cpp` 已有同一行），与输入格式、
图片大小无关：只要点「开始转换」或「重试失败」就会命中。

## 修复

- 新增 `awj::studio::clear_shared_string()`（`src/ui/slint_string_util.h`，只依赖
  `slint.h`），实现为 `value = slint::SharedString{};`，并写入“禁止 `= {}` /
  `= nullptr`”的注释与根因说明；崩溃点改为调用该函数。
- `to_shared` / `shared_to_string` 一并移入该头，使回归测试无需引入 Studio 状态头。
- `QueueImageItem::log_text` 仍为 `slint::SharedString`（保持日志共享存储，避免每行
  重建时复制整行日志），字段处补充“清空必须走 `clear_shared_string`”的约束注释。

## 本轮实际执行的验证

| 项目 | 结果 |
| --- | --- |
| Windows Release 受影响子集（`update_model_core`、`changelog_history_complete`、`changelog_chunk_boundary`、`ui_smoke`、`ui_queue_stress`） | 5/5 通过（`build/x64/Release`，`ctest -C Release`） |
| 负向对照：把 `clear_shared_string` 改回 `value = {}` 后重跑 `ui_smoke` | **SegFault 失败**（复现原闪退机理），恢复修复后再次通过 |
| Linux Release 受影响子集（同一提交的原生 WSL 构建） | 5/5 通过（`build/release-tests`） |
| 发行二进制静态核对：1.0.15 `AWJ.exe` 的 `begin_queue_conversion_run`（`0x14091E1A0`～`0x14091FB60`） | 195 个调用点、**0 个 `xor ecx,ecx`**，即 1.0.14 崩溃点那种 `call strlen` 空指针调用已消失 |
| 版本一致性 | Windows `AWJ.exe --version` 与 Linux `AWJ --version` 均为 `AWJimage 1.0.15` |
| 归档门槛（`scripts/package-linux-release.sh`、`scripts/package-release.ps1` 自动执行） | `7z t`、精确成员集合、全新解压逐文件 SHA-256、`--help`/可执行位检查全部通过 |

## 未执行与残余风险

- 未运行完整 Windows/Linux 测试套件（按维护者“只跑受影响范围”的要求）；历史全量
  结果只对应 1.0.14 之前的提交，不能作为本轮证据。
- 未执行自动更新端到端测试（无 VM/WSL 更新 E2E）。
- 未由本机自动化完成实机 UI 复现（原生 UI 自动化不可用）：需要用测试图
  `D:\图片\[Stray]   2025_5_31 23_16_26.jxr` 导入队列 → 开始转换 → 界面不退出、
  生成输出、`AWJ-crash.log` 不新增 `stage=studio-ui` 行。该步由维护者执行。
- 残余风险：`slint::SharedString`（以及 `std::string` 的 `operator=(const char*)`）
  仍接受空指针赋值；若将来在别处新写 `value = {}` / `= nullptr`，仍会以同样方式崩溃。
  本轮以“唯一安全清空入口 + 注释 + 回归测试”压制，未引入类型级包装（会改动队列项
  类型及其与 `TaskRow` 的所有转换点，超出补丁范围）。
- 2026-09-15 的 `AWJ-crash.log` 中另有一条 `unhandled-seh stage=cli code=0xe06d7363`
  记录，属 CLI 路径上未复现的独立问题，不在 1.0.15 范围。

## 发布记录（2026-09-18）

- 分支 `codex/1.0.15` 已推送：`bd72977`（修复与版本/日志）→ `292e642`（归档与签名 manifest、本文档）。
- tag `1.0.15` 指向源码提交 `bd7297706f492392e064d2abf5b0ab3a4330edda`；Windows 与 Linux
  发行二进制均由该提交的干净工作树构建。
- `master` 已合入本分支与远端 README 更新并推送。
- GitHub Release `1.0.15`（stable，非 prerelease）已发布，资产仅
  `AWJ_Win.7z`（12,021,474 字节）与 `AWJ_Linux.7z`（19,447,807 字节）。
- 公开下载核验：两个资产返回 HTTP 200 且大小与本地一致、SHA-256 与 manifest 记录一致；
  `https://raw.githubusercontent.com/Dominic485649/AWJimage/master/update-archive.json`
  已是 sequence 10 且含 1.0.15，detached 签名用 `release-2026` 公钥验证通过。
- 仍未执行更新器端到端（下载→安装→健康检查→回滚）与实机 UI 复现。
