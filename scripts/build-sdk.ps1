param(
    [string]$BuildDirectory = 'build/sdk-native-msvc',
    [string]$CompilerDirectory = '',
    [switch]$CoreOnly,
    [int]$Parallelism = 2,
    [switch]$EnableLto = $true,
    [string[]]$ExtraCMakeArguments = @(),
    [string[]]$ExtraCTestArguments = @(),
    [string]$CMakePath = 'cmake',
    [string]$CTestPath = 'ctest',
    [string]$NinjaPath = 'ninja',
    [string]$VcpkgDirectory = 'D:/Scoop/apps/vcpkg/current',
    [string]$FFmpegIncludeDirectory = '',
    [string]$FFmpegTestLibraryPath = '',
    [string]$TestReportPath = '',
    [string]$DependencyDirectory = 'build/sdk-deps',
    [string]$DependencyDownloadsDirectory = '',
    [string]$DependencyBinaryCacheDirectory = '',
    [switch]$VerboseBuild,
    [string]$PreTestSnapshotPath = '',
    [scriptblock]$PreTestValidator = $null
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

function Write-AwjSdkPreTestSnapshot {
    param([string]$RepositoryRoot, [string]$Build, [string]$Path, $Toolchain)
    $snapshot = Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path $Path
    if (Test-Path -LiteralPath $snapshot) { throw '测试前快照必须全新。' }
    $runtime = Join-Path $Build 'runtime/Release'
    $names = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    foreach ($name in @('CMakeCache.txt','compile_commands.json','.ninja_log','build.ninja','CMakeFiles/rules.ninja',
            'awj-msvc-awj_sdk-Release.txt','sdk-validation-targets.dot','sdk-validation-commands.txt','sdk-ctest-plan.json',
            'CTestTestfile.cmake','native/CTestTestfile.cmake')) { [void]$names.Add($name) }
    foreach ($item in Get-ChildItem -LiteralPath $runtime -Force) {
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '测试前 runtime 必须只包含普通文件。'
        }
        [void]$names.Add("runtime/Release/$($item.Name)")
    }
    if ($names.Count -gt 128) { throw '测试前 SDK 输出成员过多。' }
    $files = @(
        foreach ($name in $names) {
            $safe = Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $Build $name)
            $item = Get-Item -LiteralPath $safe -Force
            if ($item.PSIsContainer) { throw '测试前来源记录必须是普通文件。' }
            [ordered]@{path=$name;size=[UInt64]$item.Length;sha256=(Get-FileHash -LiteralPath $safe -Algorithm SHA256).Hash.ToLowerInvariant()}
        }
    )
    $head = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $head -cnotmatch '^[0-9a-f]{40}$') { throw '测试前无法绑定真实源码 HEAD。' }
    $profile = Get-AwjMsvcBuildEvidence -BuildDirectory $Build -Targets @('awj_sdk') -Toolchain $Toolchain -MinimumCpu x64
    [ordered]@{schema=1;kind='windows-sdk-pre-test-snapshot';source_root=$RepositoryRoot;build_directory=$Build;git_head=$head
        source_fingerprint=(Get-AwjSourceFingerprint -RepositoryRoot $RepositoryRoot);created_at=[DateTimeOffset]::UtcNow.ToString('o')
        files=$files;toolchain=$profile} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $snapshot -Encoding utf8
}

$root = Split-Path $PSScriptRoot -Parent
Get-ChildItem -LiteralPath $root -Name | Out-Null
Import-Module (Join-Path $PSScriptRoot 'release-safety.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'windows-msvc.psm1') -Force
if (-not $EnableLto) { throw 'Windows SDK 交付必须保留 Release IPO/LTO。' }
if ($PreTestValidator -and -not $PreTestSnapshotPath) { throw '测试前校验器必须绑定真实快照，不能单独运行。' }
$build = Assert-AwjBuildPath -RepositoryRoot $root -Path $BuildDirectory
$compilerRoot = if ($CompilerDirectory) { [IO.Path]::GetFullPath($CompilerDirectory, $root) } else { '' }
$msvc = Initialize-AwjMsvcToolchain -CompilerDirectory $compilerRoot
$temporary = Join-Path $root 'build/sdk-temporary-msvc'
New-Item -ItemType Directory -Force -Path $temporary | Out-Null
$env:TEMP = $temporary
$env:TMP = $temporary
$arguments = @('-S', (Join-Path $root 'sdk'), '-B', $build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', "-DCMAKE_MAKE_PROGRAM=$NinjaPath",
    "-DCMAKE_C_COMPILER=$($msvc.compiler)", "-DCMAKE_CXX_COMPILER=$($msvc.compiler)",
    "-DCMAKE_LINKER=$($msvc.linker)", "-DCMAKE_AR=$($msvc.archive)",
    "-DCMAKE_RC_COMPILER=$($msvc.resource)", "-DCMAKE_RC_COMPILER_INIT=$($msvc.resource)",
    '-DBUILD_TESTING=ON', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DAVIF_ENABLE_RELEASE_IPO=ON', "-DAWJ_SDK_NATIVE_CODECS=$(-not $CoreOnly)")
if (-not $CoreOnly) {
    $dependencies = Assert-AwjBuildPath -RepositoryRoot $root -Path $DependencyDirectory
    Get-ChildItem -LiteralPath (Join-Path $root 'build') -Name | Out-Null
    foreach ($directory in @('downloads','binary-cache','registries','buildtrees','packages')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $dependencies $directory) | Out-Null
    }
    # 只复用下载和内容寻址二进制缓存；安装树、构建树与补丁化源码仍属于当前仓库。
    $downloads = if ($DependencyDownloadsDirectory) { [IO.Path]::GetFullPath($DependencyDownloadsDirectory, $root) } else { Join-Path $dependencies 'downloads' }
    $binaryCache = if ($DependencyBinaryCacheDirectory) { [IO.Path]::GetFullPath($DependencyBinaryCacheDirectory, $root) } else { Join-Path $dependencies 'binary-cache' }
    foreach ($path in @($downloads, $binaryCache)) { New-Item -ItemType Directory -Force -Path $path | Out-Null }
    $env:VCPKG_DOWNLOADS = $downloads
    $env:VCPKG_REGISTRIES_CACHE = Join-Path $dependencies 'registries'
    $cacheMode = if ($DependencyBinaryCacheDirectory) { 'read' } else { 'readwrite' }
    $env:VCPKG_BINARY_SOURCES = "clear;files,$($binaryCache.Replace('\','/')),$cacheMode"
    $env:VCPKG_MAX_CONCURRENCY = "$Parallelism"
    $vcpkg = [IO.Path]::GetFullPath($VcpkgDirectory, $root).Replace('\','/')
    if (-not (Test-Path -LiteralPath "$vcpkg/scripts/buildsystems/vcpkg.cmake" -PathType Leaf)) { throw '找不到显式指定的 vcpkg 工具链。' }
    $arguments += @("-DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake",
        "-DVCPKG_ROOT=$vcpkg", '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
        "-DVCPKG_MANIFEST_DIR=$root/sdk", "-DVCPKG_INSTALLED_DIR=$dependencies/installed",
        "-DVCPKG_OVERLAY_PORTS=$root/vcpkg-overlays",
        "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$dependencies/buildtrees;--x-packages-root=$dependencies/packages",
        "-DAWJ_RUNTIME_OUTPUT_ROOT=$build/runtime", '-DAWJ_ENABLE_X64_V3=OFF')
}
if ($FFmpegIncludeDirectory) {
    $headers = [IO.Path]::GetFullPath($FFmpegIncludeDirectory, $root).Replace('\','/')
    if (-not (Test-Path -LiteralPath (Join-Path $headers 'libavutil/frame.h') -PathType Leaf)) { throw 'FFmpeg 头目录缺少 libavutil/frame.h。' }
    $arguments += "-DAWJ_SDK_AVUTIL_INCLUDE_DIR=$headers"
}
if ($FFmpegTestLibraryPath) {
    $testLibrary = [IO.Path]::GetFullPath($FFmpegTestLibraryPath, $root).Replace('\','/')
    if (-not (Test-Path -LiteralPath $testLibrary -PathType Leaf)) { throw 'FFmpeg 测试动态库不存在。' }
    $arguments += "-DAWJ_SDK_FFMPEG_TEST_AVUTIL=$testLibrary"
}
$testArguments = @($ExtraCTestArguments)
if ($TestReportPath) {
    $testReport = Assert-AwjBuildPath -RepositoryRoot $root -Path $TestReportPath
    if (Test-Path -LiteralPath $testReport) { throw 'SDK JUnit 必须由本轮测试生成，不能覆盖已有报告。' }
    New-Item -ItemType Directory -Force -Path (Split-Path $testReport -Parent) | Out-Null
    $testArguments += @('--output-junit', $testReport)
}
$arguments += $ExtraCMakeArguments
& $CMakePath @arguments
if ($LASTEXITCODE) { throw 'SDK 配置失败。' }
$profile = Get-AwjMsvcBuildEvidence -BuildDirectory $build -Targets @('awj_sdk') -Toolchain $msvc -MinimumCpu x64
$buildArguments = @('--build', $build, '--target', 'awj_sdk_validation', '--parallel', "$Parallelism")
if ($VerboseBuild) { $buildArguments += '--verbose' }
& $CMakePath @buildArguments
if ($LASTEXITCODE) { throw 'SDK 构建失败。' }
if ($PreTestSnapshotPath) {
    # 在任何测试开始前采集真正 validation 依赖图、生成命令、CTest 执行计划和输出字节。
    # 正式窄编排必须消费并复核此快照；开发入口不据此自行声明 formal。
    if ($CoreOnly) { throw '测试前来源快照仅适用于完整原生 SDK。' }
    $snapshot = Assert-AwjBuildPath -RepositoryRoot $root -Path $PreTestSnapshotPath
    if (Test-Path -LiteralPath $snapshot) { throw '测试前快照必须全新。' }
    [void][IO.Directory]::CreateDirectory((Split-Path $snapshot -Parent))
    $graph = & $NinjaPath -C $build -t graph awj_sdk_validation
    if ($LASTEXITCODE -ne 0) { throw '无法采集实际 awj_sdk_validation 依赖图。' }
    [IO.File]::WriteAllLines((Join-Path $build 'sdk-validation-targets.dot'), [string[]]$graph, [Text.UTF8Encoding]::new($false))
    $commands = & $NinjaPath -C $build -t commands awj_sdk_validation
    if ($LASTEXITCODE -ne 0) { throw '无法采集实际 awj_sdk_validation 生成命令。' }
    [IO.File]::WriteAllLines((Join-Path $build 'sdk-validation-commands.txt'), [string[]]$commands, [Text.UTF8Encoding]::new($false))
    $plan = & $CTestPath --test-dir $build --show-only=json-v1 @ExtraCTestArguments
    if ($LASTEXITCODE -ne 0) { throw '无法采集实际 SDK CTest 执行计划。' }
    [IO.File]::WriteAllLines((Join-Path $build 'sdk-ctest-plan.json'), [string[]]$plan, [Text.UTF8Encoding]::new($false))
    Write-AwjSdkPreTestSnapshot -RepositoryRoot $root -Build $build -Path $snapshot -Toolchain $msvc
    # 只接受调用方直接传入的可信脚本块，不从证据 JSON 读取路径或执行命令。
    if ($PreTestValidator) { & $PreTestValidator $snapshot $build $msvc | Out-Null }
}
& $CTestPath --test-dir $build --output-on-failure @testArguments
if ($LASTEXITCODE) { throw 'SDK 定向测试失败。' }
$profile = Get-AwjMsvcBuildEvidence -BuildDirectory $build -Targets @('awj_sdk') -Toolchain $msvc -MinimumCpu x64
$profile | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $build 'msvc-build-profile.json') -Encoding utf8
