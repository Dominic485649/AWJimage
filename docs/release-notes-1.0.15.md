# AWJimage 1.0.15

正式版，2026-09-18。本版修复 Studio 队列「开始转换」必崩的闪退，并把自动检查更新的
周期由 7 天缩短为 12 小时；其余功能与 1.0.14 相同。根因证据与本轮验证范围见
[验证报告](https://github.com/Dominic485649/AWJimage/blob/master/docs/validation-1.0.15.md)。

## 发布概述

1.0.13 起，Studio 首页导入任意图片后点击「开始转换」，界面进程会在启动编码后
1～2 秒内直接消失（Windows 错误报告记录 `0xC0000005`）。本版彻底修复该崩溃，并
补齐了可复现该问题的回归测试。

## 主要变化

- **修复「开始转换」闪退**：开始编码前的队列重置用空花括号清空每项日志，而
  `slint::SharedString` 的 `operator=(const char*)` 重载会把空花括号当成空指针，
  内部 `std::string_view(nullptr)` 调用 `strlen(nullptr)`，界面进程随即以
  `0xC0000005`（读地址 0）退出。清空统一改走新增的 `awj::studio::clear_shared_string`。
  该问题与输入格式、图片大小无关，队列转换与「重试失败」都会命中。
- **自动检查更新更及时**：启动时的自动检查间隔由 7 天改为 12 小时。手动检查、
  切换渠道仍不受该间隔限制；系统时间被改到未来时依旧立即检查。

## 依赖与构建

源码提交 `bd7297706f492392e064d2abf5b0ab3a4330edda`（tag `1.0.15`）。
Windows 为 MSVC（Visual Studio 18 2026）x64、`x64-windows-static`、静态 Slint、
`AWJ_ENABLE_X64_V3=OFF`；Linux 为 GCC 16.1 原生文件系统构建、`x64-linux`、
Release IPO 开启；两个平台的发行构建均关闭测试接口（`BUILD_TESTING=OFF`）。

## 发行归档

归档已通过 `7z t`、精确成员集合、全新解压逐文件 SHA-256 与 `--version`/`--help`
检查；Linux 解压后的 `AWJ` 保留可执行位。

| 归档 | 精确内容 | 大小 | SHA-256 |
| --- | --- | ---: | --- |
| AWJ_Win.7z | AWJ.exe、AWJ.com、LICENSE、NOTICE.txt | 12,021,474 | `fe9b2910f1eec40cead3e02d9d25801dde61a6a0b46880bee67821e6ddf6fd0b` |
| AWJ_Linux.7z | AWJ、LICENSE、NOTICE.txt | 19,447,807 | `eb6b44bfede9753cd1b1900a137b3296c50d7aba1a5d95f18b0730af1886ea27` |

成员校验：

| 成员 | 大小 | SHA-256 |
| --- | ---: | --- |
| Windows `AWJ.exe` | 43,161,600 | `c8ccb0b22b619f537a2d1ddc58e19cf8f936ec8b18ebf8e88ac5a726f5a7c272` |
| Windows `AWJ.com` | 451,072 | `f4f820dfa6865032ba4561eff3bc67c39b9dd15e71cbfd99132a381da4f917a3` |
| Linux `AWJ` | 67,639,760 | `354408c9396139ecaba43c5af157363ba3a363bba0967e975753592fc3b804d7` |
