param(
    [string]$BuildDirectory = 'build/sdk-native-clang21',
    [string]$CompilerDirectory = 'build/sdk-tools/clang21-extracted/bin',
    [switch]$CoreOnly,
    [int]$Parallelism = 2,
    [string[]]$ExtraCMakeArguments = @(),
    [string[]]$ExtraCTestArguments = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Get-ChildItem -LiteralPath $root -Name | Out-Null
$build = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
if (-not $build.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'SDK 构建目录必须在当前工作树内。'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $vs) { throw '没有找到 Windows C++ 工具链。' }
$vcvars = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$env:PATH = (Split-Path $vswhere -Parent) + ';' + $env:PATH
# 环境只导入当前构建进程，不修改用户/系统 PATH 或全局工具链。
$environment = & $env:ComSpec /d /c "call `"$vcvars`" -no_logo -arch=amd64 -host_arch=amd64 >nul && set"
if ($LASTEXITCODE) { throw '初始化 Windows C++ 构建环境失败。' }
foreach ($line in $environment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0,$separator), $line.Substring($separator+1), 'Process') }
}
$temporary = Join-Path $root 'build/sdk-temporary'
Get-ChildItem -LiteralPath $root -Name | Out-Null
New-Item -ItemType Directory -Force -Path $temporary | Out-Null
$env:TEMP = $temporary
$env:TMP = $temporary
$compilerRoot = [IO.Path]::GetFullPath($CompilerDirectory, $root)
$clang = Join-Path $compilerRoot 'clang-cl.exe'
$arguments = @('-S', (Join-Path $root 'sdk'), '-B', $build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_C_COMPILER=$clang", "-DCMAKE_CXX_COMPILER=$clang",
    '-DBUILD_TESTING=ON', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    "-DAWJ_SDK_NATIVE_CODECS=$(-not $CoreOnly)")
if (-not $CoreOnly) {
    $dependencies = Join-Path $root 'build/sdk-deps'
    Get-ChildItem -LiteralPath (Join-Path $root 'build') -Name | Out-Null
    foreach ($directory in @('downloads','binary-cache','registries','buildtrees','packages')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $dependencies $directory) | Out-Null
    }
    $env:VCPKG_DOWNLOADS = Join-Path $dependencies 'downloads'
    $env:VCPKG_REGISTRIES_CACHE = Join-Path $dependencies 'registries'
    $env:VCPKG_BINARY_SOURCES = "clear;files,$dependencies/binary-cache,readwrite"
    $env:VCPKG_MAX_CONCURRENCY = "$Parallelism"
    $arguments += @('-DCMAKE_TOOLCHAIN_FILE=D:/Scoop/apps/vcpkg/current/scripts/buildsystems/vcpkg.cmake',
        '-DVCPKG_ROOT=D:/Scoop/apps/vcpkg/current', '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        "-DVCPKG_MANIFEST_DIR=$root/sdk", "-DVCPKG_INSTALLED_DIR=$dependencies/installed",
        "-DVCPKG_OVERLAY_PORTS=$root/vcpkg-overlays",
        "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$dependencies/buildtrees;--x-packages-root=$dependencies/packages",
        "-DAWJ_RUNTIME_OUTPUT_ROOT=$build/runtime", '-DAWJ_ENABLE_X64_V3=OFF')
}
$arguments += $ExtraCMakeArguments
& cmake @arguments
if ($LASTEXITCODE) { throw 'SDK 配置失败。' }
& cmake --build $build --target awj_sdk_validation --parallel $Parallelism
if ($LASTEXITCODE) { throw 'SDK 构建失败。' }
& ctest --test-dir $build --output-on-failure @ExtraCTestArguments
if ($LASTEXITCODE) { throw 'SDK 定向测试失败。' }
