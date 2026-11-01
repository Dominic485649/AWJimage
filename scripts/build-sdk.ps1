param(
    [string]$BuildDirectory = 'build/sdk-native-msvc',
    [string]$CompilerDirectory = '',
    [switch]$CoreOnly,
    [int]$Parallelism = 2,
    [switch]$EnableLto = $true,
    [string[]]$ExtraCMakeArguments = @(),
    [string[]]$ExtraCTestArguments = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Get-ChildItem -LiteralPath $root -Name | Out-Null
Import-Module (Join-Path $PSScriptRoot 'release-safety.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'windows-msvc.psm1') -Force
if (-not $EnableLto) { throw 'Windows SDK 交付必须保留 Release IPO/LTO。' }
$build = Assert-AwjBuildPath -RepositoryRoot $root -Path $BuildDirectory
$compilerRoot = if ($CompilerDirectory) { [IO.Path]::GetFullPath($CompilerDirectory, $root) } else { '' }
$msvc = Initialize-AwjMsvcToolchain -CompilerDirectory $compilerRoot
$temporary = Join-Path $root 'build/sdk-temporary-msvc'
New-Item -ItemType Directory -Force -Path $temporary | Out-Null
$env:TEMP = $temporary
$env:TMP = $temporary
$arguments = @('-S', (Join-Path $root 'sdk'), '-B', $build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    "-DCMAKE_C_COMPILER=$($msvc.compiler)", "-DCMAKE_CXX_COMPILER=$($msvc.compiler)",
    "-DCMAKE_LINKER=$($msvc.linker)", "-DCMAKE_AR=$($msvc.archive)",
    "-DCMAKE_RC_COMPILER=$($msvc.resource)", "-DCMAKE_RC_COMPILER_INIT=$($msvc.resource)",
    '-DBUILD_TESTING=ON', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DAVIF_ENABLE_RELEASE_IPO=ON', "-DAWJ_SDK_NATIVE_CODECS=$(-not $CoreOnly)")
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
$profile = Get-AwjMsvcBuildEvidence -BuildDirectory $build -Targets @('awj_sdk') -Toolchain $msvc -MinimumCpu x64
& cmake --build $build --target awj_sdk_validation --parallel $Parallelism
if ($LASTEXITCODE) { throw 'SDK 构建失败。' }
& ctest --test-dir $build --output-on-failure @ExtraCTestArguments
if ($LASTEXITCODE) { throw 'SDK 定向测试失败。' }
$profile = Get-AwjMsvcBuildEvidence -BuildDirectory $build -Targets @('awj_sdk') -Toolchain $msvc -MinimumCpu x64
$profile | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $build 'msvc-build-profile.json') -Encoding utf8
