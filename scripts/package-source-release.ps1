[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Import-Module (Join-Path $PSScriptRoot 'release-safety.psm1') -Force -DisableNameChecking
$output = Assert-AwjBuildPath -RepositoryRoot $root -Path $OutputDirectory
if (Test-Path -LiteralPath $output) { throw '源码正式输出目录必须全新。' }
$version = ([IO.File]::ReadAllText((Join-Path $root 'VERSION'))).Trim()
$head = (git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw '读取源码 HEAD 失败。' }
$status = @(git -C $root status --porcelain=v1)
if ($LASTEXITCODE -or $status.Count) { throw '源码归档要求干净的正式提交。' }
$tag = (git -C $root describe --exact-match --tags HEAD).Trim()
if ($LASTEXITCODE -or $tag -cne $version) { throw '源码归档要求匹配 VERSION 的 tag。' }
$fingerprint = Get-AwjSourceFingerprint -RepositoryRoot $root
[void][IO.Directory]::CreateDirectory($output)
$name = "AWJimage-$version-source.zip"
$archive = Join-Path $output $name
& git -C $root archive --format=zip "--output=$archive" HEAD
if ($LASTEXITCODE) { throw '正式源码归档失败。' }
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName
        if (-not $names.Add($relative) -or $relative.StartsWith('/') -or $relative.Contains('\') -or
            $relative -match '(^|/)\.\.(/|$)|(^|/)(release-signing\.local\.json|[^/]*(secret|seed|private[-_]?key)[^/]*)$|\.(pem|pfx|key|seed)$') {
            throw "源码归档含非法或敏感成员：$relative"
        }
    }
    foreach ($required in @('VERSION','LICENSE','THIRD_PARTY_NOTICES.txt','sdk/CMakeLists.txt','include/awj/awj.h',
        'src/sdk/native_encode.cpp','examples/basic/basic.c','docs/sdk.md','cmake/codec-dependencies.json',
        'scripts/package-release.ps1','scripts/package-sdk-release.cmake')) {
        if (-not $names.Contains($required)) { throw "对应源码缺少 $required" }
    }
    $reader = [IO.StreamReader]::new($zip.GetEntry('VERSION').Open(), [Text.Encoding]::UTF8)
    try { if ($reader.ReadToEnd().Trim() -cne $version) { throw '源码归档版本不匹配。' } }
    finally { $reader.Dispose() }
} finally { $zip.Dispose() }
if ((git -C $root rev-parse HEAD).Trim() -cne $head -or @(git -C $root status --porcelain=v1).Count -or
    (Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $fingerprint) { throw '源码在归档期间发生变化。' }
[ordered]@{
    schema=1; kind='source'; mode='formal'; status='passed'; platform='source'; version=$version
    git_head=$head; source_dirty=$false; source_fingerprint=$fingerprint
    archive=[ordered]@{name=$name; size=[UInt64](Get-Item -LiteralPath $archive).Length;
        sha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()}
    verification=[ordered]@{source_commit='passed'; source_layout='passed'}
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'asset-evidence.json') -Encoding utf8
"正式对应源码已归档并核对：$name"
