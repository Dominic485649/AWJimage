# Windows 右键菜单无 UAC 兼容性调查

## 目标

在普通用户令牌下完成安装、配置、移除和使用，并同时支持 Windows Explorer 与 Directory Opus。兼容性以真实宿主行为为准，不假设 Explorer 支持的注册表布局也会被第三方文件管理器支持。

## 方案实验

| 方案 | Explorer | Directory Opus | 是否需要 UAC | 结论 |
| --- | --- | --- | --- | --- |
| HKLM `Explorer\\CommandStore` 静态级联（PR #3 / PR #5 路线） | 可用 | 可用 | 是 | 菜单兼容，但不满足无 UAC 目标 |
| HKCU `Explorer\\CommandStore` | 不稳定，部分场景为空 | 不稳定 | 否 | 不作为产品方案 |
| HKCU `ExtendedSubCommandsKey` 共享树 | 可展开 | 无法正常展开 | 否 | 不能满足 DOpus |
| `SystemFileAssociations` 入口 | 可见性依宿主变化 | `ContextMenu SHOWCMDS ITEMMENU` 不显示 | 否 | 不能满足 DOpus |
| MSIX / sparse package + HKCU COM | Explorer 读取 package `IExplorerCommand`；经典入口作为文件缺失时的回退 | DOpus 同时发现 package 和 HKCU handler，运行时仅保留 package 命令树 | 注册过程不需要 UAC；发布包必须使用目标机信任的签名证书 | **最终方案** |
| 受限令牌写 HKLM / Registry Virtualization | `ERROR_ACCESS_DENIED (5)` | 不适用 | 否 | 不能绕过 HKLM ACL |

## 最终实现

`AWJ.ShellExtension.dll` 同时实现 `IShellExtInit`、`IContextMenu` 和 `IExplorerCommand`。经典宿主从每用户 COM 注册读取配置；Windows 11 现代 Explorer 通过同目录的签名 sparse MSIX package 激活 `IExplorerCommand`。经典 HKCU handler 使用 `{8829EA47-8F26-4670-A910-348D2340DDAA}`，sparse package 使用独立的 `{63CBBCAE-762F-4224-92C6-B7395BFCD9E2}`；两个 CLSID 仍由同一个 DLL 类工厂提供，避免 Explorer 将两条注册路径合并后重复枚举子命令。

主程序 `AWJ.exe` 不嵌入 MSIX package identity。这样即使 sparse package 尚未安装、已失效或指向旧的外部目录，用户仍可直接启动 AWJ；现代右键菜单的 package 注册是可选增强路径，不会阻塞主程序启动。

经典路径继续读取 HKCU 下的 `REG_MULTI_SZ Configuration`，因此可保留用户预设分组并兼容 Directory Opus。现代路径只读取当前用户可写的 `%LOCALAPPDATA%\AWJimage\AWJimage.ShellExtension.Configuration` 文件；该文件由注册流程原子更新，并包含基础格式命令及用户预设分组。Windows 11 Explorer 的 `IExplorerCommand` 不支持“子命令自身再拥有子命令”，因此现代 Explorer 宿主将预设项展平为 `预设名 - 转换为 PNG` 等可执行叶命令，确保注入预设不会消失；DOpus 若直接承载命令对象则保留级联分组，若经 `dllhost.exe` 代理则同样使用展平项。文件不可用时，现代 COM 不回退到 HKCU 注册表，避免把经典菜单再次合并到 Explorer；此时 Explorer 的现代入口不显示，而经典 HKCU 入口仍可供未被抑制的宿主使用。现代 COM 使用 `KF_FLAG_NO_PACKAGE_REDIRECTION` 定位文件，避免 package identity 把路径重定向到另一份配置。两条路径共同提供：

实机发现 Directory Opus 13.25 也会同时聚合 sparse package 的 `IExplorerCommand` 和 HKCU 经典 handler；若两个入口都返回菜单，DOpus 会在同一个父菜单下显示两套格式命令。因而当现代配置文件可用时，经典 `IContextMenu` 入口会在 Windows Explorer、明确命令行包含现代 sparse CLSID 的 `dllhost.exe` COM surrogate，以及 DOpus 的 `dopus.exe`、`dopusrt.exe`、`dopuscm.exe` 宿主中返回空菜单。这样两个宿主都只使用 package-backed 的现代命令树；其他传统宿主仍可使用 HKCU 经典回退。

- `AWJimage 转换` 父菜单；
- PNG、WebP、AVIF、JXL、JPGLI 五个格式命令（可选追加 AVIF.png）；
- 配置预设分组及其格式子项；原生 Explorer 使用带预设前缀的叶命令，DOpus 直接宿主使用级联分组或在 COM surrogate 路径使用同等可执行的展平项。

经典菜单注册表只写当前用户：

- `HKCU\\Software\\Classes\\CLSID\\{8829EA47-8F26-4670-A910-348D2340DDAA}`；
- `HKCU\\Software\\Classes\\*\\shellex\\ContextMenuHandlers\\AWJimage.Classic`；
- `HKCU\\Software\\Classes\\Folder\\shellex\\ContextMenuHandlers\\AWJimage.Classic`。

现代 Explorer 另外需要同目录的 `AWJ.ContextMenu.msix`。该包只包含 manifest 和图标，使用 `uap10:AllowExternalContent` 指向外部安装目录，并声明 `windows.comServer` 与 `windows.fileExplorerContextMenus`。Release 发布前必须使用与 manifest `Publisher="CN=AWJimage"` 匹配、且目标计算机信任的证书签名；测试自签名证书仅用于本机实验，不能作为公开发行证书。

UI 注册流程使用事务快照、所有权标记、漂移校验和失败回滚；COM DLL 的自注册导出也只写 HKCU。预设在 Windows 安装目录不可写时自动切换到 `%LOCALAPPDATA%\\AWJimage\\preset`，并迁移旧的相邻预设文件，不触发提权。

## 实机验证

环境：Windows x64、Directory Opus 13.25 x64、Windows Explorer，构建选项 `AWJ_ENABLE_X64_V3=ON`。

1. 通过 `rundll32.exe AWJ.ShellExtension.dll,DllRegisterServer` 注册到当前用户，未出现 UAC；
2. DOpus 单选 PNG 时显示父菜单和五个格式子项，执行 PNG 后生成带编号的输出文件；
3. DOpus 多选两个 PNG 时仍显示完整子菜单，执行 WebP 后为两个输入分别生成输出；
4. 现代 Explorer package 注册后完全退出并重启 Explorer（必要时结束残留的 Explorer COM surrogate）；在 AVIF.png 关闭且注入一个预设的配置下，普通 COM probe 与 package surrogate probe 确认 sparse manifest 可激活 `IExplorerCommand`，Explorer surrogate 根节点返回五个直接命令及五个带预设前缀的叶命令；DOpus 的直接宿主路径保留预设级联，代理路径显示同等可执行的展平项；经典 Shell API 测试覆盖单选、多选、目录和六种命令路径；
5. 注销后确认 CLSID、文件入口、目录入口和注册事务均从 HKCU 删除，并移除当前用户 sparse package。

证据截图：

- `build/context-menu-research/dopus-awj-submenu2.png`
- `build/context-menu-research/dopus-awj-multi-submenu.png`

## 自动化验证

Release 构建：

```text
cmake --build build\\x64\\Release --config Release --parallel 1
```

CTest：Release 全量测试共 49 项，全部通过；与本改动直接相关的 `shell_extension_core`、`shell_extension_com`、`shell_context_menu_logic`、`shell_context_menu_registry`、`shell_context_menu_explorer` 均通过。`shell_extension_com` 还验证了“HKCU 含预设 + 文件配置存在”时 modern COM 优先读取文件，并枚举直接命令和预设级联项；Explorer 宿主的展平分支由同一实现按宿主进程选择。旧 Explorer 进程或 COM surrogate 尚未退出时，Explorer 可能暂时保留旧的重复菜单缓存；重启 Explorer 后应只保留 package-backed 菜单。本机已验证 package 注册状态为 `Ok`，COM surrogate 可激活。

## 限制

把文件复制到 `C:\\Program Files` 等受保护目录本身仍可能需要管理员权限；这是文件部署权限，不是右键菜单注册或使用权限。若安装器也必须完全无管理员权限，应把程序和 DLL 部署到用户可写目录（例如 `%LOCALAPPDATA%\\Programs\\AWJimage`）。在程序已经位于可读的受保护目录时，HKCU 菜单注册、sparse package 每用户注册、配置和使用均不调用 UAC。若使用自签名测试证书，首次把证书加入系统信任根可能需要管理员权限；这不是产品方案的发行方式，正式包应使用目标机已信任的代码签名证书。
