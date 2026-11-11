[CmdletBinding()]
param(
    [string]$SourceRoot = '',
    [string]$WorkRoot = '',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$CompilerDirectory = '',
    [Parameter(Mandatory)][string]$FFmpegIncludeDirectory,
    [Parameter(Mandatory)][string]$FFmpegTestLibraryPath,
    [string]$VcpkgDirectory = 'D:/Scoop/apps/vcpkg/current',
    [string]$DependencyDownloadsDirectory = '',
    [string]$DependencyBinaryCacheDirectory = '',
    [string]$CMakePath = 'cmake',
    [string]$CTestPath = '',
    [string]$NinjaPath = 'ninja',
    [ValidateRange(1,256)][int]$Parallelism = 2
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $PSScriptRoot 'release-safety.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $PSScriptRoot 'windows-msvc.psm1') -Force

function ConvertTo-SdkReleaseVersion([string]$Value) {
    if ($Value -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'SDK VERSION 必须为规范三段版本，各段范围为 0..2147483647。' }
    foreach ($part in $Value.Split('.')) {
        $number = 0
        if (-not [Int32]::TryParse($part, [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) { throw 'SDK VERSION 必须为规范三段版本，各段范围为 0..2147483647。' }
    }
    return [version]$Value
}

function Assert-SdkReleaseRoots([string]$ScriptRepository, [string]$RequestedSource, [string]$RequestedWork) {
    $repository = [IO.Path]::GetFullPath($ScriptRepository).TrimEnd('\','/')
    $source = (Resolve-AwjRepoPath -RepositoryRoot $ScriptRepository -Path $RequestedSource -AllowRepositoryRoot).TrimEnd('\','/')
    if (-not $source.Equals($repository, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'SourceRoot 必须是当前脚本自身的仓库；不能从旧目录运行脚本而仅把输出指向洁净 clone。'
    }
    $work = [IO.Path]::GetFullPath($RequestedWork, $source).TrimEnd('\','/')
    if (-not $work.Equals($source.TrimEnd('\','/'), [StringComparison]::OrdinalIgnoreCase) -and
        -not (Join-Path $work 'src').Equals($source.TrimEnd('\','/'), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'WorkRoot 必须是 SourceRoot，或恰好以 WorkRoot/src 为 SourceRoot。'
    }
    return $work
}

function Read-SdkReleaseGit([string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new('git')
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in (@('-C', $root) + $Arguments)) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw '无法启动 SDK Git 来源检查。' }
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $text = $stdout.GetAwaiter().GetResult(); $errorText = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "SDK Git 来源检查失败：$errorText" }
        return $text
    } finally { $process.Dispose() }
}

function Get-SdkReleaseFile([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "SDK 输入不是普通文件：$Path" }
    return [ordered]@{path=$item.FullName;size=[UInt64]$item.Length;sha256=(Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}

function Get-SdkReleaseTree([string]$Directory) {
    $base = [IO.Path]::GetFullPath($Directory)
    $stack = [Collections.Generic.Stack[string]]::new(); $stack.Push($base)
    $records = [Collections.Generic.SortedDictionary[string,object]]::new([StringComparer]::Ordinal)
    $count = 0
    while ($stack.Count) {
        $directoryItem = Get-Item -LiteralPath $stack.Pop() -Force
        if (-not $directoryItem.PSIsContainer -or ($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'SDK 输入树不能使用链接或 junction。' }
        foreach ($item in Get-ChildItem -LiteralPath $directoryItem.FullName -Force) {
            if (++$count -gt 8192 -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'SDK 输入树过大或包含链接。' }
            if ($item.PSIsContainer) { $stack.Push($item.FullName); continue }
            $relative = [IO.Path]::GetRelativePath($base, $item.FullName).Replace('\','/')
            $record = Get-SdkReleaseFile $item.FullName
            $records.Add($relative, [ordered]@{path=$relative;size=$record.size;sha256=$record.sha256})
        }
    }
    return @($records.Values)
}

function Get-SdkReleaseSourceState {
    $repository = (Read-SdkReleaseGit @('rev-parse','--show-toplevel')).Trim()
    if (-not [IO.Path]::GetFullPath($repository).Equals($root, [StringComparison]::OrdinalIgnoreCase)) { throw '脚本并不位于独立的源码仓库根。' }
    # 完整状态包括所有未跟踪文件和子模块；不使用应用候选的缩减状态规则。
    $status = Read-SdkReleaseGit @('status','--porcelain=v1','--untracked-files=all','--ignore-submodules=none')
    if ($status.Length) { throw '正式 SDK 构建要求完整 Git 状态洁净，不接收本地候选或未跟踪素材。' }
    $indexEntries = (Read-SdkReleaseGit @('ls-files','-v','-z')).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
    foreach ($entry in $indexEntries) {
        if (-not $entry.StartsWith('H ', [StringComparison]::Ordinal)) { throw '正式 SDK 不接收 assume-unchanged、skip-worktree 或其他隐藏索引状态。' }
    }
    $head = (Read-SdkReleaseGit @('rev-parse','HEAD')).Trim()
    $version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
    $null = ConvertTo-SdkReleaseVersion $version
    $tag = (Read-SdkReleaseGit @('rev-parse',"refs/tags/$version^{commit}")).Trim()
    if ($head -cnotmatch '^[0-9a-f]{40}$' -or $tag -cne $head) { throw '正式 SDK 构建要求 VERSION 的精确 tag 指向当前 HEAD。' }
    $submodules = Read-SdkReleaseGit @('submodule','status','--recursive')
    if ($submodules -match '(?m)^[-+U]') { throw '子模块未初始化、存在偏移或冲突。' }
    $files = @(
        foreach ($relative in (Read-SdkReleaseGit @('ls-files','--cached','-z')).Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
            $path = Resolve-AwjRepoPath -RepositoryRoot $root -Path $relative
            $expectedBlob = (Read-SdkReleaseGit @('rev-parse',"HEAD:$relative")).Trim()
            $actualBlob = (Read-SdkReleaseGit @('-c','core.autocrlf=input','-c','core.safecrlf=false','hash-object',"--path=$relative",'--',$relative)).Trim()
            if ($expectedBlob -cnotmatch '^[0-9a-f]{40,64}$' -or $actualBlob -cne $expectedBlob) { throw "SDK 实际源码不属于当前精确 tag：$relative" }
            $record = Get-SdkReleaseFile $path
            [ordered]@{path=$relative;size=$record.size;sha256=$record.sha256;git_blob=$expectedBlob}
        }
    )
    return [ordered]@{source_root=$root;version=$version;git_head=$head;tag_commit=$tag;git_status=$status;
        submodules=$submodules;source_fingerprint=(Get-AwjSourceFingerprint -RepositoryRoot $root);files=$files}
}

function Resolve-SdkReleaseTool([string]$Command) {
    $tool = Get-Command $Command -CommandType Application -ErrorAction Stop | Select-Object -First 1
    if ($tool.Source -match '(?i)[\\/]shims[\\/]' -or [IO.Path]::GetExtension($tool.Source) -ine '.exe') {
        throw '正式 SDK 要求显式的真实工具可执行文件，不接收 Scoop shim 或脚本包装入口；请指定安装目录中的 CMake/CTest/Ninja 路径。'
    }
    return (Get-SdkReleaseFile $tool.Source)
}

function Read-SdkReleaseCache([string]$Path) {
    $cache = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if (-not $line -or $line.StartsWith('#') -or $line.StartsWith('//')) { continue }
        if ($line -cnotmatch '^([^:=]+):[^=]*=(.*)$' -or -not $cache.TryAdd($Matches[1],$Matches[2])) { throw 'CMakeCache 存在重复或无效配置。' }
    }
    return $cache
}

function Assert-SdkReleaseConfiguration($Cache, $Msvc, [string]$Build, [string]$Dependencies, [string]$Headers, [string]$TestLibrary, [string]$Ninja, [string]$Vcpkg, [string]$CMake, [string]$CTest) {
    # IPO_SUPPORTED 是生成期普通变量，不假定它被写入 Cache；真实 IPO 由目标 profile/命令双重核对。
    foreach ($key in @('BUILD_TESTING','CMAKE_EXPORT_COMPILE_COMMANDS','VCPKG_MANIFEST_INSTALL','VCPKG_MANIFEST_MODE',
            'AVIF_STATIC_MSVC_RUNTIME','AVIF_ENABLE_RELEASE_IPO',
            'AWJ_BUILD_SDK','AWJ_SDK_NATIVE_CODECS','AWJ_SDK_D3D11','AWJ_SDK_SHM','AWJ_SDK_FFMPEG','AWJ_SDK_TOOLS','AWJ_ENABLE_JPEGLI')) {
        if (-not $Cache.ContainsKey($key) -or $Cache[$key] -notmatch '^(1|ON|TRUE|YES|Y)$') { throw "正式 SDK 有效配置未开启：$key" }
    }
    foreach ($key in @('AWJ_BUILD_APPLICATION','AWJ_ENABLE_X64_V3','AWJ_USE_PORTABLE_STDLIB')) {
        if (-not $Cache.ContainsKey($key) -or $Cache[$key] -notmatch '^(0|OFF|FALSE|NO|N|IGNORE|NOTFOUND|)$') { throw "正式 SDK 有效配置不符合基线：$key" }
    }
    foreach ($pair in @(@('CMAKE_BUILD_TYPE','Release'),@('CMAKE_GENERATOR','Ninja'),@('CMAKE_MSVC_RUNTIME_LIBRARY','MultiThreaded'),@('VCPKG_TARGET_TRIPLET','x64-windows-static'))) {
        if (-not $Cache.ContainsKey($pair[0]) -or $Cache[$pair[0]] -cne $pair[1]) { throw "正式 SDK 有效配置不匹配：$($pair[0])" }
    }
    foreach ($pair in @(@('CMAKE_HOME_DIRECTORY',(Join-Path $root 'sdk')),@('CMAKE_C_COMPILER',$Msvc.compiler),@('CMAKE_CXX_COMPILER',$Msvc.compiler),
            @('CMAKE_LINKER',$Msvc.linker),@('CMAKE_AR',$Msvc.archive),@('CMAKE_RC_COMPILER',$Msvc.resource),
            @('CMAKE_COMMAND',$CMake),@('CMAKE_CTEST_COMMAND',$CTest),@('CMAKE_MAKE_PROGRAM',$Ninja),
            @('CMAKE_TOOLCHAIN_FILE',(Join-Path $Vcpkg 'scripts/buildsystems/vcpkg.cmake')),
            @('VCPKG_ROOT',$Vcpkg),@('VCPKG_MANIFEST_DIR',(Join-Path $root 'sdk')),@('VCPKG_INSTALLED_DIR',(Join-Path $Dependencies 'installed')),
            @('VCPKG_OVERLAY_PORTS',(Join-Path $root 'vcpkg-overlays')),@('AWJ_RUNTIME_OUTPUT_ROOT',(Join-Path $Build 'runtime')),
            @('FETCHCONTENT_BASE_DIR',(Join-Path $Build '_deps')),@('AWJ_SDK_AVUTIL_INCLUDE_DIR',$Headers),@('AWJ_SDK_FFMPEG_TEST_AVUTIL',$TestLibrary))) {
        if (-not $Cache.ContainsKey($pair[0])) { throw "正式 SDK 实际输入路径不匹配：$($pair[0])" }
        if ($pair[0] -cin @('CMAKE_C_COMPILER','CMAKE_CXX_COMPILER','CMAKE_LINKER','CMAKE_AR','CMAKE_RC_COMPILER','CMAKE_COMMAND','CMAKE_CTEST_COMMAND','CMAKE_MAKE_PROGRAM')) {
            $samePath = (Resolve-SdkReleasePhysicalPath $Cache[$pair[0]]).Equals((Resolve-SdkReleasePhysicalPath $pair[1]), [StringComparison]::OrdinalIgnoreCase)
        } else {
            $samePath = [IO.Path]::GetFullPath($Cache[$pair[0]]).Equals([IO.Path]::GetFullPath($pair[1]), [StringComparison]::OrdinalIgnoreCase)
        }
        if (-not $samePath) { throw "正式 SDK 实际输入路径不匹配：$($pair[0])" }
    }
    foreach ($key in @('CMAKE_C_COMPILER_LAUNCHER','CMAKE_CXX_COMPILER_LAUNCHER','CMAKE_C_LINKER_LAUNCHER','CMAKE_CXX_LINKER_LAUNCHER','VCPKG_CHAINLOAD_TOOLCHAIN_FILE')) {
        if ($Cache.ContainsKey($key) -and $Cache[$key]) { throw "正式 SDK 不接收未审计的编译/链接代理：$key" }
    }
    foreach ($key in $Cache.Keys) {
        if ($key -cmatch '^FETCHCONTENT_SOURCE_DIR_' -and $Cache[$key]) { throw '正式 SDK 不接收跨源码路径复用的 FetchContent 源码/对象。' }
    }
}

function Get-SdkReleaseRequiredTests {
    return @('sdk_core','sdk_c_abi','sdk_native_encode','sdk_pixel_conversion','sdk_borrowed_yuv','sdk_stdlib_contract',
        'sdk_codec_worker_failure','sdk_portable_stdlib_contract','sdk_shared_memory','sdk_ffmpeg','sdk_d3d11','sdk_d3d11_shared')
}

function Get-SdkReleaseTestExecutables {
    return [ordered]@{sdk_core='awj_sdk_tests';sdk_c_abi='awj_sdk_c_consumer';sdk_native_encode='awj_sdk_encode_tests'
        sdk_pixel_conversion='awj_sdk_pixel_tests';sdk_borrowed_yuv='awj_sdk_yuv_readonly_tests';sdk_stdlib_contract='awj_sdk_stdlib_tests'
        sdk_codec_worker_failure='awj_sdk_worker_failure_tests';sdk_portable_stdlib_contract='awj_sdk_portable_stdlib_tests'
        sdk_shared_memory='awj_sdk_shm_tests';sdk_ffmpeg='awj_sdk_ffmpeg_tests';sdk_d3d11='awj_sdk_d3d11_tests';sdk_d3d11_shared='awj_sdk_d3d11_tests'}
}

function Assert-SdkReleaseTestPlan([string]$Path, [string]$Build, [string]$TestLibrary) {
    $plan = [IO.File]::ReadAllText($Path) | ConvertFrom-Json -AsHashtable
    Assert-AwjJsonFields $plan
    $expected = Get-SdkReleaseTestExecutables
    if ($plan.tests -isnot [Array] -or $plan.tests.Count -ne $expected.Count) { throw '实际 CTest 执行计划必须恰好覆盖 Windows profile 的 12 个 SDK 测试。' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($test in $plan.tests) {
        Assert-AwjJsonFields $test @('name')
        if (-not $expected.Contains($test.name) -or -not $seen.Add($test.name) -or $test.command -isnot [Array]) { throw '实际 CTest 测试未知、重复或缺少程序。' }
        $arguments = @((Join-Path $Build "runtime/Release/$($expected[$test.name]).exe"))
        if ($test.name -ceq 'sdk_ffmpeg') { $arguments += $TestLibrary }
        if ($test.name -ceq 'sdk_d3d11_shared') { $arguments += '--shared' }
        if ($test.command.Count -ne $arguments.Count) { throw '实际 CTest 命令含未知包装器、运行参数或旧程序。' }
        for ($index = 0; $index -lt $arguments.Count; ++$index) {
            if ($test.command[$index] -isnot [string]) { throw '实际 CTest 命令参数必须为字符串。' }
            if ($index -eq 0 -or $test.name -ceq 'sdk_ffmpeg') {
                if (-not [IO.Path]::IsPathFullyQualified($test.command[$index]) -or
                    -not [IO.Path]::GetFullPath($test.command[$index]).Equals([IO.Path]::GetFullPath($arguments[$index]), [StringComparison]::OrdinalIgnoreCase)) {
                    throw '实际 CTest 未执行本轮的精确 SDK 程序或显式 FFmpeg 测试 DLL。'
                }
            } elseif ($test.command[$index] -cne $arguments[$index]) { throw '实际 CTest 参数与 profile 不匹配。' }
        }
        if ($test.properties -isnot [Array]) { throw '实际 CTest 属性必须为数组。' }
        foreach ($property in $test.properties) {
            Assert-AwjJsonFields $property @('name')
            if ($property.name -cin @('ENVIRONMENT','ENVIRONMENT_MODIFICATION') -or
                ($property.name -ceq 'DISABLED' -and $property.value -notin @($false,0,'OFF','FALSE','0'))) { throw '正式 SDK 不允许隐藏禁用或未审计的测试环境覆盖。' }
            if ($property.name -ceq 'WORKING_DIRECTORY' -and
                -not [IO.Path]::GetFullPath($property.value).Equals((Join-Path $Build 'native'), [StringComparison]::OrdinalIgnoreCase)) {
                throw '实际 CTest 工作目录不属于本轮 SDK 构建。'
            }
        }
    }
}

function Assert-SdkReleasePreTestSnapshot([string]$Path, [string]$Build, $Source, $Msvc) {
    $null = Get-SdkReleaseFile $Path
    $json = [IO.File]::ReadAllText($Path)
    $snapshot = $json | ConvertFrom-Json -AsHashtable
    # PowerShell 可把 ISO 字符串自动转成 DateTime；仍按原 JSON 类型校验，不能因此放宽字段契约。
    $document = [System.Text.Json.JsonDocument]::Parse($json)
    try {
        $createdAt = $document.RootElement.GetProperty('created_at')
        if ($createdAt.ValueKind -ne [System.Text.Json.JsonValueKind]::String) { throw 'JSON 字段必须是字符串：created_at' }
        $snapshot['created_at'] = $createdAt.GetString()
    } finally { $document.Dispose() }
    Assert-AwjJsonFields $snapshot @('kind','source_root','build_directory','git_head','source_fingerprint','created_at')
    if (-not (Test-AwjJsonUnsignedInteger $snapshot.schema) -or $snapshot.schema -ne 1 -or $snapshot.kind -cne 'windows-sdk-pre-test-snapshot' -or
        -not $snapshot.source_root.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        -not $snapshot.build_directory.Equals($Build, [StringComparison]::OrdinalIgnoreCase) -or
        $snapshot.git_head -cne $Source.git_head -or $snapshot.source_fingerprint -cne $Source.source_fingerprint -or
        $snapshot.files -isnot [Array] -or $snapshot.files.Count -gt 128) { throw '测试前快照没有绑定当前源码和全新构建。' }
    Assert-AwjMsvcReleaseEvidence $snapshot.toolchain @('awj_sdk') x64
    foreach ($key in @('compiler_version','compiler_sha256','linker_version','linker_sha256')) {
        if ($snapshot.toolchain[$key] -cne $Msvc.$key) { throw '测试前工具链不属于本轮实际 MSVC。' }
    }
    $required = @('CMakeCache.txt','compile_commands.json','.ninja_log','build.ninja','CMakeFiles/rules.ninja',
        'awj-msvc-awj_sdk-Release.txt','sdk-validation-targets.dot','sdk-validation-commands.txt','sdk-ctest-plan.json',
        'CTestTestfile.cmake','native/CTestTestfile.cmake')
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($record in $snapshot.files) {
        Assert-AwjJsonFields $record @('path','sha256')
        if (-not $seen.Add($record.path) -or ($record.path -cnotin $required -and $record.path -cnotmatch '^runtime/Release/[A-Za-z0-9_.-]+$') -or
            $record.sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not (Test-AwjJsonUnsignedInteger $record.size)) { throw '测试前来源清单有未知、重复或越界输入。' }
        $safe = Assert-AwjBuildPath -RepositoryRoot $root -Path (Join-Path $Build $record.path)
        $actual = Get-SdkReleaseFile $safe
        if ($actual.sha256 -cne $record.sha256 -or $actual.size -ne $record.size) { throw "真实配置、程序或 DLL 在测试前后发生变化：$($record.path)" }
    }
    foreach ($name in $required + @('runtime/Release/awj_sdk.dll','runtime/Release/awj-stream.exe')) {
        if (-not $seen.Contains($name)) { throw "测试前快照缺少实际输入：$name" }
    }
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $Build 'runtime/Release') -Force) {
        if ($item.PSIsContainer -or -not $seen.Contains("runtime/Release/$($item.Name)")) { throw '测试后 runtime 出现未封存的程序或输入。' }
    }
    return $snapshot
}

function Get-SdkReleaseGraphPaths([string]$Path, [string]$Build) {
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $text = [IO.File]::ReadAllText($Path)
    if ($text -notmatch '^digraph ninja \{' -or $text.Length -gt 16777216) { throw '实际 validation 依赖图无效。' }
    foreach ($line in $text.Split([char]10)) {
        if ($line -match '^\s*"[^"\r\n]+" \[label="((?:\\.|[^"\\])*)"\];?\s*$') {
            # Ninja DOT 只在文件节点使用此形状；规则节点还带 shape=ellipse，不能当产物。
            $label = $Matches[1].Replace('\\','\').Replace('\"','"')
            if ($label -match '[\r\n]' -or -not $label) { throw '实际 Ninja 路径无效。' }
            [void]$paths.Add([IO.Path]::GetFullPath($label, $Build))
        }
    }
    if (-not $paths.Contains((Join-Path $Build 'awj_sdk_validation'))) { throw '实际依赖图不是 awj_sdk_validation 的闭包。' }
    return ,$paths
}

function Resolve-SdkReleasePhysicalPath([string]$Path, [string]$BaseDirectory = '') {
    $absolute = if ($BaseDirectory) { [IO.Path]::GetFullPath($Path, $BaseDirectory) } else { [IO.Path]::GetFullPath($Path) }
    if (-not $IsWindows) { throw 'SDK Windows 文件身份解析仅适用于 Windows。' }
    if (-not ('AwjSdkReleaseNativePath' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
public static class AwjSdkReleaseNativePath {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint count, uint flags);
    public static string Resolve(string path) {
        using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            var result = new StringBuilder(512);
            for (;;) {
                uint needed = GetFinalPathNameByHandleW(file.SafeFileHandle, result, (uint)result.Capacity, 0);
                if (needed == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
                if (needed < result.Capacity) return result.ToString();
                if (needed >= 32768) throw new IOException("SDK tool path is too long.");
                result = new StringBuilder(checked((int)needed + 1));
            }
        }
    }
}
'@
    }
    $resolved = [AwjSdkReleaseNativePath]::Resolve($absolute)
    if ($resolved.StartsWith('\\?\UNC\', [StringComparison]::OrdinalIgnoreCase)) { return '\\' + $resolved.Substring(8) }
    if ($resolved.StartsWith('\\?\', [StringComparison]::Ordinal)) { return $resolved.Substring(4) }
    return $resolved
}

function Assert-SdkReleaseLinkDriver([string]$Command, [string]$Directory, [string]$Linker) {
    $candidate = ''
    if ($Command -match '(?i)\s-E\s+vs_link_(dll|exe)(?=\s)' -and
        $Command -match '(?:^|\s)--\s+(?:"(?<driver>[^"]+)"|(?<driver>\S+))') {
        $candidate = $Matches['driver']
    } elseif ($Command -match '^\s*(?:"(?<driver>[^"]+)"|(?<driver>\S+))') {
        $candidate = $Matches['driver']
    }
    if (-not $candidate -or -not [IO.Path]::IsPathFullyQualified($candidate) -or
        -not (Resolve-SdkReleasePhysicalPath $candidate $Directory).Equals((Resolve-SdkReleasePhysicalPath $Linker), [StringComparison]::OrdinalIgnoreCase)) {
        throw '缺少本轮真实 link.exe 的 SDK 最终链接命令。'
    }
}

function Assert-SdkReleaseCompileCommand([string]$Command, [string]$Directory, [string]$Compiler) {
    if ($Command -notmatch '^\s*(?:"([^"]+)"|(\S+))') { throw 'SDK 编译命令缺少真实驱动。' }
    $driver = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
    if (-not (Resolve-SdkReleasePhysicalPath $driver $Directory).Equals((Resolve-SdkReleasePhysicalPath $Compiler), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'SDK 实际编译驱动并非本轮 cl.exe。'
    }
    $flags = @([regex]::Matches($Command, '(?<!\S)[/-][A-Za-z][^\s]*') | ForEach-Object { '/' + $_.Value.Substring(1) })
    foreach ($flag in @('/O2','/Ob2','/Oi','/Ot','/Gy','/Gw','/GF','/GL','/MT')) {
        if ($flag -cnotin $flags) { throw "SDK 实际编译命令缺少完整 Release/MT/IPO 参数：$flag" }
    }
    if (@($flags | Where-Object { $_ -match '^/(arch:|Od$|O1$|Os$|Ob[01]$|(?:GL|Oi|Ot|Gy|Gw|GF)-$|MDd?$|MTd$|fp:fast$)' }).Count) {
        throw 'SDK 实际编译命令降档或超出 x64 基线。'
    }
}

function Get-SdkReleaseCompiledInputs([string]$Build, $Msvc) {
    # 新构建树 + 真实 validation 依赖闭包 + Ninja 实际输出，三者缺一不可。
    $graph = Get-SdkReleaseGraphPaths (Join-Path $Build 'sdk-validation-targets.dot') $Build
    $log = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $Build '.ninja_log'))) {
        if ($line.StartsWith('#')) { continue }
        $parts = $line.Split([char]9)
        if ($parts.Count -ne 5) { throw 'Ninja 实际构建记录无效。' }
        [void]$log.Add([IO.Path]::GetFullPath($parts[3], $Build))
    }
    $commands = [IO.File]::ReadAllText((Join-Path $Build 'compile_commands.json')) | ConvertFrom-Json -AsHashtable
    if ($commands -isnot [Array] -or -not $commands.Count -or $commands.Count -gt 65536) { throw '缺少真实 CMake 编译命令集。' }
    foreach ($entry in $commands) {
        Assert-AwjJsonFields $entry @('directory','file','command','output')
        if (-not [IO.Path]::GetFullPath($entry.directory).Equals($Build, [StringComparison]::OrdinalIgnoreCase)) { throw '实际编译目录不属于本轮全新构建树。' }
        $null = Resolve-AwjRepoPath -RepositoryRoot $root -Path ([IO.Path]::GetFullPath($entry.file, $entry.directory))
        $object = Assert-AwjBuildPath -RepositoryRoot $root -Path ([IO.Path]::GetFullPath($entry.output, $entry.directory))
        if (-not $object.StartsWith($Build + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw '实际编译对象越过本轮构建树。' }
    }
    $records = @(
        foreach ($relative in @('src/sdk/awj_sdk.cpp','src/sdk/frame_stream.cpp','src/sdk/native_encode.cpp','src/interop/d3d11_frame.cpp','src/interop/frame_transport_win32.cpp','src/interop/ffmpeg_frame.cpp')) {
            $source = Join-Path $root $relative
            $entries = @($commands | Where-Object {
                [IO.Path]::GetFullPath($_.file, $_.directory).Equals($source, [StringComparison]::OrdinalIgnoreCase) -and
                $_.command -match 'CMakeFiles[/\\]awj_sdk\.dir[/\\]'
            })
            if ($entries.Count -ne 1) { throw "实际 SDK 编译输入缺失或重复：$relative" }
            $entry = $entries[0]
            Assert-SdkReleaseCompileCommand $entry.command $entry.directory $Msvc.compiler
            if ($entry.command -notmatch '(?i)(?<!\S)[/-]Fo(?:"([^"]+)"|(\S+))') { throw 'SDK 实际编译命令缺少对象输出。' }
            $objectName = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
            $object = Assert-AwjBuildPath -RepositoryRoot $root -Path ([IO.Path]::GetFullPath($objectName, $entry.directory))
            if (-not $object.Equals([IO.Path]::GetFullPath($entry.output, $entry.directory), [StringComparison]::OrdinalIgnoreCase) -or
                -not $log.Contains($object) -or -not $graph.Contains($object)) { throw 'SDK 对象没有本轮真实编译或 validation 依赖记录。' }
            [ordered]@{source=$relative;source_sha256=(Get-SdkReleaseFile $source).sha256;object=(Get-SdkReleaseFile $object)}
        }
    )
    $names = @('awj_sdk.dll','awj-stream.exe') + @((Get-SdkReleaseTestExecutables).Values | Sort-Object -Unique | ForEach-Object { "$_.exe" })
    # 某些源码版本有 WGC 辅助目标；仅在真实 validation 图中存在时采集，不要求不存在的 exe。
    $optional = 'awj_wgc_cli_validation.exe'
    if ($graph.Contains((Join-Path $Build "runtime/Release/$optional"))) { $names += $optional }
    $binaries = @(
        foreach ($name in $names) {
            $path = Join-Path $Build "runtime/Release/$name"
            if (-not $log.Contains($path) -or -not $graph.Contains($path)) { throw "程序没有本轮真实最终链接或 validation 依赖记录：$name" }
            Get-SdkReleaseFile $path
        }
    )
    foreach ($path in $graph) {
        if ($path.StartsWith((Join-Path $Build 'runtime/Release') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetExtension($path) -in @('.dll','.exe') -and [IO.Path]::GetFileName($path) -cnotin $names) {
            throw '实际 validation 存在未审计的 SDK 目标。'
        }
    }
    $links = @(
        foreach ($line in [IO.File]::ReadAllLines((Join-Path $Build 'sdk-validation-commands.txt'))) {
            if ($line -match '(?i)(?<!\S)/out:(?:"([^"]+)"|([^\s"]+))') {
                $name = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
                if ([IO.Path]::GetFullPath($name, $Build).Equals((Join-Path $Build 'runtime/Release/awj_sdk.dll'), [StringComparison]::OrdinalIgnoreCase)) { $line }
            }
        }
    )
    if ($links.Count -ne 1) { throw '缺少本轮真实 link.exe 的 SDK 最终链接命令。' }
    Assert-SdkReleaseLinkDriver $links[0] $Build $Msvc.linker
    foreach ($flag in @('/LTCG','/OPT:REF','/OPT:ICF','/INCREMENTAL:NO','/machine:x64')) {
        if ($links[0] -notmatch ('(?i)(?<!\S)' + [regex]::Escape($flag) + '(?=\s|$)')) { throw "SDK 实际最终链接降档：$flag" }
    }
    if ($links[0] -match '(?i)(?<!\S)/(LTCG:OFF|INCREMENTAL(:YES)?|OPT:(NOREF|NOICF))(?=\s|$)') { throw 'SDK 实际链接命令覆盖完整 IPO/LTO。' }
    return [ordered]@{sources=$records;binaries=$binaries;validation_target='awj_sdk_validation';link_command_sha256=(
        [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($links[0]))).ToLowerInvariant())}
}

function Assert-SdkReleaseEnvironment {
    $replaced = @('VCPKG_ROOT','VCPKG_DOWNLOADS','VCPKG_REGISTRIES_CACHE','VCPKG_BINARY_SOURCES','VCPKG_MAX_CONCURRENCY')
    foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
        if ($entry.Value -and ($entry.Key -match '^(CL|_CL_|LINK|_LINK_|CC|CXX|CFLAGS|CXXFLAGS|CPPFLAGS|LDFLAGS)$' -or
            ($entry.Key -match '^(CMAKE_|FETCHCONTENT_|VCPKG_)' -and $entry.Key -cnotin $replaced))) {
            throw "正式 SDK 不接收隐式工具链、依赖或编译覆盖环境变量：$($entry.Key)"
        }
    }
}

function Assert-SdkReleaseDependencyLocks([string]$ReleaseVersion) {
    $lock = [IO.File]::ReadAllText((Join-Path $root 'cmake/codec-dependencies.json')) | ConvertFrom-Json -AsHashtable
    $manifest = [IO.File]::ReadAllText((Join-Path $root 'sdk/vcpkg.json')) | ConvertFrom-Json -AsHashtable
    $configuration = [IO.File]::ReadAllText((Join-Path $root 'sdk/vcpkg-configuration.json')) | ConvertFrom-Json -AsHashtable
    Assert-AwjJsonFields $lock @('awj_version','vcpkg_baseline')
    Assert-AwjJsonFields $manifest @('version')
    Assert-AwjJsonFields $configuration['default-registry'] @('kind','repository','baseline')
    $registry = $configuration['default-registry']
    if ($lock.awj_version -cne $ReleaseVersion -or $manifest.version -cne $ReleaseVersion -or
        $lock.vcpkg_baseline -cnotmatch '^[0-9a-f]{40}$' -or $registry.kind -cne 'git' -or
        $registry.repository -cne 'https://github.com/microsoft/vcpkg' -or $registry.baseline -cne $lock.vcpkg_baseline -or
        $configuration.Count -ne 1 -or $registry.Count -ne 3) { throw 'SDK manifest、源码版本和唯一锁定依赖 baseline 不一致。' }
}

function Invoke-SdkReleaseBuild {
    if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64' -or
        [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -ne 'X64') { throw '正式 Windows SDK 编排仅支持原生 Windows x64 PowerShell 7。' }
    $source = if ($SourceRoot) { $SourceRoot } else { $root }
    $requestedWork = if ($WorkRoot) { $WorkRoot } else { $root }
    $work = Assert-SdkReleaseRoots $root $source $requestedWork
    $output = Assert-AwjBuildPath -RepositoryRoot $root -Path $OutputDirectory
    if (Test-Path -LiteralPath $output) { throw '正式 SDK 编排必须使用全新的输出目录；不接收旧 CMakeCache、BMI、对象、DLL 或 JUnit。' }
    Assert-SdkReleaseEnvironment
    $before = Get-SdkReleaseSourceState
    $beforeText = $before | ConvertTo-Json -Depth 8
    Assert-SdkReleaseDependencyLocks $before.version
    $headers = [IO.Path]::GetFullPath($FFmpegIncludeDirectory, $root)
    $testLibrary = [IO.Path]::GetFullPath($FFmpegTestLibraryPath, $root)
    $headerInputs = @(Get-SdkReleaseTree (Join-Path $headers 'libavutil'))
    if (-not $headerInputs.Count -or -not (Test-Path -LiteralPath (Join-Path $headers 'libavutil/frame.h') -PathType Leaf)) { throw '正式 SDK 必须显式指定真实 FFmpeg 头目录。' }
    $headerText = ConvertTo-Json -InputObject $headerInputs -Depth 5
    $testLibraryRecord = Get-SdkReleaseFile $testLibrary
    $cmake = Resolve-SdkReleaseTool $CMakePath
    $requestedCTest = if ($CTestPath) { $CTestPath } else { Join-Path (Split-Path $cmake.path -Parent) 'ctest.exe' }
    $ctest = Resolve-SdkReleaseTool $requestedCTest
    $ninja = Resolve-SdkReleaseTool $NinjaPath
    $vcpkg = [IO.Path]::GetFullPath($VcpkgDirectory, $root)
    $vcpkgInputs = @((Get-SdkReleaseFile (Join-Path $vcpkg 'vcpkg.exe')),
        (Get-SdkReleaseFile (Join-Path $vcpkg 'scripts/buildsystems/vcpkg.cmake')),
        (Get-SdkReleaseFile (Join-Path $vcpkg 'triplets/x64-windows-static.cmake')))
    $vcpkgScripts = @(Get-SdkReleaseTree (Join-Path $vcpkg 'scripts'))
    $vcpkgScriptsText = ConvertTo-Json -InputObject $vcpkgScripts -Depth 5
    $downloads = if ($DependencyDownloadsDirectory) { [IO.Path]::GetFullPath($DependencyDownloadsDirectory, $root) } else { '' }
    $binaryCache = if ($DependencyBinaryCacheDirectory) { [IO.Path]::GetFullPath($DependencyBinaryCacheDirectory, $root) } else { '' }
    foreach ($cachePath in @($downloads,$binaryCache)) {
        if ($cachePath -and ($cachePath -match '[,;\r\n]' -or -not (Test-Path -LiteralPath $cachePath -PathType Container))) { throw '复用缓存必须是已存在的显式普通目录，不得注入 vcpkg cache 选项。' }
    }
    $compilerRoot = if ($CompilerDirectory) { [IO.Path]::GetFullPath($CompilerDirectory, $root) } else { '' }
    $msvc = Initialize-AwjMsvcToolchain -CompilerDirectory $compilerRoot
    $msvcEnvironment = [ordered]@{include=$env:INCLUDE;lib=$env:LIB;libpath=$env:LIBPATH;vc_tools=$env:VCToolsInstallDir;windows_sdk=$env:WindowsSdkDir}
    $env:VCPKG_ROOT = $vcpkg
    $tools = @($cmake,$ctest,$ninja) + $vcpkgInputs + @((Get-SdkReleaseFile $msvc.compiler),(Get-SdkReleaseFile $msvc.linker),(Get-SdkReleaseFile $msvc.archive),(Get-SdkReleaseFile $msvc.resource))
    $build = Join-Path $output 'build'; $install = Join-Path $output 'install'; $dependencies = Join-Path $output 'dependencies'
    $junit = Join-Path $output 'sdk-tests.xml'; $preTestPath = Join-Path $output 'pre-test-snapshot.json'
    $requiredTests = @(Get-SdkReleaseRequiredTests)
    $started = [DateTimeOffset]::UtcNow.ToString('o')
    [void][IO.Directory]::CreateDirectory($output)
    [IO.File]::WriteAllText((Join-Path $output 'source-state-before.json'), $beforeText, [Text.UTF8Encoding]::new($false))
    ConvertTo-Json -InputObject $headerInputs -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'ffmpeg-header-inputs.json') -Encoding utf8
    ConvertTo-Json -InputObject $vcpkgScripts -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'vcpkg-script-inputs.json') -Encoding utf8
    # 只复用核验下载及 ABI 内容寻址缓存；不复制任何项目对象、BMI、安装树或补丁化依赖源码。
    $extra = @('-DAWJ_SDK_D3D11=ON','-DAWJ_SDK_SHM=ON','-DAWJ_SDK_FFMPEG=ON','-DAWJ_SDK_TOOLS=ON',
        '-DAWJ_BUILD_APPLICATION=OFF','-DAWJ_BUILD_SDK=ON','-DAWJ_ENABLE_JPEGLI=ON','-DAVIF_STATIC_MSVC_RUNTIME=ON',
        '-DAWJ_ENABLE_X64_V3=OFF','-DAWJ_USE_PORTABLE_STDLIB=OFF','-DVCPKG_MANIFEST_INSTALL=ON','-DVCPKG_MANIFEST_MODE=ON',
        '-DCMAKE_C_COMPILER_LAUNCHER=','-DCMAKE_CXX_COMPILER_LAUNCHER=','-DCMAKE_C_LINKER_LAUNCHER=','-DCMAKE_CXX_LINKER_LAUNCHER=',
        '-DCMAKE_INSTALL_BINDIR=bin','-DCMAKE_INSTALL_LIBDIR=lib','-DCMAKE_INSTALL_DATADIR=share',
        "-DFETCHCONTENT_BASE_DIR=$($build.Replace('\','/'))/_deps")
    $preTestCapture = @{}
    $validateBeforeTests = {
        param($SnapshotPath, $ActualBuild, $ActualMsvc)
        # ScriptBlock 由本编排直接定义；子脚本调用时保留当前动态作用域中的预构建身份。
        $preTestCapture.file = Get-SdkReleaseFile $SnapshotPath
        $preTestCapture.snapshot = Assert-SdkReleasePreTestSnapshot $SnapshotPath $ActualBuild $before $ActualMsvc
        Assert-SdkReleaseConfiguration (Read-SdkReleaseCache (Join-Path $ActualBuild 'CMakeCache.txt')) $ActualMsvc $ActualBuild $dependencies $headers $testLibrary $ninja.path $vcpkg $cmake.path $ctest.path
        Assert-SdkReleaseTestPlan (Join-Path $ActualBuild 'sdk-ctest-plan.json') $ActualBuild $testLibrary
        $preTestCapture.compiled = Get-SdkReleaseCompiledInputs $ActualBuild $ActualMsvc
        if ((Get-SdkReleaseFile $testLibrary).sha256 -cne $testLibraryRecord.sha256) { throw 'FFmpeg 测试输入在运行前变化。' }
    }
    & (Join-Path $PSScriptRoot 'build-sdk.ps1') -BuildDirectory $build -CompilerDirectory (Split-Path $msvc.compiler -Parent) -Parallelism $Parallelism `
        -CMakePath $cmake.path -CTestPath $ctest.path -NinjaPath $ninja.path -VcpkgDirectory $vcpkg -DependencyDirectory $dependencies `
        -DependencyDownloadsDirectory $downloads -DependencyBinaryCacheDirectory $binaryCache `
        -FFmpegIncludeDirectory $headers -FFmpegTestLibraryPath $testLibrary -TestReportPath $junit -PreTestSnapshotPath $preTestPath -PreTestValidator $validateBeforeTests -VerboseBuild `
        -ExtraCMakeArguments $extra -ExtraCTestArguments @('--no-tests=error','-C','Release','-R',('^(' + ($requiredTests -join '|') + ')$')) *>&1 |
        Tee-Object -FilePath (Join-Path $output 'build-and-tests.log') | Out-Host
    if ($preTestCapture.Count -ne 3) { throw '正式编排的测试前校验器没有完成，不能签发来源证明。' }
    $preTestFile = $preTestCapture.file
    if ((Get-SdkReleaseFile $preTestPath).sha256 -cne $preTestFile.sha256) { throw '测试前快照被测试过程替换。' }
    $preTest = Assert-SdkReleasePreTestSnapshot $preTestPath $build $before $msvc
    Assert-SdkReleaseTestPlan (Join-Path $build 'sdk-ctest-plan.json') $build $testLibrary
    $cache = Read-SdkReleaseCache (Join-Path $build 'CMakeCache.txt')
    Assert-SdkReleaseConfiguration $cache $msvc $build $dependencies $headers $testLibrary $ninja.path $vcpkg $cmake.path $ctest.path
    $profile = Get-AwjMsvcBuildEvidence -BuildDirectory $build -Targets @('awj_sdk') -Toolchain $msvc -MinimumCpu x64
    $compiled = Get-SdkReleaseCompiledInputs $build $msvc
    $compiledText = $compiled | ConvertTo-Json -Depth 8
    if (($preTestCapture.compiled | ConvertTo-Json -Depth 8) -cne $compiledText) { throw '实际对象或 validation 目标在测试前后变化。' }
    $dependencyMetadata = @(Get-SdkReleaseTree (Join-Path $dependencies 'installed/vcpkg'))
    $dependencyText = ConvertTo-Json -InputObject $dependencyMetadata -Depth 5
    ConvertTo-Json -InputObject $dependencyMetadata -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'dependency-metadata.json') -Encoding utf8
    & $cmake.path --install $build --config Release --prefix $install --component AWJimageSDK *>&1 |
        Tee-Object -FilePath (Join-Path $output 'install.log') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '正式 SDK 组件安装失败。' }
    $builtLibrary = Get-SdkReleaseFile (Join-Path $build 'runtime/Release/awj_sdk.dll')
    $installedLibrary = Get-SdkReleaseFile (Join-Path $install 'bin/awj_sdk.dll')
    $testedLibrary = @($preTest.files | Where-Object path -ceq 'runtime/Release/awj_sdk.dll')
    if ($testedLibrary.Count -ne 1 -or $builtLibrary.sha256 -cne $installedLibrary.sha256 -or $installedLibrary.sha256 -cne $testedLibrary[0].sha256) {
        throw '安装 DLL 并非本轮实际链接并在全部 12 个测试前封存的同一 DLL。'
    }
    $licenses = @(Get-SdkReleaseTree (Join-Path $root 'licenses'))
    $installedLicenses = @(Get-SdkReleaseTree (Join-Path $install 'share/AWJimage/licenses'))
    if ((ConvertTo-Json -InputObject $licenses -Depth 5) -cne (ConvertTo-Json -InputObject $installedLicenses -Depth 5)) { throw '组件安装未保留本次完整许可。' }
    # 复用正式规则，退出 0 不能掩盖 skip/disabled；profile 必需集合变化必须显式更新编排。
    $auditPath = Join-Path $output 'audit-sdk-build.cmake'
    [IO.File]::WriteAllText($auditPath, @'
cmake_minimum_required(VERSION 3.30)
include("${SOURCE_ROOT}/cmake/sdk-release-profiles.cmake")
file(READ "${SOURCE_ROOT}/VERSION" version)
string(STRIP "${version}" version)
awj_sdk_select_profile("${version}" windows-x64-msvc Windows x86_64 profile)
awj_sdk_required_tests(profile required_tests)
list(SORT required_tests)
list(SORT EXPECTED_REQUIRED_TESTS)
if(NOT required_tests STREQUAL EXPECTED_REQUIRED_TESTS)
    message(FATAL_ERROR "Windows SDK required test set changed")
endif()
awj_sdk_validate_junit("${TEST_REPORT}" "${required_tests}")
awj_sdk_check_install("${INSTALL_DIRECTORY}" profile)
message(STATUS "AWJ_SDK_BUILD_AUDIT_PASSED")
'@, [Text.UTF8Encoding]::new($false))
    & $cmake.path "-DSOURCE_ROOT=$($root.Replace('\','/'))" "-DTEST_REPORT=$($junit.Replace('\','/'))" `
        "-DINSTALL_DIRECTORY=$($install.Replace('\','/'))" "-DEXPECTED_REQUIRED_TESTS=$($requiredTests -join ';')" -P $auditPath *>&1 |
        Tee-Object -FilePath (Join-Path $output 'audit-sdk-build.log') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '正式 SDK JUnit 或安装树未通过既有正式规则。' }
    $evidenceInputs = @(
        foreach ($path in @((Join-Path $build 'CMakeCache.txt'),(Join-Path $build 'compile_commands.json'),(Join-Path $build '.ninja_log'),
                (Join-Path $build 'build.ninja'),(Join-Path $build 'CMakeFiles/rules.ninja'),(Join-Path $build 'sdk-validation-targets.dot'),
                (Join-Path $build 'sdk-validation-commands.txt'),(Join-Path $build 'sdk-ctest-plan.json'),
                (Join-Path $build 'awj-msvc-awj_sdk-Release.txt'),(Join-Path $build 'msvc-build-profile.json'),$junit,$preTestPath,
                (Join-Path $output 'ffmpeg-header-inputs.json'),(Join-Path $output 'vcpkg-script-inputs.json'),(Join-Path $output 'dependency-metadata.json'),
                (Join-Path $dependencies 'installed/vcpkg/status'),(Join-Path $output 'build-and-tests.log'),(Join-Path $output 'install.log'),
                (Join-Path $output 'audit-sdk-build.log'),$auditPath)) { Get-SdkReleaseFile $path }
    )
    foreach ($tool in $tools) { if ((Get-SdkReleaseFile $tool.path).sha256 -cne $tool.sha256) { throw '实际工具链在正式 SDK 构建期间变化。' } }
    if ((ConvertTo-Json -InputObject @(Get-SdkReleaseTree (Join-Path $vcpkg 'scripts')) -Depth 5) -cne $vcpkgScriptsText -or
        (ConvertTo-Json -InputObject @(Get-SdkReleaseTree (Join-Path $dependencies 'installed/vcpkg')) -Depth 5) -cne $dependencyText) { throw '实际 vcpkg 工具脚本或锁定 ABI 安装元数据在构建/安装期间变化。' }
    if ((ConvertTo-Json -InputObject @(Get-SdkReleaseTree (Join-Path $headers 'libavutil')) -Depth 5) -cne $headerText -or
        (Get-SdkReleaseFile $testLibrary).sha256 -cne $testLibraryRecord.sha256) { throw 'FFmpeg 实际输入在正式 SDK 构建期间变化。' }
    if (((Get-SdkReleaseCompiledInputs $build $msvc) | ConvertTo-Json -Depth 8) -cne $compiledText) { throw '实际源码对象或最终链接产物在安装期间变化。' }
    $null = Assert-SdkReleasePreTestSnapshot $preTestPath $build $before $msvc
    if ((Get-SdkReleaseFile $preTestPath).sha256 -cne $preTestFile.sha256) { throw '测试前快照在安装/封存期间变化。' }
    $after = Get-SdkReleaseSourceState
    $afterText = $after | ConvertTo-Json -Depth 8
    if ($afterText -cne $beforeText) { throw '完整 Git 状态、精确 tag、源码字节或指纹在正式 SDK 构建期间变化。' }
    [IO.File]::WriteAllText((Join-Path $output 'source-state-after.json'), $afterText, [Text.UTF8Encoding]::new($false))
    foreach ($record in $evidenceInputs) { if ((Get-SdkReleaseFile $record.path).sha256 -cne $record.sha256) { throw '实际配置、构建日志或 JUnit 在封存前变化。' } }
    $proof = [ordered]@{
        schema=1;kind='sdk-build';mode='formal';platform='windows-x64';version=$before.version;git_head=$before.git_head;source_dirty=$false
        source_fingerprint_algorithm='sha256/git-normalized-source-v1';source_fingerprint=$before.source_fingerprint
        source_root=$root;work_root=$work;build_directory=$build;install_directory=$install
        profile='windows-x64-msvc';compiler_id='MSVC';compiler_version=$msvc.compiler_version;compiler_sha256=$msvc.compiler_sha256
        target_triple='x86_64-pc-windows-msvc';abi=1;native_codecs=$true;compiled_source_verified=$true
        encoders=@('avif','jpegli','jxl','png','webp');features=[ordered]@{d3d11=$true;shm=$true;ffmpeg=$true;stream=$true}
        toolchain=$profile;library_sha256=$installedLibrary.sha256;test_report_sha256=(Get-SdkReleaseFile $junit).sha256
        provenance=[ordered]@{kind='fresh-in-tree-sdk-build';started_at=$started;completed_at=[DateTimeOffset]::UtcNow.ToString('o')
            source_before=(Get-SdkReleaseFile (Join-Path $output 'source-state-before.json'));source_after=(Get-SdkReleaseFile (Join-Path $output 'source-state-after.json'))
            pre_test_snapshot=$preTestFile;tested_library_sha256=$testedLibrary[0].sha256;msvc_environment=$msvcEnvironment
            tools=$tools;inputs=$evidenceInputs;compiled=$compiled;required_tests=$requiredTests
            ffmpeg_headers=(Get-SdkReleaseFile (Join-Path $output 'ffmpeg-header-inputs.json'));ffmpeg_test_library=$testLibraryRecord
            dependency_cache_policy='locked-downloads-and-vcpkg-abi-cache-only;fresh-project-objects-and-installed-tree'
            dependency_lock=(Get-SdkReleaseFile (Join-Path $root 'cmake/codec-dependencies.json'));sdk_manifest=(Get-SdkReleaseFile (Join-Path $root 'sdk/vcpkg.json'))
            sdk_registry=(Get-SdkReleaseFile (Join-Path $root 'sdk/vcpkg-configuration.json'))}
    }
    $proof | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'build-evidence.json') -Encoding utf8
    [ordered]@{mode='formal';version=$before.version;source_root=$root;work_root=$work;build_evidence=(Join-Path $output 'build-evidence.json')
        test_report=$junit;install_directory=$install;packaging='not-run';ffmpeg_runtime_distribution='not-included'} | ConvertTo-Json -Compress
}

# 构建入口会初始化 VS/vcpkg/TEMP；成功或失败均恢复调用者环境。
$environment = [Environment]::GetEnvironmentVariables('Process')
try { Invoke-SdkReleaseBuild }
finally {
    foreach ($name in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
        if (-not $environment.Contains($name)) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    }
    foreach ($name in $environment.Keys) { [Environment]::SetEnvironmentVariable($name, [string]$environment[$name], 'Process') }
}
