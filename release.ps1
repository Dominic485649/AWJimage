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
    [string]$BuildDirectory = "",
    [string]$RuntimeOutputRoot = "",
    [ValidateSet('Visual Studio 18 2026', 'Ninja')] [string]$Generator = 'Visual Studio 18 2026',
    [string]$CompilerDirectory = "",
    [switch]$BuildTests,
    [string]$VcpkgInstalledDirectory = "",
    [string]$DependencySourceRoot = "",
    [switch]$LocalCandidate,
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
Import-Module (Join-Path $Repo 'scripts/release-safety.psm1') -Force
$Profile = if ($VcpkgTriplet) { $VcpkgTriplet } elseif ($DynamicRuntime) { 'x64-windows' } else { 'x64-windows-static' }
if ($Profile -notmatch '^[A-Za-z0-9-]+$') { throw 'vcpkg triplet 不能包含路径或特殊字符。' }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $Repo "build/current/$Profile-release" }
if (-not $RuntimeOutputRoot) { $RuntimeOutputRoot = Join-Path $Repo "build/current/$Profile-runtime" }
$BuildDir = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $BuildDirectory
$RuntimeOutputRoot = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $RuntimeOutputRoot
$OutputDir = Assert-AwjBuildPath -RepositoryRoot $Repo -Path (Join-Path $RuntimeOutputRoot 'Release')
$null = Assert-AwjBuildPath -RepositoryRoot $Repo -Path (Join-Path $RuntimeOutputRoot 'Debug')
$null = Assert-AwjBuildPath -RepositoryRoot $Repo -Path (Join-Path $RuntimeOutputRoot 'RelWithDebInfo')
$null = Assert-AwjBuildPath -RepositoryRoot $Repo -Path (Join-Path $RuntimeOutputRoot 'MinSizeRel')
$OverlayPorts = Join-Path $Repo 'vcpkg-overlays'
$ReleaseFiles = @('AWJ.exe', 'AWJ.com', 'LICENSE', 'NOTICE.txt', 'awj_update_manifest_sign.exe', 'build-evidence.json')
foreach ($Name in $ReleaseFiles) { $null = Resolve-AwjRepoPath -RepositoryRoot $Repo -Path (Join-Path $OutputDir $Name) }
$Version = (Get-Content (Join-Path $Repo 'VERSION') -Raw).Trim()
$VcpkgConfiguration = Get-Content (Join-Path $Repo 'vcpkg-configuration.json') -Raw | ConvertFrom-Json
$VcpkgBaseline = $VcpkgConfiguration.'default-registry'.baseline

# 1.0.4+ updates are signed archive manifests, not a Windows-only raw asset
# build. Keep this script as the Windows builder; the bounded cross-platform
# packager owns the only manifest-writing path.
if (-not $SkipUpdateManifest) {
    throw "release.ps1 只构建 Windows 资产；请使用 scripts\\package-release.ps1 生成并签名归档 manifest。"
}


if (-not $DependencyCacheRoot) {
    $DependencyCacheRoot = Join-Path $Repo "build/dependency-cache/$Profile"
}
$DependencyCacheRoot = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $DependencyCacheRoot
if (-not $VcpkgInstalledDirectory) { $VcpkgInstalledDirectory = Join-Path $DependencyCacheRoot 'installed' }
$InstalledDir = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $VcpkgInstalledDirectory
if (-not $DependencySourceRoot) { $DependencySourceRoot = Join-Path $DependencyCacheRoot 'sources' }
$DependencySourceRoot = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $DependencySourceRoot
if ($CleanDependencies) {
    throw '构建入口不直接删除缓存。请先生成项目限定清理清单，并取得实际删除权限。'
}
$DependencyBinaryCache = Join-Path $DependencyCacheRoot "binary-cache"
$DependencyBuildtrees = Join-Path $DependencyCacheRoot "buildtrees"
$DependencyPackages = Join-Path $DependencyCacheRoot "packages"
New-Item -ItemType Directory -Path $DependencyBinaryCache, $DependencyBuildtrees, $DependencyPackages -Force | Out-Null
$env:VCPKG_DEFAULT_BINARY_CACHE = $DependencyBinaryCache
$GitHead = (git -C $Repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $GitHead -notmatch '^[0-9a-f]{40}$') { throw '无法读取源码 HEAD。' }
$TrackedChanges = @(git -C $Repo status --porcelain=v1 --untracked-files=no)
if ($LASTEXITCODE -ne 0) { throw '无法读取源码状态。' }
$UntrackedSources = @(git -C $Repo ls-files --others --exclude-standard -- src ui cmake tests scripts docs licenses vcpkg-overlays)
if ($LASTEXITCODE -ne 0) { throw '无法核对未跟踪源码。' }
$GitDirty = $TrackedChanges.Count -gt 0 -or $UntrackedSources.Count -gt 0
$GitTag = ''
try { $GitTag = (git -C $Repo describe --tags --exact-match HEAD 2>$null).Trim() } catch {}
if (-not $LocalCandidate -and ($GitDirty -or $GitTag -cne $Version)) {
    throw '正式构建要求干净源码和匹配 VERSION 的 tag。脏工作区必须显式使用 -LocalCandidate。'
}
$BuildMode = if ($LocalCandidate) { 'local-candidate' } else { 'formal' }
$GitCommit = $GitHead + $(if ($GitDirty) { '-dirty' } else { '' })
if ($LocalCandidate -or $GitTag -cne $Version) { $GitTag = '' }
$SourceFingerprint = Get-AwjSourceFingerprint -RepositoryRoot $Repo

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
        $BuildManifestInstalled = Join-Path $InstalledDir "$Triplet/share/$PackageShareName"
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
            "--x-install-root=$InstalledDir" `
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
    $InfoDirs = @((Join-Path $InstalledDir 'vcpkg/info'))
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
        " M src/gainmap.c",
        " M src/read.c",
        " M src/write.c",
        "?? include/avif/awj_gain_map.h"
    )
    "slint"   = @(
        " M Cargo.toml",
        " M Cargo.lock",
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
$ExistingCache = if (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')) {
    Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')
} else { @() }
$CachedHome = @($ExistingCache | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' })
if ($CachedHome.Count -eq 1 -and [IO.Path]::GetFullPath($CachedHome[0].Split('=', 2)[1]) -ne [IO.Path]::GetFullPath($Repo)) {
    throw '该 CMake 缓存属于其他源码目录，请选择全新的 BuildDirectory；不会自动删除旧缓存。'
}
foreach ($FetchContentName in @('libavif', 'jpegli', 'slint')) {
    $SourceDir = Join-Path $BuildDir "_deps/$FetchContentName-src"
    $Override = @($ExistingCache | Where-Object { $_ -match "^FETCHCONTENT_SOURCE_DIR_$($FetchContentName.ToUpperInvariant()):PATH=.+$" })
    if ($Override.Count -eq 1) { $SourceDir = $Override[0].Substring($Override[0].IndexOf('=') + 1) }
    $StableSource = Join-Path $DependencySourceRoot "$FetchContentName-src"
    if (Test-Path -LiteralPath $StableSource) { $SourceDir = $StableSource }
    $SourceDir = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $SourceDir
    if (-not (Test-Path -LiteralPath $SourceDir)) { continue }
    $ExpectedCommit = $PinnedFetchContentCommits[$FetchContentName]
    if (-not $ExpectedCommit -or -not (Test-Path -LiteralPath (Join-Path $SourceDir '.git'))) {
        throw "现有依赖源码缺少可验证 pin：$SourceDir。请指定新缓存路径，不会自动删除。"
    }
    $ResolvedCommit = (& git -C $SourceDir rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $ResolvedCommit -cne $ExpectedCommit) { throw "依赖源码 commit 与锁不符：$SourceDir" }
    $SourceStatus = @(git -C $SourceDir status --porcelain --untracked-files=all)
    if ($LASTEXITCODE -ne 0) { throw "无法验证依赖源码状态：$SourceDir" }
    $ExpectedPatchedFiles = @($PinnedFetchContentPatchedFiles[$FetchContentName])
    $StatusMatches = $SourceStatus.Count -eq 0
    if (-not $StatusMatches -and $ExpectedPatchedFiles.Count -gt 0 -and $SourceStatus.Count -eq $ExpectedPatchedFiles.Count) {
        $StatusMatches = @(Compare-Object ($ExpectedPatchedFiles | Sort-Object) ($SourceStatus | Sort-Object)).Count -eq 0
    }
    if (-not $StatusMatches) { throw "依赖源码含非预期修改，保留不动：$SourceDir" }
    $FetchContentSourceOverrides += "-DFETCHCONTENT_SOURCE_DIR_$($FetchContentName.ToUpperInvariant())=$SourceDir"
}
# 由 CMake 增量更新本次构建树，不清除已有源码、缓存或其他 build。

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
    "-G", $Generator
)
$CompilerDescription = 'Visual Studio 18 2026 toolset'
if ($Generator -eq 'Ninja') {
    if (-not $CompilerDirectory) { throw 'Ninja 正式构建须显式提供 Clang 23 CompilerDirectory。' }
    $Clang = Join-Path ([IO.Path]::GetFullPath($CompilerDirectory, $Repo)) 'clang-cl.exe'
    if (-not (Test-Path -LiteralPath $Clang -PathType Leaf)) { throw 'Clang 编译器不存在。' }
    $CompilerOutput = @(& $Clang --version)
    if ($LASTEXITCODE -ne 0 -or -not $CompilerOutput.Count -or $CompilerOutput[0] -notmatch 'clang version 23\.') {
        throw '应用正式 Ninja 构建要求 Clang 23。'
    }
    $CompilerDescription = $CompilerOutput[0]
    $ConfigureArgs += "-DCMAKE_C_COMPILER=$Clang"
    $ConfigureArgs += "-DCMAKE_CXX_COMPILER=$Clang"
} else {
    if ($CompilerDirectory) { throw '显式 Clang 编译器需搭配 Ninja 生成器。' }
    $ConfigureArgs += @('-A', 'x64')
}
if ($VcpkgRoot) {
    $ConfigureArgs += "-DVCPKG_ROOT=$VcpkgRoot"
    $ConfigureArgs += "-DVCPKG_TRIPLET=$VcpkgTriplet"
    $ConfigureArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
    $ToolchainPath = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    $ConfigureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainPath"
}
$ConfigureArgs += "-DAWJ_VCPKG_LIBRARY_CONFIG=Release"
$ConfigureArgs += "-DVCPKG_OVERLAY_PORTS=$OverlayPorts"
$ConfigureArgs += "-DVCPKG_INSTALLED_DIR=$InstalledDir"
$ConfigureArgs += "-DAWJ_RUNTIME_OUTPUT_ROOT=$RuntimeOutputRoot"
# NoVcpkgInstall 同时约束配置阶段，不能让工具链因缓存 ABI 差异重新安装依赖。
$ConfigureArgs += "-DVCPKG_MANIFEST_INSTALL=$(if ($NoVcpkgInstall) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$DependencyBuildtrees;--x-packages-root=$DependencyPackages"
$ConfigureArgs += "-DBUILD_TESTING=$(if ($BuildTests) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_MSVC_RUNTIME=$(if ($UseStaticRuntime) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_SLINT=$(if ($SharedSlint) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAVIF_ENABLE_RELEASE_IPO=$(if ($EnableLto) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAWJ_ENABLE_X64_V3=ON"
if ($UpdatePublicKeyHex) {
    $ConfigureArgs += "-DAWJ_UPDATE_PUBLIC_KEY_HEX=$UpdatePublicKeyHex"
}
$ConfigureArgs += $FetchContentSourceOverrides


cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败，退出码 $LASTEXITCODE。" }
$ConfiguredRuntime = @(Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') | Where-Object { $_ -match '^AWJ_RUNTIME_OUTPUT_ROOT:PATH=' })
if ($ConfiguredRuntime.Count -ne 1) { throw 'CMake 未记录显式 runtime 输出路径，拒绝构建。' }
$ActualRuntimeRoot = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $ConfiguredRuntime[0].Split('=', 2)[1]
if ($ActualRuntimeRoot -ne $RuntimeOutputRoot) { throw '实际 runtime 路径与本次隔离目录不一致。' }
$OutputDir = Assert-AwjBuildPath -RepositoryRoot $Repo -Path (Join-Path $ActualRuntimeRoot 'Release')
foreach ($Name in $ReleaseFiles) { $null = Resolve-AwjRepoPath -RepositoryRoot $Repo -Path (Join-Path $OutputDir $Name) }
if ((Get-AwjSourceFingerprint -RepositoryRoot $Repo) -cne $SourceFingerprint) { throw '源码在配置期间变化，请稳定后重新构建。' }
$BuildProvenance = Initialize-AwjBuildProvenance -RepositoryRoot $Repo -BuildDirectory $BuildDir -SourceFingerprint $SourceFingerprint -BinaryPaths @((Join-Path $OutputDir 'AWJ.exe'), (Join-Path $OutputDir 'AWJ.com'))
cmake --build $BuildDir --config Release --target AWJ AWJ-com awj_update_manifest_sign --parallel $BuildParallelism
if ($LASTEXITCODE -ne 0) { throw "Release 构建失败，退出码 $LASTEXITCODE。" }
if ((Get-AwjSourceFingerprint -RepositoryRoot $Repo) -cne $SourceFingerprint -or (git -C $Repo rev-parse HEAD).Trim() -cne $GitHead) {
    throw '源码在编译期间变化，不能为该产物写入通过证据。'
}
Complete-AwjBuildProvenance -State $BuildProvenance
$ActualVersion = (& (Join-Path $OutputDir 'AWJ.com') --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $ActualVersion -cne "AWJimage $Version") { throw "构建二进制版本不匹配：$ActualVersion" }

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
Build Mode: $BuildMode
Source Fingerprint: $SourceFingerprint
Git Commit: $GitCommit
$GitTagLine
Architecture: x64
Minimum CPU: AVX2
Release IPO/LTO: $(if ($EnableLto) { 'ON' } else { 'OFF' })
Compiler: $CompilerDescription
CMake Generator: $Generator
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
winit: $($CodecLock.winit.version) ($($CodecLock.winit.commit); project-local DPI backport)

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

$BinaryEvidence = foreach ($Name in @('AWJ.exe', 'AWJ.com')) {
    $Path = Join-Path $OutputDir $Name
    [ordered]@{path=$Name; size=[UInt64](Get-Item -LiteralPath $Path).Length; sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$LockText = (Get-Content -LiteralPath (Join-Path $Repo 'cmake/codec-dependencies.json') -Raw).Replace("`r`n", "`n")
$LockHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($LockText))).ToLowerInvariant()
[ordered]@{
    schema=1; mode=$BuildMode; platform='windows-x64'; version=$Version
    source_fingerprint_algorithm='sha256/git-normalized-source-v1'; source_fingerprint=$SourceFingerprint
    git_head=$GitHead; source_dirty=[bool]$GitDirty; dependency_lock_sha256=$LockHash
    profile=[ordered]@{build_type='Release'; minimum_cpu='AVX2'; ipo=[bool]$EnableLto; static_runtime=[bool]$UseStaticRuntime; triplet=$VcpkgTriplet}
    runtime_output_root=$RuntimeOutputRoot; build_directory=$BuildDir; binaries=@($BinaryEvidence)
    winit=[ordered]@{version=$CodecLock.winit.version; commit=$CodecLock.winit.commit; source_archive_sha256=$CodecLock.winit.sha256; backport_commits=@($CodecLock.winit.backport_commits)}
    verification=[ordered]@{compiled_source_verified=$true; binary_version=$true}
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDir 'build-evidence.json') -Encoding utf8
Write-Host ""
Write-Host "Release 输出:"
Write-Host "  $OutputDir\AWJ.exe"
Write-Host "  $OutputDir\AWJ.com"
