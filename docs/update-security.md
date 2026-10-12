# 自动更新签名与密钥轮换

此文档是 AWJ 1.0.6 及后的发布操作约束。归档、manifest、keyring 和所有临时文件都只能位于仓库 `build/`、`bin/` 或仓库根的受版本控制 manifest 文件；私钥绝不属于仓库或发布资产。

## 私钥保管

签名 seed 保存在仓库外的受保护目录，由环境变量或参数提供：

```text
<external-key-directory>\update-ed25519-seed.hex
```

该目录在仓库外。`.gitignore` 同时忽略 `**/*update-ed25519-seed*.hex` 和 `**/*update-ed25519*.seed`，但 Git ignore 不是保管措施：不得把 seed 复制到仓库、`build/`、`bin/`、Release 资产、日志、CI 输出或聊天记录。legacy、两把 recovery root 及每把 release seed 都须保存在相互独立的离线加密备份中；丢失或怀疑泄露时立即按下文撤销流程处理。

## 信任链与防重放

客户端先验签原始 UTF-8 字节，再解析 JSON。公开入口仅为 `update-keyring.json` / `.sig` 和 `update-archive.json` / `.sig`；网络错误、缺失、验签失败、解析失败和防重放失败都拒绝更新。1.2.1 停止请求和生成旧入口，1.0.12 及更早版本需手动升级。已落盘的旧事务仍可恢复。

keyring 由编入客户端的三把 root 中至少两把不同密钥签名，委派有 `key_id`、公钥、有效期和撤销标志的 release key。keyring 和归档清单分别包含递增 `sequence`、`issued_at`、`expires_at`，最长有效期 180 天。过期或明显晚于本机时间的文档拒绝。

客户端在程序旁的 `.awj-update-security-state.json` 保存最后验证的 sequence 和原始 SHA-256，使用跨进程锁、同目录临时文件、刷盘和原子替换。较小 sequence、同 sequence 的不同内容和损坏状态均拒绝。保留旧状态迁移与 `AWJ.jsonc` 中 sequence 的额外下限。

这个机制不能抵抗能删除/替换应用目录及状态文件的本机同权限攻击者；这类攻击已等同于篡改可执行文件。网络决策仍保留 URL 白名单、验签后解析、归档及成员 size/SHA-256 校验、安全解压、helper 二次验签、同卷 staging、健康检查和回滚。

## 发布与轮换

1. 先创建或更新 keyring 原文。每个 release key 必须有唯一小写 `key_id`；新密钥先加入并设置 `not_before`，旧密钥保留到使用它签发的 manifest 都自然过期。
2. 由至少两把仍安全的 root 签名 keyring。脚本从 CMake 读取编译进客户端的 root 公钥，并要求提供的 seed 一一匹配：

   ```powershell
   .\scripts\sign-update-keyring.ps1 `
     -KeyringPath .\update-keyring.json `
     -RootSeedFiles @{
       'root-legacy-2026' = '<external-key-directory>\update-ed25519-seed.hex'
       'root-recovery-a-2026' = '<external-key-directory>\update-ed25519-root-recovery-a-2026.hex'
     } `
     -SignerPath .\bin\x64\Release\awj_update_manifest_sign.exe
   ```

3. 提交并公开下载已签名的 keyring 与 `.sig`，确认它的 sequence 比任何客户端已见版本高；然后用 keyring 中对应的 release seed 签 manifest。`scripts/package-release.ps1` 先用编译 root 验签 keyring 封套后才解析，强制 `-ManifestKeyId`、`-ManifestExpiresAtUtc`，并验证该 ID 是 keyring 中未撤销且与 `-UpdatePublicKeyHex` 相同的公钥。已有 manifest 仍由上一把公钥验签时，显式传入 `-ExistingManifestPublicKeyHex`。
4. release seed 泄露时，用两把未泄露 root 发布更高 sequence 的 keyring：把泄露 release key 的 `revoked` 设为 `true`，加入新 release key，随后只由新 key 签 manifest。不要等待旧 manifest 过期。
5. root seed 丢失或泄露时，先用其余两把 root 发布 release-key 轮换；随后在一个正常签名的新客户端中替换编译 root 集，等旧客户端停止支持后才退役旧 root。root 是 bootstrap trust anchor，不能由单把 release key 自行改写。

GitHub 已启用 Immutable Releases；对其生效后的 Release，发布前必须确认 tag、名称、资产、哈希和 prerelease 状态，不能依赖上传后的修订。
