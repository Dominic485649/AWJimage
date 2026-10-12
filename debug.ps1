param(
    [string]$VcpkgRoot = "",
    [string]$VcpkgTriplet = "",
    [switch]$StaticRuntime,
    [switch]$DynamicRuntime,
    [switch]$SharedSlint,
    [switch]$NoVcpkgInstall,
    [switch]$DisableZenravif,
    [switch]$DisableSvtAv1Hdr
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSCommandPath
$BuildDir = Join-Path $Repo "build\x64\Debug"

if (-not $VcpkgRoot) {
    if ($env:VCPKG_ROOT) {
        $VcpkgRoot = $env:VCPKG_ROOT
    }
}

function Ensure-VcpkgPackage([string]$Root, [string]$Triplet, [string]$Port, [string]$PackageShareName, [bool]$NoInstall) {
    if (-not $Root) {
        throw "未找到 vcpkg。请设置 VCPKG_ROOT，或传入 -VcpkgRoot。"
    }

    $ManifestInstalled = Join-Path $BuildDir "vcpkg_installed\$Triplet\share\$PackageShareName"
    $GlobalInstalled = Join-Path $Root "installed\$Triplet\share\$PackageShareName"
    if ((Test-Path -LiteralPath $ManifestInstalled -PathType Container) -or
        (Test-Path -LiteralPath $GlobalInstalled -PathType Container)) {
        return
    }

    if ($NoInstall) {
        throw "未安装 $Port`:$Triplet。请先运行: `"$Root\vcpkg.exe`" install $Port`:$Triplet"
    }

    $VcpkgExe = Join-Path $Root "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $VcpkgExe -PathType Leaf)) {
        throw "未找到 vcpkg.exe: $VcpkgExe"
    }

    Write-Host "未发现 $Port`:$Triplet，开始使用 vcpkg 安装..."
    & $VcpkgExe install "$Port`:$Triplet"
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg install $Port`:$Triplet 失败，退出码 $LASTEXITCODE。"
    }
}

if ($StaticRuntime -and $DynamicRuntime) {
    throw "不能同时指定 -StaticRuntime 和 -DynamicRuntime。"
}

$UseStaticRuntime = -not [bool]$DynamicRuntime
if ($StaticRuntime) {
    $UseStaticRuntime = $true
}

if (-not $VcpkgTriplet) {
    $VcpkgTriplet = if ($UseStaticRuntime) { "x64-windows-static" } else { "x64-windows" }
}

Ensure-VcpkgPackage $VcpkgRoot $VcpkgTriplet "scnlib" "scnlib" $NoVcpkgInstall
Ensure-VcpkgPackage $VcpkgRoot $VcpkgTriplet "libwebp" "libwebp" $NoVcpkgInstall
Ensure-VcpkgPackage $VcpkgRoot $VcpkgTriplet "libjxl" "libjxl" $NoVcpkgInstall
Ensure-VcpkgPackage $VcpkgRoot $VcpkgTriplet "aom" "aom" $NoVcpkgInstall
Ensure-VcpkgPackage $VcpkgRoot $VcpkgTriplet "libyuv" "libyuv" $NoVcpkgInstall

$ConfigureArgs = @(
    "-U", "scn_DIR",
    "-U", "FastFloat_DIR",
    "-U", "fast_float_DIR",
    "-S", $Repo,
    "-B", $BuildDir,
    "-G", "Visual Studio 18 2026",
    "-A", "x64"
)
if ($VcpkgRoot) {
    $ConfigureArgs += "-DVCPKG_ROOT=$VcpkgRoot"
    $ConfigureArgs += "-DVCPKG_TRIPLET=$VcpkgTriplet"
    $ToolchainPath = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    $ConfigureArgs += "-DCMAKE_TOOLCHAIN_FILE=$ToolchainPath"
}
$ConfigureArgs += "-DAVIF_STATIC_MSVC_RUNTIME=$(if ($UseStaticRuntime) { 'ON' } else { 'OFF' })"
$ConfigureArgs += "-DAVIF_STATIC_SLINT=$(if ($SharedSlint) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAWJ_ENABLE_ZENRAVIF=$(if ($DisableZenravif) { 'OFF' } else { 'ON' })"
$ConfigureArgs += "-DAWJ_ENABLE_SVTAV1HDR=$(if ($DisableSvtAv1Hdr) { 'OFF' } else { 'ON' })"

cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码 $LASTEXITCODE。"
}
cmake --build $BuildDir --config Debug --target AWJ AWJ-com --parallel

if ($LASTEXITCODE -ne 0) {
    throw "Debug 构建失败，退出码 $LASTEXITCODE。"
}

$OutputDir = Join-Path $Repo "bin\x64\Debug"
$Keep = @("AWJ.exe", "AWJ.com", "AWJ.pdb", "AWJ-com.pdb", "AWJ.exe.sha256", "AWJ.com.sha256", "slint_cpp.dll")
if (Test-Path $OutputDir) {
    Get-ChildItem -LiteralPath $OutputDir -Force | Where-Object { $Keep -notcontains $_.Name } | Remove-Item -Recurse -Force
    if (-not $SharedSlint) {
        foreach ($Name in @("slint_cpp.dll", "slint-compiler.exe", "slint_compiler.pdb")) {
            $Path = Join-Path $OutputDir $Name
            if (Test-Path $Path) { Remove-Item -LiteralPath $Path -Force }
        }
    }
}

Write-Host ""
Write-Host "Debug 输出:"
Write-Host "  $OutputDir\AWJ.exe"
Write-Host "  $OutputDir\AWJ.com"
Write-Host "  svt-av1-hdr: 已静态集成到主程序。"
