param(
    [string]$BuildDirectory = 'build/sdk-native-clang21',
    [string]$CompilerDirectory = 'build/sdk-tools/clang21-extracted/bin',
    [string]$FfmpegDirectory = 'D:/DevTools/Cli/FFmpeg',
    [string]$RunId = ('windows-x64-clang21-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($RunId -notmatch '^[A-Za-z0-9_-]+$') { throw '验证标识只允许 ASCII 字母、数字、下划线和连字符。' }
$build = [IO.Path]::GetFullPath($BuildDirectory, $root)
$compiler = [IO.Path]::GetFullPath($CompilerDirectory, $root)
$evidence = Join-Path $root "build/sdk-evidence/$RunId"
Get-ChildItem -LiteralPath (Join-Path $root 'build') -Name | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $evidence -Parent) | Out-Null
if (Test-Path -LiteralPath $evidence) { throw '拒绝覆盖现有验证证据。' }
New-Item -ItemType Directory -Path $evidence | Out-Null
$sourceModule = Import-Module (Join-Path $PSScriptRoot 'release-safety.psm1') -Force -PassThru -DisableNameChecking
$before = Get-AwjSourceFingerprint -RepositoryRoot $root
$head = (& git -C $root rev-parse HEAD).Trim()
$branch = (& git -C $root branch --show-current).Trim()
if ($LASTEXITCODE -or $branch -ne 'feature/1.3.0-memory-pipeline') { throw '本验证入口仅用于隔离的 1.3.0 开发分支。' }
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
if ($version -ne '1.3.0') { throw '版本不是预期的 1.3.0。' }
$fixed126 = 'f77b5f0fa15a15f5047d64cb0c0bd98be95dbaa4'
& git -C $root merge-base --is-ancestor $fixed126 HEAD
$merged126 = $LASTEXITCODE -eq 0
if (-not $merged126) { throw '固定的 1.2.6 尚未合入当前源码，拒绝生成整合后的验证记录。' }
$sourceDirty = [bool](& git -C $root status --porcelain --untracked-files=normal)
# 记录逐文件真实字节哈希，与规范化 Git blob 的汇总指纹区分。
$paths = & $sourceModule {
    param($Repository)
    $listing = Invoke-AwjGit -RepositoryRoot $Repository -Arguments @('ls-files','--cached','--others','--exclude-standard','-z')
    foreach ($path in $listing.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
        if (Test-AwjSourcePath $path) { $path }
    }
} $root
$files = foreach ($relative in ($paths | Sort-Object -Unique)) {
    $path = Resolve-AwjRepoPath -RepositoryRoot $root -Path $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        [ordered]@{path=$relative; size=(Get-Item -LiteralPath $path).Length; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
}
$files | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence 'source-files.json') -Encoding utf8
& (Join-Path $PSScriptRoot 'build-sdk.ps1') -BuildDirectory $BuildDirectory -CompilerDirectory $compiler -Parallelism 2 `
    -ExtraCMakeArguments @('-DAWJ_SDK_D3D11=ON','-DAWJ_SDK_SHM=ON','-DAWJ_SDK_FFMPEG=ON','-DAWJ_SDK_TOOLS=ON','-DAWJ_ENABLE_GPU_METRICS=OFF',
        "-DAWJ_SDK_AVUTIL_INCLUDE_DIR=$root/build/sdk-tools/ffmpeg-headers", "-DAWJ_SDK_FFMPEG_TEST_AVUTIL=$FfmpegDirectory/avutil-61.dll") `
    -ExtraCTestArguments @('--output-junit', (Join-Path $evidence 'sdk-tests.xml')) *>&1 |
    Tee-Object -FilePath (Join-Path $evidence 'build-and-tests.log')
if ((Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $before) { throw '构建期间源码变化，拒绝绑定证据。' }
$install = Join-Path $evidence 'install'
& cmake --install $build --prefix $install --component AWJimageSDK *>&1 | Tee-Object -FilePath (Join-Path $evidence 'install.log')
if ($LASTEXITCODE) { throw 'SDK 安装失败。' }
& (Join-Path $PSScriptRoot 'verify-sdk-exports.ps1') -Library (Join-Path $install 'bin/awj_sdk.dll') -IncludeDirectory (Join-Path $install 'include') `
    -ReadObject (Join-Path $compiler 'llvm-readobj.exe') | Tee-Object -FilePath (Join-Path $evidence 'exports.log')
$consumer = Join-Path $evidence 'consumers'
& cmake -S (Join-Path $install 'share/AWJimage/examples/basic') -B $consumer -G Ninja '-DCMAKE_BUILD_TYPE=Release' `
    '-DCMAKE_C_COMPILER=cl.exe' '-DCMAKE_CXX_COMPILER=cl.exe' "-DCMAKE_PREFIX_PATH=$install" *>&1 |
    Tee-Object -FilePath (Join-Path $evidence 'consumer-configure.log')
if ($LASTEXITCODE) { throw '独立 MSVC 消费者配置失败。' }
& cmake --build $consumer --parallel 2 *>&1 | Tee-Object -FilePath (Join-Path $evidence 'consumer-build.log')
if ($LASTEXITCODE) { throw '独立消费者构建失败。' }
& ctest --test-dir $consumer --output-on-failure --output-junit (Join-Path $evidence 'consumer-tests.xml') *>&1 |
    Tee-Object -FilePath (Join-Path $evidence 'consumer-tests.log')
if ($LASTEXITCODE) { throw '独立消费者运行失败。' }
$consumerClang = Join-Path $evidence 'consumers-clang23'
$clang23 = 'D:/Scoop/apps/llvm/current/bin/clang-cl.exe'
& $clang23 --version | Set-Content -LiteralPath (Join-Path $evidence 'consumer-clang-version.txt') -Encoding utf8
if ($LASTEXITCODE) { throw '无法核对 Clang 消费者工具链。' }
& cmake -S (Join-Path $install 'share/AWJimage/examples/basic') -B $consumerClang -G Ninja '-DCMAKE_BUILD_TYPE=Release' `
    "-DCMAKE_C_COMPILER=$clang23" "-DCMAKE_CXX_COMPILER=$clang23" "-DCMAKE_PREFIX_PATH=$install" *>&1 |
    Tee-Object -FilePath (Join-Path $evidence 'consumer-clang-configure.log')
if ($LASTEXITCODE) { throw '独立 Clang 消费者配置失败。' }
& cmake --build $consumerClang --parallel 2 *>&1 | Tee-Object -FilePath (Join-Path $evidence 'consumer-clang-build.log')
if ($LASTEXITCODE) { throw '独立 Clang 消费者构建失败。' }
& ctest --test-dir $consumerClang --output-on-failure --output-junit (Join-Path $evidence 'consumer-clang-tests.xml') *>&1 |
    Tee-Object -FilePath (Join-Path $evidence 'consumer-clang-tests.log')
if ($LASTEXITCODE) { throw '独立 Clang 消费者运行失败。' }
& (Join-Path $root 'tests/sdk_stream_cli_tests.ps1') -StreamCli (Join-Path $install 'bin/awj-stream.exe') `
    -Ffmpeg (Join-Path $FfmpegDirectory 'ffmpeg.exe') -WgcCli (Join-Path $build 'runtime/Release/awj_wgc_cli_validation.exe') `
    -WorkDirectory (Join-Path $evidence 'stream-work') *>&1 | Tee-Object -FilePath (Join-Path $evidence 'stream-tests.log')
if ((Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $before) { throw '验证期间源码变化，拒绝交付不匹配的源码。' }
$configuration = Join-Path $evidence 'configuration'
New-Item -ItemType Directory -Path $configuration | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'CMakeCache.txt') -Destination $configuration
Copy-Item -LiteralPath (Join-Path $root 'cmake/codec-dependencies.json') -Destination $configuration
Copy-Item -LiteralPath (Join-Path $root 'sdk/vcpkg.json') -Destination (Join-Path $configuration 'sdk-vcpkg.json')
Copy-Item -LiteralPath (Join-Path $root 'build/sdk-deps/installed/vcpkg/status') -Destination (Join-Path $configuration 'vcpkg-status.txt')
$imports = & (Join-Path $compiler 'llvm-readobj.exe') --coff-imports (Join-Path $install 'bin/awj_sdk.dll')
if ($LASTEXITCODE) { throw '无法核对 DLL 依赖。' }
$imports | Set-Content -LiteralPath (Join-Path $configuration 'dll-imports.txt') -Encoding utf8
if (($imports -join "`n") -match '(?i)Name: (avutil|avcodec|avformat|libc\+\+)') { throw 'SDK 不应硬链接 FFmpeg 或其 C++ 运行库。' }
$toolchain = & (Join-Path $compiler 'clang-cl.exe') --version
$toolchain | Set-Content -LiteralPath (Join-Path $configuration 'clang-version.txt') -Encoding utf8
[xml]$tests = [IO.File]::ReadAllText((Join-Path $evidence 'sdk-tests.xml'))
$summary = [ordered]@{
    schema=1; kind='unreleased-sdk-development-validation'; version=$version; abi_version=1;
    run_id=$RunId; source_root=$root; source_fingerprint=$before; baseline_commit=$head; branch=$branch;
    uncommitted_source=$sourceDirty; platform='Windows x64'; compiler='clang-cl 21.1.8 / MSVC ABI'; configuration='Release; /MT; GPU metrics OFF; x64-v3 OFF';
    sdk_tests=$tests.SelectNodes('//testcase').Count; skipped_tests=$tests.SelectNodes('//testcase[skipped]').Count;
    consumer_tests=4; consumers='独立 install 包，MSVC 与 Clang 23 的 C99 / C++11 源码消费同一 C ABI';
    ffmpeg_runtime='仅用于实际集成测试，不分发；libavutil 61.18.100';
    not_verified=@('真实 WGC/物理 HDR GPU/跨设备共享','真实 FFmpeg D3D11 解码会话','完整 GUI/批处理应用与 GPU visual metrics：本脚本不覆盖','HarmonyOS Native SDK/真机','Linux：使用独立原生验证记录，本 Windows 脚本不覆盖','系统级全进程文件 I/O 跟踪');
    release=$false; merged_126=$merged126; integrated_126_commit=$fixed126; main_renamed=$false
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'validation.json') -Encoding utf8
# 完整的项目源快照不包含 build/bin、私钥、Git 元数据或 FFmpeg 用户运行库。
$snapshot = Join-Path $evidence 'source'
New-Item -ItemType Directory -Path $snapshot | Out-Null
foreach ($file in $files) {
    $origin = Resolve-AwjRepoPath -RepositoryRoot $root -Path $file.path
    if ((Get-FileHash -LiteralPath $origin -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw '源文件在快照复制前变化。' }
    $destination = Join-Path $snapshot $file.path
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $origin -Destination $destination
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw '源码快照字节核验失败。' }
}
$archive = Join-Path $evidence 'AWJimage-1.3.0-development-source.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($snapshot, $archive, [IO.Compression.CompressionLevel]::Optimal, $false)
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $entries = @($zip.Entries | Where-Object { $_.Name })
    if ($entries.Count -ne @($files).Count) { throw '源码归档文件数量不匹配。' }
    foreach ($file in $files) {
        $entry = $zip.GetEntry($file.path.Replace('\','/'))
        if (-not $entry -or $entry.Length -ne $file.size) { throw "源码归档缺失：$($file.path)" }
        $stream = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)).ToLowerInvariant() }
        finally { $sha.Dispose(); $stream.Dispose() }
        if ($hash -cne $file.sha256) { throw '源码归档哈希不匹配。' }
    }
} finally { $zip.Dispose() }
# 删除前已逐条比对源目录/归档字节；源快照是本运行创建的重复副本，不是工作源码。
Remove-Item -LiteralPath $snapshot -Recurse -Force -Confirm:$false
"定向验证、安装消费者、真实流集成、导出白名单及源码归档完成：$evidence"
