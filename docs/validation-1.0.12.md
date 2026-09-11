# 1.0.12 验证记录

本文件与 `png-quality-1.0.12.md`、`resource-budget-1.0.12.md` 配套。原始日志保存在本机 `build/evidence/1.0.12`，Linux 构建位于 WSL 原生目录 `/home/dominic/awj-1.0.12`。本地发行包与文案经审阅后，于 2026-09-11 发布正式版。

## 稳定性

- 同一焦点回归在 Linux 临时还原 Slint 借用修复后复现 `RefCell already borrowed` panic；恢复修复后通过。`focus-negative/result.json` 记录未修复 CTest 退出 8、修复后退出 0，源码已恢复。
- `idle-run-3` 使用独立默认配置与现有配置副本，持续采样；两窗口分别运行 1976.47 与 1980.91 秒，均达到 30 分钟且正常退出 0，stderr 为空。之后两套配置均观察到重新启动。
- 空闲观察对应 `idle-run-3/{default,current}/run.json` 中的二进制 SHA-256；后续版本文字、队列布局和 PNG/AVIF 内存修正使用最终构建与回归检查验证，未把先前空闲时长记到另一个二进制哈希下。

## 界面与拖放

- UI smoke 覆盖 100/125/150/200% 缩放、中英文、明暗主题、五格式普通/菜单两页的能力显示，JPGLI 渐进与哈夫曼联动，以及普通/菜单参数隔离。
- 队列检查覆盖卡片/列表几何、按下不关闭、释放关闭、无点击穿透、180 ms 过渡、最小窗口、滚动锚点和下拉框边界；输入/输出/队列的 native drop region 与实际逻辑几何对应。
- Win32 CF_HDROP 检查包含 Unicode/长路径、COPY-only、禁止接收、异常边界、注册所有权转移及注销。原生路由源码对屏幕→客户区→Slint 逻辑坐标分层转换，输出多项拒绝，运行中锁定。
- 软件渲染生成 176 张快照，覆盖四档 DPI、中英文、明暗及五格式两页；测试工具断言实际缩放和物理像素尺寸，并校验不同缩放的像素哈希不同。参数页画布分别为 1220×2000、1525×2500、1830×3000、2440×4000；原始索引见 `ui-render-verification.json`。快照使用模拟队列与参数值，用于检查字体、排版、颜色和参数页面；不等同于对每一种显示驱动的截图验收。本轮未执行 Directory Opus 或 F6 捕获功能验收。

## Shell 与预设

- 隔离注册表回归覆盖完整 schema、重复安装/移除、修复/漂移、迁移、切换/写入失败恢复，以及预设 0/1/10/11 个注入边界；预设文件检查覆盖名称/路径碰撞、改名、删除与恢复。
- Explorer 验收通过真实 `IContextMenu::InvokeCommand`，覆盖六个普通命令（五格式及 AVIF.png）的单选、多选与目录，中文空格路径与不同工作目录，共 18 次调用、24 个可解码输出。
- 测试前后额外读取本轮涉及的 86 个当前/历史 HKCU 根，树内容逐字节比较；测试自身也保存并恢复这些注册。历史 HKLM 路径仅只读检测，未执行提权清理。

## 编码与资源

- PNG 回归覆盖 q100 像素、8/16-bit IHDR、逐通道 sBIT、alpha、灰度/调色板解码、量化端点及取消；112 行质量样本指标与人工渐变/暗部检查见 PNG 评测。
- AOM/Grid 检查覆盖奇数尺寸、色度、HDR/alpha，以及 65537×64 宽图的自动 Grid 编码/解码保持尺寸；手动限制宽度可重新规划为普通 AOM。
- AVIF 16 格资源测量与 PNG q100/q50 六格压力测量通过预算检查，低预算提前拒绝。内存数值是保守调度估算，不能解释为操作系统强制限制；HDR PQ 指标是码值指标，未替代真实 HDR 显示器验收。

## 最终构建与归档

- Windows：MSVC 19.51.36256.0，Release；45 项自动检查通过（`windows-final-layout-ctest.log`），另行执行真实 Explorer 测试并通过，总计 46 项。测试工具后续仅修正渲染缩放输入，再次运行 UI smoke 通过，产品二进制未变化。
- Linux：Clang 20.1.2、Release/O3；27 项自动检查通过（`linux-final-ctest.log`），渲染工具调整后 UI smoke 再次通过。ELF 不动态依赖 libstdc++/libgcc，仍依赖系统 Vulkan、fontconfig、freetype 和 libc。
- 最终 Explorer 注册表前后 SHA-256 均为 `c06f0086fa973b03d58bf113ff6f5b31af02577093103c35455603b2ac2cb7a7`，86 个根均与测试前一致。
- 发行归档使用现有打包脚本的本地候选模式。成员、大小、SHA-256 与打包验证结果记录在 `build/release/1.0.12` 交付目录；发布使用同一份已审阅归档，未重新打包。

## 正式发布

- [1.0.12 Release](https://github.com/Dominic485649/AWJimage/releases/tag/1.0.12) 为 stable、Latest、immutable；tag 指向构建源码 `24ac989f5a05cca7453f3c78a724b0a3818c8665`。
- GitHub 资产仅包含 `AWJ_Win.7z` 与 `AWJ_Linux.7z`。公开链接回下载后，大小、SHA-256 与已审阅归档完全一致，两包 `7z t` 通过。
- Windows：11,471,674 bytes，SHA-256 `41a5fdc6e4535f2954c2e9de942a79ff1d91532c6eef04ab8492f6516c5413b2`。
- Linux：18,007,499 bytes，SHA-256 `75cc653f3dde21460b906167bd78c1f0aa72643b3cf5fbc883c1ad8c97065745`。
- v2 更新清单使用 `release-2026` 签名，sequence 从 7 递增至 8，新增 stable 1.0.12 并保留全部历史条目；逐项核对两份归档和七个成员。keyring root 签名及发布密钥有效期检查通过；清单在公开下载验证后提交。
