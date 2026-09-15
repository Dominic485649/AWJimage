param(
    [string]$VcpkgRoot = "",
    [string]$VcpkgTriplet = "",
    [switch]$StaticRuntime,
    [switch]$DynamicRuntime,
    [switch]$SharedSlint,
    [switch]$NoVcpkgInstall,
    [switch]$EnableLto,
    [switch]$CleanDependencies,
    [string]$DependencyCacheRoot = "",
    [ValidateSet("stable", "prerelease")]
    [string]$UpdateChannel = "stable",
    [UInt64]$UpdateManifestSequence = 0,
    [string]$UpdatePublishedAtUtc = "",
    [string]$UpdateSigningSeedFile = $env:AWJ_UPDATE_SIGNING_SEED_FILE,
    [string]$UpdatePublicKeyHex = $env:AWJ_UPDATE_PUBLIC_KEY_HEX,
    [string]$MinimumUpdaterVersion = "",
    [string]$LinuxAssetPath = "",
    [switch]$SkipUpdateManifest
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSCommandPath
# FetchContent sources are public HTTPS repositories.  Keep a developer's
# global URL rewrite from turning them into an interactive SSH fetch.
$env:GIT_CONFIG_GLOBAL = "NUL"
$env:GIT_CONFIG_NOSYSTEM = "1"
$env:GIT_TERMINAL_PROMPT = "0"
$env:GIT_CONFIG_COUNT = "1"
$env:GIT_CONFIG_KEY_0 = "http.version"
$env:GIT_CONFIG_VALUE_0 = "HTTP/1.1"
$BuildDir = Join-Path $Repo "build\x64\Release"
$OutputDir = Join-Path $Repo "bin\x64\Release"
$OverlayPorts = Join-Path $Repo "vcpkg-overlays"
$ReleaseFiles = @("AWJ.exe", "AWJ.com", "LICENSE", "NOTICE.txt", "awj_update_manifest_sign.exe")
$Version = (Get-Content (Join-Path $Repo "VERSION") -Raw).Trim()
$MinimumUpdaterVersion = if ($MinimumUpdaterVersion) { $MinimumUpdaterVersion } else { $Version }
$VcpkgConfiguration = Get-Content (Join-Path $Repo "vcpkg-configuration.json") -Raw | ConvertFrom-Json
$VcpkgBaseline = $VcpkgConfiguration.'default-registry'.baseline

# 1.0.4+ updates are signed archive manifests, not a Windows-only raw asset
# build. Keep this script as the Windows builder; the bounded cross-platform
# packager owns the only manifest-writing path.
if (-not $SkipUpdateManifest) {
    throw "release.ps1 只构建 Windows 资产；请使用 scripts\\package-release.ps1 生成并签名归档 manifest。"
}

function Remove-RepoDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $ResolvedRepo = [IO.Path]::GetFullPath($Repo).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $ResolvedPath = [IO.Path]::GetFullPath($Path)
    if (-not $ResolvedPath.StartsWith($ResolvedRepo, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理仓库外目录: $ResolvedPath"
    }
    Remove-Item -LiteralPath $ResolvedPath -Recurse -Force
}

if (-not $DependencyCacheRoot) {
    $DependencyCacheRoot = Join-Path $Repo "build\dependency-cache\$Version"
}
if ($CleanDependencies) {
    Remove-RepoDirectory $BuildDir
    Remove-RepoDirectory (Join-Path $Repo "vcpkg_installed")
    Remove-RepoDirectory $DependencyCacheRoot
}
$DependencyBinaryCache = Join-Path $DependencyCacheRoot "binary-cache"
$DependencyBuildtrees = Join-Path $DependencyCacheRoot "buildtrees"
$DependencyPackages = Join-Path $DependencyCacheRoot "packages"
New-Item -ItemType Directory -Path $DependencyBinaryCache, $DependencyBuildtrees, $DependencyPackages -Force | Out-Null
$env:VCPKG_DEFAULT_BINARY_CACHE = $DependencyBinaryCache
$GitCommit = (git -C $Repo rev-parse HEAD).Trim()
$GitStatus = @(git -C $Repo status --porcelain=v1 --untracked-files=all)
$GitDirty = @($GitStatus | Where-Object {
    $Path = $_.Substring(3).Trim('"').Replace('\', '/')
    $GeneratedManifest = $Path -in @('update-manifest.json', 'update-manifest.json.sig')
    $ReleaseOutput = $Path.StartsWith('bin/x64/Release/') -and
        [IO.Path]::GetFileName($Path) -in $ReleaseFiles
    -not ($GeneratedManifest -or $ReleaseOutput)
}).Count -gt 0
try {
    $GitTag = (git -C $Repo describe --tags --exact-match HEAD 2>$null).Trim()
} catch {
    $GitTag = ""
}
if ($GitDirty) {
    $GitCommit += "-dirty"
    $GitTag = ""
}
elseif ($GitTag -ne $Version) {
    $GitTag = ""
}

if (-not $VcpkgRoot) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    }
    elseif (Test-Path "D:\Scoop\apps\vcpkg\current") {
        $VcpkgRoot = "D:\Scoop\apps\vcpkg\current"
    }
}

function Ensure-VcpkgManifestPackages([string]$Root, [string]$Triplet, [string[]]$PackageShareNames, [bool]$NoInstall) {
    if (-not $Root) {
        throw "未找到 vcpkg。请设置 VCPKG_ROOT，或传入 -VcpkgRoot。"
    }

    $Missing = @()
    foreach ($PackageShareName in $PackageShareNames) {
        $BuildManifestInstalled = Join-Path $BuildDir "vcpkg_installed\$Triplet\share\$PackageShareName"
        if (Test-Path -LiteralPath $BuildManifestInstalled -PathType Container) {
            continue
        }
        $Missing += $PackageShareName
    }

    if ($Missing.Count -eq 0) {
        return
    }

    if ($NoInstall) {
        throw "未安装 vcpkg manifest 依赖 ($($Missing -join ', '))。请先运行: `"$Root\vcpkg.exe`" install --triplet $Triplet"
    }

    $VcpkgExe = Join-Path $Root "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $VcpkgExe -PathType Leaf)) {
        throw "未找到 vcpkg.exe: $VcpkgExe"
    }

    Write-Host "未发现 vcpkg manifest 依赖 ($($Missing -join ', '))，开始使用 vcpkg 安装..."
    Push-Location $Repo
    try {
        & $VcpkgExe install --triplet $Triplet `
            "--overlay-ports=$OverlayPorts" `
            "--x-install-root=$(Join-Path $BuildDir 'vcpkg_installed')" `
            "--x-buildtrees-root=$DependencyBuildtrees" `
            "--x-packages-root=$DependencyPackages"
        if ($LASTEXITCODE -ne 0) {
            throw "vcpkg install --triplet $Triplet 失败，退出码 $LASTEXITCODE。"
        }
    }
    finally {
        Pop-Location
    }
}

function Get-VcpkgPackageVersion([string]$Root, [string]$Triplet, [string]$Package) {
    $InfoDirs = @((Join-Path $BuildDir "vcpkg_installed\vcpkg\info"))
    foreach ($InfoDir in $InfoDirs) {
        $File = Get-ChildItem -LiteralPath $InfoDir -Filter "${Package}_*_${Triplet}.list" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($File.Name -match "^$([Regex]::Escape($Package))_(.+)_$([Regex]::Escape($Triplet))\.list$") {
            return $Matches[1]
        }
    }
    return "unknown"
}

function Get-ChangelogEntry([string]$Path, [string]$ReleaseVersion) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "缺少更新日志: $Path"
    }
    $Text = (Get-Content -LiteralPath $Path -Raw).Replace("`r`n", "`n")
    $Pattern = "(?ms)^## $([Regex]::Escape($ReleaseVersion)) - [^`n]+`n(?<body>.*?)(?=^## |\z)"
    $Match = [Regex]::Match($Text, $Pattern)
    if (-not $Match.Success) {
        throw "更新日志 $Path 缺少版本 $ReleaseVersion 的独立条目。"
    }
    $Body = $Match.Groups['body'].Value.Trim()
    if (-not $Body) {
        throw "更新日志 $Path 的版本 $ReleaseVersion 条目为空。"
    }
    return $Body
}

function Get-StrictUpdateVersionParts([string]$Text, [string]$FieldName) {
    $Match = [Regex]::Match($Text, '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $Match.Success) {
        throw "$FieldName 必须是无前导零的纯 MAJOR.MINOR.PATCH。"
    }
    $Parts = @()
    foreach ($Index in 1..3) {
        [UInt32]$Part = 0
        if (-not [UInt32]::TryParse(
                $Match.Groups[$Index].Value,
                [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$Part)) {
            throw "$FieldName 的版本号字段超出 UInt32 范围。"
        }
        $Parts += $Part
    }
    return $Parts
}

function Compare-UpdateVersionParts([UInt32[]]$Left, [UInt32[]]$Right) {
    foreach ($Index in 0..2) {
        if ($Left[$Index] -lt $Right[$Index]) { return -1 }
        if ($Left[$Index] -gt $Right[$Index]) { return 1 }
    }
    return 0
}

function Get-UpdateVersionSortKey([string]$Text) {
    $Parts = @(Get-StrictUpdateVersionParts $Text "manifest version")
    return '{0:D10}.{1:D10}.{2:D10}' -f $Parts[0], $Parts[1], $Parts[2]
}

if ($StaticRuntime -and $DynamicRuntime) {
    throw "不能同时指定 -StaticRuntime 和 -DynamicRuntime。"
}
if ($SharedSlint) {
    throw "Release 产物必须静态链接 Slint；请不要使用 -SharedSlint。"
}
if (-not $SkipUpdateManifest) {
    $CurrentVersionParts = @(Get-StrictUpdateVersionParts $Version "VERSION")
    if ($UpdateManifestSequence -eq 0) {
        throw "发布更新必须通过 -UpdateManifestSequence 提供大于 0 的递增 sequence；开发构建请显式使用 -SkipUpdateManifest。"
    }
    if ($UpdatePublicKeyHex -notmatch '^[0-9a-f]{64}$') {
        throw "-UpdatePublicKeyHex / AWJ_UPDATE_PUBLIC_KEY_HEX 必须是 64 位小写十六进制 Ed25519 公钥。"
    }
    $ParsedPublishedAt = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParseExact(
            $UpdatePublishedAtUtc,
            "yyyy-MM-dd'T'HH:mm:ss'Z'",
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal -bor
                [Globalization.DateTimeStyles]::AdjustToUniversal,
            [ref]$ParsedPublishedAt)) {
        throw "发布更新必须通过 -UpdatePublishedAtUtc 提供严格 UTC 时间 YYYY-MM-DDTHH:MM:SSZ。"
    }
    if (-not $UpdateSigningSeedFile -or
        -not (Test-Path -LiteralPath $UpdateSigningSeedFile -PathType Leaf)) {
        throw "发布更新必须通过 -UpdateSigningSeedFile 或 AWJ_UPDATE_SIGNING_SEED_FILE 提供仓库外的 Ed25519 seed 文件。"
    }
    $ResolvedRepoRoot = [IO.Path]::GetFullPath($Repo).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $ResolvedSeedPath = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $UpdateSigningSeedFile).Path)
    if ($ResolvedSeedPath.StartsWith($ResolvedRepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Ed25519 seed 必须位于仓库外，拒绝使用仓库内的私钥材料。"
    }
    if (-not $LinuxAssetPath -or
        -not (Test-Path -LiteralPath $LinuxAssetPath -PathType Leaf)) {
        throw "发布 manifest 需要 -LinuxAssetPath 指向已验证的 Linux x64 AWJ 资产。"
    }
    $ChangelogZh = Get-ChangelogEntry (Join-Path $Repo "CHANGELOG.md") $Version
    $ChangelogEn = Get-ChangelogEntry (Join-Path $Repo "CHANGELOG.en.md") $Version
    $null = @(Get-StrictUpdateVersionParts $MinimumUpdaterVersion
        "-MinimumUpdaterVersion")
}

$UseStaticRuntime = -not [bool]$DynamicRuntime
if ($StaticRuntime) {
    $UseStaticRuntime = $true
}

if (-not $VcpkgTriplet) {
    $VcpkgTriplet = if ($UseStaticRuntime) { "x64-windows-static" } else { "x64-windows" }
}

Ensure-VcpkgManifestPackages $VcpkgRoot $VcpkgTriplet @(
    "scnlib",
    "libwebp",
    "libjxl",
    "libpng",
    "libjpeg-turbo",
    "giflib",
    "tiff",
    "libraw",
    "libarchive",
    "libplacebo",
    "aom",
    "dav1d",
    "libyuv",
    "libsodium",
    "nlohmann-json"
) $NoVcpkgInstall

# Release pins must take effect even when an earlier configure populated FetchContent.
# Reuse only clean sources whose resolved commit already matches the release pin.
$PinnedFetchContentCommits = @{
    "libavif" = "c5240fc79fe5c2407e10afd35f5505ef6333ea49"
    "jpegli"  = "031a0077f5799a6041004267fc12b956c1f52a20"
    "slint"   = "cf62c975c311e7036d599ed8ed0b7e6a8386a934"
}
$PinnedFetchContentPatchedFiles = @{
    "libavif" = @(
        " M src/write.c"
    )
    "slint"   = @(
        " M api/cpp/include/private/slint_config.h",
        " M internal/backends/winit/accesskit.rs",
        " M internal/backends/winit/event_loop.rs",
        " M internal/core/window.rs"
    )
}
$FetchContentSourceOverrides = @()
foreach ($FetchContentName in @("libavif", "jpegli", "slint")) {
    $SourceDir = Join-Path $BuildDir "_deps\$FetchContentName-src"
    $ExpectedCommit = $PinnedFetchContentCommits[$FetchContentName]
    $KeepSource = $ExpectedCommit -and
        (Test-Path -LiteralPath (Join-Path $SourceDir ".git"))
    if ($KeepSource) {
        $ResolvedCommit = (& git -C $SourceDir rev-parse HEAD).Trim()
        $SourceStatus = @(git -C $SourceDir status --porcelain --untracked-files=all)
        $ExpectedPatchedFiles = @($PinnedFetchContentPatchedFiles[$FetchContentName])
        $StatusMatches = if ($SourceStatus.Count -eq 0) {
            $true
        }
        elseif ($ExpectedPatchedFiles.Count -gt 0 -and
                $SourceStatus.Count -eq $ExpectedPatchedFiles.Count) {
            @(Compare-Object -ReferenceObject ($ExpectedPatchedFiles | Sort-Object) `
                             -DifferenceObject ($SourceStatus | Sort-Object)).Count -eq 0
        }
        else {
            $false
        }
        $KeepSource = $LASTEXITCODE -eq 0 -and $ResolvedCommit -eq $ExpectedCommit -and
            $StatusMatches
    }
    if (-not $KeepSource) {
        Remove-RepoDirectory $SourceDir
    }
    else {
        if ($FetchContentName -eq "slint") {
            cmake "-DSOURCE_DIR=$SourceDir" -P (Join-Path $Repo "cmake\patch_slint_static_import.cmake")
            if ($LASTEXITCODE -ne 0) {
                throw "无法修补缓存 Slint 源码，退出码 $LASTEXITCODE。"
            }
        }
        $FetchContentSourceOverrides +=
            "-DFETCHCONTENT_SOURCE_DIR_$($FetchContentName.ToUpperInvariant())=$SourceDir"
    }
    foreach ($FetchContentSuffix in @("build", "tmp")) {
        Remove-RepoDirectory (Join-Path $BuildDir "_deps\$FetchContentName-$FetchContentSuffix")
    }
}
if (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt")) {
    Remove-Item -LiteralPath (Join-Path $BuildDir "CMakeCache.txt") -Force
}
Remove-RepoDirectory (Join-Path $BuildDir "CMakeFiles")

$ConfigureArgs = @(
    "-U", "scn_DIR",
    "-U", "FastFloat_DIR",
    "-U", "fast_float_DIR",
    "-U", "AWJ_LIBAVIF_GIT_REPOSITORY",
    "-U", "AWJ_LIBAVIF_GIT_TAG",
    "-U", "AWJ_JPEGLI_GIT_REPOSITORY",
    "-U", "AWJ_JPEGLI_GIT_TAG",
    "-U", "AWJ_SLINT_GIT_REPOSITORY",
    "-U", "AWJ_SLINT_GIT_TAG",
    "-S", $Repo,
    "-B", $BuildDir,
    "-G", "Visual Studio 18 2026",
    "-A", "x64"
)
if ($VcpkgRoot) {
    $ConfigureArgs += "-DVCPKG_ROOT=$VcpkgRoot"
    $ConfigureArgs += "-DVCPKG_TRIPLET=$VcpkgTriplet"
    $ConfigureArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
    $ToolchainPath = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    $ConfigureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainPath"
}
$ConfigureArgs += "-DAWJ_VCPKG_LIBRARY_CONFIG=Release"
$ConfigureArgs += "-DVCPKG_OVERLAY_PORTS=$OverlayPorts"
$ConfigureArgs += "-DVCPKG_INSTALLED_DIR=$(Join-Path $BuildDir 'vcpkg_installed')"
$ConfigureArgs += "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$DependencyBuildtrees;--x-packages-root=$DependencyPackages"
$ConfigureArgs += "-DBUILD_TESTING=OFF"
$ConfigureArgs += "-DAVIF_STATIC_MSVC_RUNTIME=$(if ($UseStaticRuntime) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_SLINT=$(if ($SharedSlint) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAVIF_ENABLE_RELEASE_IPO=$(if ($EnableLto) { 'ON' } else { 'OFF' })"
if ($UpdatePublicKeyHex) {
    $ConfigureArgs += "-DAWJ_UPDATE_PUBLIC_KEY_HEX=$UpdatePublicKeyHex"
}
$ConfigureArgs += $FetchContentSourceOverrides


cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码 $LASTEXITCODE。"
}
cmake --build $BuildDir --config Release --target AWJ AWJ-com awj_update_manifest_sign --parallel

if ($LASTEXITCODE -ne 0) {
    throw "Release 构建失败，退出码 $LASTEXITCODE。"
}

# --- Generate NOTICE.txt ---
$BuildDate = (Get-Date).ToString("yyyy-MM-ddTHH:mm:sszzz")
$AomVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "aom"
$Dav1dVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "dav1d"
$LibyuvVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "libyuv"
$LibarchiveVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "libarchive"
$LibplaceboVersion = Get-VcpkgPackageVersion $VcpkgRoot $VcpkgTriplet "libplacebo"
$FetchContentCommit = {
    param([string]$Name)
    $SourceDir = Join-Path $BuildDir "_deps\$Name-src"
    $CommitMarker = Join-Path $SourceDir ".awj-source-commit"
    if (Test-Path -LiteralPath $CommitMarker -PathType Leaf) {
        $Commit = (Get-Content -LiteralPath $CommitMarker -Raw).Trim()
        if ($Commit -notmatch "^[0-9a-f]{40}$") {
            throw "FetchContent commit marker is invalid: $CommitMarker"
        }
        return $Commit
    }
    if (-not (Test-Path -LiteralPath (Join-Path $SourceDir ".git"))) {
        return "not-built"
    }
    $Commit = (& git -C $SourceDir rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $Commit) {
        throw "无法读取 FetchContent commit: $SourceDir"
    }
    return $Commit
}
$LibavifCommit = & $FetchContentCommit "libavif"
$JpegliCommit = & $FetchContentCommit "jpegli"
$SlintCommit = & $FetchContentCommit "slint"
$GitTagLine = if ($GitTag) { "Git Tag: $GitTag" } else { "Git Tag:" }

$BuildInfoContent = @"
AWJimage $Version
Build Date: $BuildDate
Build Type: Release
Git Commit: $GitCommit
$GitTagLine
Architecture: x64
Source: https://github.com/Dominic485649/AWJimage

Vcpkg baseline: $VcpkgBaseline
AOM: $AomVersion
dav1d: $Dav1dVersion
libyuv: $LibyuvVersion
libarchive: $LibarchiveVersion
libplacebo: $LibplaceboVersion
libheif: 1.23.4 (decoder-only; libde265 backend)
libde265: 1.1.2 (decoder library only)

FetchContent Dependencies (actual commits):
  libavif:     $LibavifCommit
  jpegli:      $JpegliCommit
  slint:       $SlintCommit
"@

$ThirdPartyNotices = (Get-Content -LiteralPath (Join-Path $Repo "THIRD_PARTY_NOTICES.txt") -Raw).TrimEnd()
$LicenseAppendix = @(
    Get-ChildItem -LiteralPath (Join-Path $Repo "licenses") -File | Sort-Object Name | ForEach-Object {
        $LicenseText = (Get-Content -LiteralPath $_.FullName -Raw).TrimEnd()
        @"
-------------------------------------------------------------------------------
BEGIN FULL LICENSE: licenses/$($_.Name)
-------------------------------------------------------------------------------
$LicenseText
-------------------------------------------------------------------------------
END FULL LICENSE: licenses/$($_.Name)
-------------------------------------------------------------------------------
"@
    }
) -join "`r`n"
$NoticeContent = @"
AWJimage Release Notice
=======================

BUILD INFORMATION
-----------------
$BuildInfoContent

THIRD-PARTY SOFTWARE NOTICES
----------------------------
$ThirdPartyNotices

FULL THIRD-PARTY LICENSE TEXTS
------------------------------
$LicenseAppendix
"@
$NoticePath = Join-Path $OutputDir "NOTICE.txt"
$NoticeWriteError = $null
for ($NoticeAttempt = 1; $NoticeAttempt -le 8; ++$NoticeAttempt) {
    try {
        Set-Content -LiteralPath $NoticePath -Value $NoticeContent -NoNewline -Encoding UTF8
        $NoticeWriteError = $null
        break
    } catch {
        $NoticeWriteError = $_
        if ($NoticeAttempt -lt 8) {
            Start-Sleep -Milliseconds 250
        }
    }
}
if ($NoticeWriteError) {
    throw $NoticeWriteError
}
Write-Host "  $NoticePath"
Copy-Item -LiteralPath (Join-Path $Repo "LICENSE") -Destination (Join-Path $OutputDir "LICENSE") -Force

if (-not $SkipUpdateManifest) {
    $WindowsExe = Join-Path $OutputDir "AWJ.exe"
    $WindowsCom = Join-Path $OutputDir "AWJ.com"
    foreach ($Asset in @($WindowsExe, $WindowsCom, $LinuxAssetPath)) {
        if (-not (Test-Path -LiteralPath $Asset -PathType Leaf)) {
            throw "更新资产不存在: $Asset"
        }
    }
    $ManifestPath = Join-Path $Repo "update-manifest.json"
    $SignaturePath = Join-Path $Repo "update-manifest.json.sig"
    $Signer = Join-Path $OutputDir "awj_update_manifest_sign.exe"
    if (-not (Test-Path -LiteralPath $Signer -PathType Leaf)) {
        throw "缺少 manifest 签名工具: $Signer"
    }
    $ExistingEntries = @()
    $ExistingSequence = [UInt64]0
    if (Test-Path -LiteralPath $ManifestPath -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $SignaturePath -PathType Leaf)) {
            throw "现有 update-manifest.json 缺少 detached signature。"
        }
        & $Signer --verify $ManifestPath $UpdatePublicKeyHex $SignaturePath
        if ($LASTEXITCODE -ne 0) {
            throw "现有 update-manifest.json 未通过内置公钥验签，拒绝在其基础上续签。"
        }
        $ExistingManifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
        if ($ExistingManifest.schema -ne 1 -or $null -eq $ExistingManifest.sequence -or
            [UInt64]$ExistingManifest.sequence -eq 0 -or
            $ExistingManifest.PSObject.Properties.Name -notcontains 'entries') {
            throw "现有 update-manifest.json schema/sequence 非法。"
        }
        $ExistingSequence = [UInt64]$ExistingManifest.sequence
        $ExistingEntries = @($ExistingManifest.entries)
        $SeenVersions = @{}
        $HighestVersion = $null
        $HighestVersionParts = $null
        foreach ($Entry in $ExistingEntries) {
            if ($null -eq $Entry -or $Entry.channel -notin @('stable', 'prerelease')) {
                throw "现有 update-manifest.json 包含非法条目或渠道。"
            }
            $EntryVersion = [string]$Entry.version
            $EntryParts = @(Get-StrictUpdateVersionParts $EntryVersion "update-manifest.json entry version")
            if ($SeenVersions.ContainsKey($EntryVersion)) {
                throw "update-manifest.json 版本 $EntryVersion 重复；同一版本号不得重新发布或跨渠道复用。"
            }
            $SeenVersions[$EntryVersion] = $true
            if ($null -eq $HighestVersionParts -or
                (Compare-UpdateVersionParts $EntryParts $HighestVersionParts) -gt 0) {
                $HighestVersion = $EntryVersion
                $HighestVersionParts = $EntryParts
            }
        }
        if ($null -ne $HighestVersionParts -and
            (Compare-UpdateVersionParts $CurrentVersionParts $HighestVersionParts) -le 0) {
            throw "新版本 $Version 必须严格高于 manifest 现有最高版本 $HighestVersion。"
        }
    }
    if ($UpdateManifestSequence -le $ExistingSequence) {
        throw "manifest sequence 必须严格递增：现有 $ExistingSequence，本次 $UpdateManifestSequence。"
    }

    function New-AssetRecord([string]$Path, [string]$Url) {
        return [ordered]@{
            url = $Url
            size = [UInt64](Get-Item -LiteralPath $Path).Length
            sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $ReleaseBase = "https://github.com/Dominic485649/AWJimage/releases"
    $CurrentEntry = [ordered]@{
        version = $Version
        channel = $UpdateChannel
        published_at = $UpdatePublishedAtUtc
        release_url = "$ReleaseBase/tag/$Version"
        minimum_updater_version = $MinimumUpdaterVersion
        revoked = $false
        assets = [ordered]@{
            windows_x64_exe = New-AssetRecord $WindowsExe "$ReleaseBase/download/$Version/AWJ.exe"
            windows_x64_com = New-AssetRecord $WindowsCom "$ReleaseBase/download/$Version/AWJ.com"
            linux_x64 = New-AssetRecord $LinuxAssetPath "$ReleaseBase/download/$Version/AWJ"
        }
        changelog = [ordered]@{
            'zh-CN' = $ChangelogZh
            en = $ChangelogEn
        }
    }
    $Entries = @($ExistingEntries) + @($CurrentEntry)
    $Entries = @($Entries | Sort-Object { Get-UpdateVersionSortKey ([string]$_.version) })
    $Manifest = [ordered]@{
        schema = 1
        sequence = $UpdateManifestSequence
        entries = $Entries
    }
    $Json = ($Manifest | ConvertTo-Json -Depth 20).Replace("`r`n", "`n") + "`n"
    $Utf8NoBom = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($ManifestPath, $Json, $Utf8NoBom)

    $DerivedPublicKey = (& $Signer $ManifestPath $UpdateSigningSeedFile $SignaturePath).Trim()
    if ($LASTEXITCODE -ne 0 -or $DerivedPublicKey -notmatch '^[0-9a-f]{64}$') {
        Remove-Item -LiteralPath $SignaturePath -Force -ErrorAction SilentlyContinue
        throw "update-manifest.json 签名失败。"
    }
    if ($DerivedPublicKey -cne $UpdatePublicKeyHex) {
        Remove-Item -LiteralPath $SignaturePath -Force -ErrorAction SilentlyContinue
        throw "签名 seed 推导出的公钥与编入 AWJ.exe 的 -UpdatePublicKeyHex 不一致。"
    }
    Write-Host "  $ManifestPath"
    Write-Host "  $SignaturePath"
}


Write-Host ""
Write-Host "Release 输出:"
Write-Host "  $OutputDir\AWJ.exe"
Write-Host "  $OutputDir\AWJ.com"
