param(
    [string]$VcpkgRoot = "",
    [string]$VcpkgTriplet = "",
    [switch]$StaticRuntime,
    [switch]$DynamicRuntime,
    [switch]$SharedSlint,
    [switch]$NoVcpkgInstall,
    [switch]$EnableLto = $true,
    [ValidateRange(1, 64)] [int]$BuildParallelism = 2,
    [switch]$CleanDependencies,
    [string]$DependencyCacheRoot = "",
    [string]$UpdatePublicKeyHex = $env:AWJ_UPDATE_PUBLIC_KEY_HEX,
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
$DependencyCacheRoot = [IO.Path]::GetFullPath($DependencyCacheRoot, $Repo)
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
    $ReleaseOutput = $Path.StartsWith('bin/x64/Release/') -and
        [IO.Path]::GetFileName($Path) -in $ReleaseFiles
    -not $ReleaseOutput
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

if ($StaticRuntime -and $DynamicRuntime) {
    throw "不能同时指定 -StaticRuntime 和 -DynamicRuntime。"
}
if ($SharedSlint) {
    throw "Release 产物必须静态链接 Slint；请不要使用 -SharedSlint。"
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
$CodecLock = Get-Content (Join-Path $Repo 'cmake/codec-dependencies.json') -Raw | ConvertFrom-Json
$PinnedFetchContentCommits = @{
    "libavif" = $CodecLock.libavif.commit
    "jpegli"  = $CodecLock.jpegli.commit
    "slint"   = $CodecLock.slint.commit
}
$PinnedFetchContentPatchedFiles = @{
    "libavif" = @(
        " M include/avif/avif.h",
        " M src/write.c"
    )
    "slint"   = @(
        " M api/cpp/Cargo.toml",
        " M api/cpp/include/private/slint_config.h",
        " M api/cpp/include/private/slint_models.h",
        " M internal/backends/winit/event_loop.rs",
        " M internal/backends/winit/winitwindowadapter.rs",
        " M internal/core/window.rs",
        " M internal/renderers/femtovg/itemrenderer.rs"
    )
}
$FetchContentSourceOverrides = @()
$ExistingCache = if (Test-Path (Join-Path $BuildDir 'CMakeCache.txt')) {
    Get-Content (Join-Path $BuildDir 'CMakeCache.txt')
} else { @() }
foreach ($FetchContentName in @("libavif", "jpegli", "slint")) {
    $SourceDir = Join-Path $BuildDir "_deps\$FetchContentName-src"
    $Override = @($ExistingCache | Where-Object { $_ -match "^FETCHCONTENT_SOURCE_DIR_$($FetchContentName.ToUpperInvariant()):PATH=.+$" })
    if ($Override.Count -eq 1) { $SourceDir = $Override[0].Substring($Override[0].IndexOf('=') + 1) }
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
# NoVcpkgInstall 同时约束配置阶段，不能让工具链因缓存 ABI 差异重新安装依赖。
$ConfigureArgs += "-DVCPKG_MANIFEST_INSTALL=$(if ($NoVcpkgInstall) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$DependencyBuildtrees;--x-packages-root=$DependencyPackages"
$ConfigureArgs += "-DBUILD_TESTING=OFF"
$ConfigureArgs += "-DAVIF_STATIC_MSVC_RUNTIME=$(if ($UseStaticRuntime) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_SLINT=$(if ($SharedSlint) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAVIF_ENABLE_RELEASE_IPO=$(if ($EnableLto) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAWJ_ENABLE_X64_V3=ON"
if ($UpdatePublicKeyHex) {
    $ConfigureArgs += "-DAWJ_UPDATE_PUBLIC_KEY_HEX=$UpdatePublicKeyHex"
}
$ConfigureArgs += $FetchContentSourceOverrides


cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码 $LASTEXITCODE。"
}
cmake --build $BuildDir --config Release --target AWJ AWJ-com awj_update_manifest_sign --parallel $BuildParallelism

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
    $Override = @($FetchContentSourceOverrides | Where-Object { $_.StartsWith("-DFETCHCONTENT_SOURCE_DIR_$($Name.ToUpperInvariant())=") })
    if ($Override.Count -eq 1) { $SourceDir = $Override[0].Substring($Override[0].IndexOf('=') + 1) }
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
Minimum CPU: AVX2
Release IPO/LTO: $(if ($EnableLto) { 'ON' } else { 'OFF' })
Source: https://github.com/Dominic485649/AWJimage

Vcpkg baseline: $VcpkgBaseline
AOM: $AomVersion
dav1d: $Dav1dVersion
libyuv: $LibyuvVersion
libarchive: $LibarchiveVersion
libplacebo: $LibplaceboVersion
libheif: $($CodecLock.libheif.tag.TrimStart('v')) (decoder-only; libde265 backend)
libde265: $($CodecLock.libde265.tag.TrimStart('v')) (decoder library only)
libultrahdr: $($CodecLock.libultrahdr.tag.TrimStart('v')) (CPU HDR JPEG and gain maps)

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

Write-Host ""
Write-Host "Release 输出:"
Write-Host "  $OutputDir\AWJ.exe"
Write-Host "  $OutputDir\AWJ.com"
