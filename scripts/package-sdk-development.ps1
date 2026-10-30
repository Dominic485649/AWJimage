param(
    [Parameter(Mandatory)][string]$EvidenceDirectory,
    [string]$DeliveryRoot = 'D:/Code/Cpp/AWJimage/bin/1.3.0/dev'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory, $root)
$expected = [IO.Path]::GetFullPath((Join-Path $root 'build/sdk-evidence')) + [IO.Path]::DirectorySeparatorChar
if (-not $evidence.StartsWith($expected, [StringComparison]::OrdinalIgnoreCase)) { throw '证据必须属于本工作树的 SDK 验证目录。' }
$record = Get-Content -LiteralPath (Join-Path $evidence 'validation.json') -Raw | ConvertFrom-Json
if ($record.kind -ne 'unreleased-sdk-development-validation' -or $record.release -or $record.version -ne '1.3.0') { throw '不是可交付的 SDK 开发验证记录。' }
Import-Module (Join-Path $PSScriptRoot 'release-safety.psm1') -Force -DisableNameChecking
if ((Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $record.source_fingerprint) { throw '当前源码不再匹配验证记录。' }
$delivery = [IO.Path]::GetFullPath($DeliveryRoot)
$authorized = [IO.Path]::GetFullPath('D:/Code/Cpp/AWJimage/bin/1.3.0/dev')
if ($delivery -ne $authorized) { throw '本任务只授权交付到 1.3.0/dev。' }
Get-ChildItem -LiteralPath 'D:/Code/Cpp/AWJimage/bin' -Name | Out-Null
New-Item -ItemType Directory -Force -Path $delivery | Out-Null
$destination = Join-Path $delivery $record.run_id
if (Test-Path -LiteralPath $destination) { throw '拒绝覆盖已存在的交付目录。' }
New-Item -ItemType Directory -Path $destination | Out-Null
Copy-Item -Path (Join-Path $evidence 'install/*') -Destination $destination -Recurse
$proof = Join-Path $destination 'evidence'
New-Item -ItemType Directory -Path $proof | Out-Null
foreach ($name in @('validation.json','source-files.json','build-and-tests.log','sdk-tests.xml','install.log','exports.log','consumer-configure.log','consumer-build.log','consumer-tests.log','consumer-tests.xml','stream-tests.log')) {
    Copy-Item -LiteralPath (Join-Path $evidence $name) -Destination $proof
}
Copy-Item -LiteralPath (Join-Path $evidence 'configuration') -Destination $proof -Recurse
Copy-Item -LiteralPath (Join-Path $evidence 'AWJimage-1.3.0-development-source.zip') -Destination $destination
$notice = @"
AWJimage 1.3.0 未发布开发构建
构建标识：$($record.run_id)
源码指纹：$($record.source_fingerprint)
基线提交：$($record.baseline_commit)，隔离分支中尚有未提交修改。
仅 Windows x64 动态 SDK；公共接口只有 C ABI，.lib 是 DLL 导入库。
本地开发交付不是正式签名发布，不能作为正式 Release/自动更新资产。
Linux/WSL 按用户要求延期；HarmonyOS 未经真实 Native SDK/真机验证。
真实 WGC/物理 HDR GPU、跨设备共享及完整应用正式发布门槛仍未验收。
许可证仍为项目现有 AGPL-3.0，第三方许可另见 share/AWJimage；未给予闭源许可豁免。
此包不包含用户 FFmpeg 运行库、签名密钥、modules/BMI 或 1.2.6 工作源码。
证据原始绝对路径用于溯源；文件复制到交付目录不构成在新路径重新运行测试。
"@
$notice | Set-Content -LiteralPath (Join-Path $destination 'UNRELEASED-DEVELOPMENT.txt') -Encoding utf8
$manifest = foreach ($file in Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName) {
    $relative = [IO.Path]::GetRelativePath($destination, $file.FullName).Replace('\','/')
    "{0}  {1}" -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
}
$manifest | Set-Content -LiteralPath (Join-Path $destination 'SHA256SUMS') -Encoding utf8
# 交付前逐项重读，确认不是仅生成清单未核对文件。
foreach ($line in $manifest) {
    $hash, $relative = $line -split '  ', 2
    if ((Get-FileHash -LiteralPath (Join-Path $destination $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash) { throw '交付文件哈希核验失败。' }
}
"未发布开发 SDK 已交付并核对 SHA-256：$destination"
