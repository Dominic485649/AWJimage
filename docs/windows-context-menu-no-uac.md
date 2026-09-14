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
| MSIX / sparse package + HKCU COM | 现代 Explorer 读取 `IExplorerCommand`；“显示更多选项”继续读取经典 COM | DOpus 读取同一 HKCU `IContextMenu` COM | 注册过程不需要 UAC；发布包必须使用目标机信任的签名证书 | **最终方案** |
| 受限令牌写 HKLM / Registry Virtualization | `ERROR_ACCESS_DENIED (5)` | 不适用 | 否 | 不能绕过 HKLM ACL |

## 最终实现

`AWJ.ShellExtension.dll` 同时实现 `IShellExtInit`、`IContextMenu` 和 `IExplorerCommand`。经典宿主从每用户 COM 注册读取配置；Windows 11 现代 Explorer 通过同目录的签名 sparse MSIX package 激活 `IExplorerCommand`。两条路径共享同一份 `REG_MULTI_SZ Configuration`，动态生成：

- `AWJimage 转换` 父菜单；
- PNG、WebP、AVIF、JXL、JPGLI 五个格式命令；
- 配置中的预设分组及其格式子项。

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
4. 现代 Explorer package 注册后重启 Explorer；COM probe 确认 sparse manifest 可激活 `IExplorerCommand`，经典 Shell API 测试覆盖单选、多选、目录和六种命令路径；
5. 注销后确认 CLSID、文件入口、目录入口和注册事务均从 HKCU 删除，并移除当前用户 sparse package。

证据截图：

- `build/context-menu-research/dopus-awj-submenu2.png`
- `build/context-menu-research/dopus-awj-multi-submenu.png`

## 自动化验证

Release 构建：

```text
cmake --build build\\x64\\Release --config Release --parallel 1
```

CTest：Release 全量测试共 49 项；与本改动直接相关的 `shell_extension_core`、`shell_extension_com`、`shell_context_menu_logic`、`shell_context_menu_registry`、`shell_context_menu_explorer` 均通过。现代 Explorer 的可视化点击仍需在目标桌面会话中人工确认；本机已验证 package 注册状态为 `Ok`，COM surrogate 可激活。

## 限制

把文件复制到 `C:\\Program Files` 等受保护目录本身仍可能需要管理员权限；这是文件部署权限，不是右键菜单注册或使用权限。若安装器也必须完全无管理员权限，应把程序和 DLL 部署到用户可写目录（例如 `%LOCALAPPDATA%\\Programs\\AWJimage`）。在程序已经位于可读的受保护目录时，HKCU 菜单注册、sparse package 每用户注册、配置和使用均不调用 UAC。若使用自签名测试证书，首次把证书加入系统信任根可能需要管理员权限；这不是产品方案的发行方式，正式包应使用目标机已信任的代码签名证书。
