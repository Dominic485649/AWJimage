param([string]$OutputDirectory = '', [string]$CMakePath = 'cmake')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false
$Project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $Project 'scripts/release-safety.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $Project 'scripts/windows-msvc.psm1') -Force
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $Project "build/release-contract-tests/windows-sdk-builder-$([Guid]::NewGuid().ToString('N'))" }
$output = Assert-AwjBuildPath -RepositoryRoot $Project -Path $OutputDirectory
$allowed = [IO.Path]::GetFullPath((Join-Path $Project 'build/release-contract-tests')) + [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)) {
    throw '夹具必须使用本项目 build/release-contract-tests 下的全新目录。'
}
$root = Join-Path $output 'src'
$build = Join-Path $root 'build/fresh'
$dependencies = Join-Path $root 'build/dependencies'
$headers = Join-Path $output 'ffmpeg/include'; $testLibrary = Join-Path $output 'ffmpeg/avutil-61.dll'
$vcpkg = Join-Path $output 'vcpkg'; $ninja = @{path=(Join-Path $output 'tools/ninja.exe')}
$cmake = @{path=(Join-Path $output 'tools/cmake.exe')}; $ctest = @{path=(Join-Path $output 'tools/ctest.exe')}
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Project 'scripts/build-sdk-release.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Windows 正式 SDK 编排语法错误。' }
# 只加载纯校验函数；绝不执行 Invoke-SdkReleaseBuild、编译器、测试二进制或 packager。
foreach ($name in @('ConvertTo-SdkReleaseVersion','Assert-SdkReleaseRoots','Get-SdkReleaseFile','Get-SdkReleaseTree','Resolve-SdkReleaseTool','Resolve-SdkReleasePhysicalPath','Assert-SdkReleaseLinkDriver',
        'Get-SdkReleaseSourceState','Read-SdkReleaseCache','Assert-SdkReleaseConfiguration','Get-SdkReleaseRequiredTests',
        'Get-SdkReleaseTestExecutables','Assert-SdkReleaseTestPlan','Assert-SdkReleasePreTestSnapshot','Get-SdkReleaseGraphPaths',
        'Assert-SdkReleaseCompileCommand','Get-SdkReleaseCompiledInputs','Assert-SdkReleaseEnvironment','Assert-SdkReleaseDependencyLocks')) {
    $node=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name}, $true)
    if (-not $node) { throw "缺少生产校验函数：$name" }
    . ([scriptblock]::Create($node.Extent.Text))
}
$script:checks=[Collections.Generic.List[object]]::new()
function Check([bool]$Condition, [string]$Label) {
    if (-not $Condition) { throw "夹具失败：$Label" }
    $script:checks.Add([ordered]@{name=$Label;result='passed'})
}
function Must-Reject([scriptblock]$Action, [string]$Label) {
    $message=''
    try { & $Action | Out-Null } catch { $message=$_.Exception.Message }
    if (-not $message) { throw "应拒绝但接受了：$Label" }
    if ($message -match 'not recognized|CommandNotFound|找不到.*命令') { throw "夹具依赖错误：$message" }
    $script:checks.Add([ordered]@{name=$Label;result='rejected';reason=$message})
}
function Save-Text([string]$Path, [string]$Text) {
    [void][IO.Directory]::CreateDirectory((Split-Path $Path -Parent))
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Save-Json($Value, [string]$Path) { Save-Text $Path (ConvertTo-Json -InputObject $Value -Depth 24) }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function New-MsvcEvidence($Toolchain) {
    @{compiler_id='MSVC';compiler_version=$Toolchain.compiler_version;compiler_sha256=$Toolchain.compiler_sha256
        linker_version=$Toolchain.linker_version;linker_sha256=$Toolchain.linker_sha256;flags_source='cmake-generated-target-profile'
        targets=@(@{name='awj_sdk';configuration='Release';runtime='MultiThreaded';ipo=$true;minimum_cpu='x64';profile_sha256=('c' * 64)
            compile_flags=@('/O2','/Ob2','/Oi','/Ot','/Gy','/Gw','/GF','/GL','/MT');link_flags=@('/LTCG','/OPT:REF','/OPT:ICF','/INCREMENTAL:NO')})}
}
foreach ($path in @($testLibrary,$ninja.path,$cmake.path,$ctest.path,(Join-Path $output 'tools/cl.exe'),(Join-Path $output 'tools/link.exe'),
        (Join-Path $output 'tools/lib.exe'),(Join-Path $output 'tools/rc.exe'))) { Save-Text $path "metadata-fixture-only:$path" }
$msvc=[pscustomobject]@{compiler=(Join-Path $output 'tools/cl.exe');compiler_version='19.51.36260';compiler_sha256=(Hash (Join-Path $output 'tools/cl.exe'))
    linker=(Join-Path $output 'tools/link.exe');linker_version='14.51.36260.0';linker_sha256=(Hash (Join-Path $output 'tools/link.exe'))
    archive=(Join-Path $output 'tools/lib.exe');resource=(Join-Path $output 'tools/rc.exe')}
Save-Text (Join-Path $root 'VERSION') '1.3.5'
Save-Json @{awj_version='1.3.5';vcpkg_baseline=('d' * 40)} (Join-Path $root 'cmake/codec-dependencies.json')
Save-Json @{version='1.3.5'} (Join-Path $root 'sdk/vcpkg.json')
Save-Json @{'default-registry'=@{kind='git';repository='https://github.com/microsoft/vcpkg';baseline=('d' * 40)}} (Join-Path $root 'sdk/vcpkg-configuration.json')
$cache=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
foreach ($key in @('BUILD_TESTING','CMAKE_EXPORT_COMPILE_COMMANDS','VCPKG_MANIFEST_INSTALL','VCPKG_MANIFEST_MODE',
        'AVIF_STATIC_MSVC_RUNTIME','AVIF_ENABLE_RELEASE_IPO','AWJ_BUILD_SDK','AWJ_SDK_NATIVE_CODECS','AWJ_SDK_D3D11','AWJ_SDK_SHM','AWJ_SDK_FFMPEG','AWJ_SDK_TOOLS','AWJ_ENABLE_JPEGLI')) { $cache[$key]='ON' }
foreach ($key in @('AWJ_BUILD_APPLICATION','AWJ_ENABLE_X64_V3','AWJ_USE_PORTABLE_STDLIB')) { $cache[$key]='OFF' }
foreach ($pair in @(@('CMAKE_BUILD_TYPE','Release'),@('CMAKE_GENERATOR','Ninja'),@('CMAKE_MSVC_RUNTIME_LIBRARY','MultiThreaded'),@('VCPKG_TARGET_TRIPLET','x64-windows-static'),
        @('CMAKE_HOME_DIRECTORY',(Join-Path $root 'sdk')),@('CMAKE_C_COMPILER',$msvc.compiler),@('CMAKE_CXX_COMPILER',$msvc.compiler),@('CMAKE_LINKER',$msvc.linker),
        @('CMAKE_AR',$msvc.archive),@('CMAKE_RC_COMPILER',$msvc.resource),@('CMAKE_COMMAND',$cmake.path),@('CMAKE_CTEST_COMMAND',$ctest.path),
        @('CMAKE_MAKE_PROGRAM',$ninja.path),@('CMAKE_TOOLCHAIN_FILE',(Join-Path $vcpkg 'scripts/buildsystems/vcpkg.cmake')),@('VCPKG_ROOT',$vcpkg),
        @('VCPKG_MANIFEST_DIR',(Join-Path $root 'sdk')),@('VCPKG_INSTALLED_DIR',(Join-Path $dependencies 'installed')),
        @('VCPKG_OVERLAY_PORTS',(Join-Path $root 'vcpkg-overlays')),@('AWJ_RUNTIME_OUTPUT_ROOT',(Join-Path $build 'runtime')),
        @('FETCHCONTENT_BASE_DIR',(Join-Path $build '_deps')),@('AWJ_SDK_AVUTIL_INCLUDE_DIR',$headers),@('AWJ_SDK_FFMPEG_TEST_AVUTIL',$testLibrary))) { $cache[$pair[0]]=$pair[1] }
Save-Text (Join-Path $build 'CMakeCache.txt') (($cache.Keys | Sort-Object | ForEach-Object { "${_}:STRING=$($cache[$_])" }) -join "`n")
$objects=@(); $compileCommands=@()
$flags='/O2 /Ob2 /Oi /Ot /Gy /Gw /GF /GL -MT'
foreach ($relative in @('src/sdk/awj_sdk.cpp','src/sdk/frame_stream.cpp','src/sdk/native_encode.cpp','src/interop/d3d11_frame.cpp','src/interop/frame_transport_win32.cpp','src/interop/ffmpeg_frame.cpp')) {
    $source=Join-Path $root $relative
    $objectName="native/CMakeFiles/awj_sdk.dir/$relative.obj"; $object=Join-Path $build $objectName
    Save-Text $source "fixture-source:$relative"; Save-Text $object "fixture-object:$relative"
    $objects+=$objectName
    $compileCommands+=@{directory=$build;file=$source;output=$object;command=('"' + $msvc.compiler + '" /nologo /TP ' + $flags + ' /Fo' + $objectName + ' -c "' + $source + '"')}
}
Save-Json $compileCommands (Join-Path $build 'compile_commands.json')
$runtimeNames=@('awj_sdk.dll','awj-stream.exe') + @((Get-SdkReleaseTestExecutables).Values | Sort-Object -Unique | ForEach-Object { "$_.exe" })
$graphLines=@('digraph ninja {','"root" [label="awj_sdk_validation"]')
$logLines=@('# ninja log v7')
$index=0
foreach ($name in $objects + @($runtimeNames | ForEach-Object { "runtime/Release/$_" })) {
    $graphLines+='"node-' + $index + '" [label="' + $name + '"]'
    $logLines+="0`t1`t1`t$name`t0000000000000001"
    ++$index
}
$graphLines+='}'
$graphText=$graphLines -join "`n"; $logText=$logLines -join "`n"
Save-Text (Join-Path $build 'sdk-validation-targets.dot') $graphText
Save-Text (Join-Path $build '.ninja_log') $logText
foreach ($name in $runtimeNames) { Save-Text (Join-Path $build "runtime/Release/$name") "fixture-binary-never-execute:$name" }
$linkCommand='cmake -E vs_link_dll -- "' + $msvc.linker + '" /nologo /out:runtime/Release/awj_sdk.dll /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /machine:x64'
Save-Text (Join-Path $build 'sdk-validation-commands.txt') $linkCommand
foreach ($name in @('build.ninja','CMakeFiles/rules.ninja','awj-msvc-awj_sdk-Release.txt','CTestTestfile.cmake','native/CTestTestfile.cmake')) {
    Save-Text (Join-Path $build $name) "metadata-fixture-only:$name"
}
$plan=@{kind='ctestInfo';tests=@()}
foreach ($pair in (Get-SdkReleaseTestExecutables).GetEnumerator()) {
    $arguments=@((Join-Path $build "runtime/Release/$($pair.Value).exe"))
    if ($pair.Key -ceq 'sdk_ffmpeg') { $arguments+=$testLibrary }
    if ($pair.Key -ceq 'sdk_d3d11_shared') { $arguments+='--shared' }
    $plan.tests+=@{name=$pair.Key;command=$arguments;properties=@(@{name='WORKING_DIRECTORY';value=(Join-Path $build 'native')})}
}
$planPath=Join-Path $build 'sdk-ctest-plan.json'; Save-Json $plan $planPath
$before=@{git_head=('a' * 40);source_fingerprint=('b' * 64)}
$snapshot=@{schema=1;kind='windows-sdk-pre-test-snapshot';source_root=$root;build_directory=$build;git_head=$before.git_head;source_fingerprint=$before.source_fingerprint
    created_at='2026-11-11T00:00:00Z';toolchain=(New-MsvcEvidence $msvc);files=@()}
foreach ($name in @('CMakeCache.txt','compile_commands.json','.ninja_log','build.ninja','CMakeFiles/rules.ninja','awj-msvc-awj_sdk-Release.txt',
        'sdk-validation-targets.dot','sdk-validation-commands.txt','sdk-ctest-plan.json','CTestTestfile.cmake','native/CTestTestfile.cmake') + @($runtimeNames | ForEach-Object { "runtime/Release/$_" })) {
    $path=Join-Path $build $name
    $snapshot.files+=@{path=$name;size=(Get-Item -LiteralPath $path).Length;sha256=(Hash $path)}
}
$snapshotPath=Join-Path $root 'build/pre-test-snapshot.json'; Save-Json $snapshot $snapshotPath
$testLibraryRecord=Get-SdkReleaseFile $testLibrary
Check ((Assert-SdkReleaseRoots $root $root $output) -ceq $output) 'own-script-repository-and-work-src'
Check ((Assert-SdkReleaseRoots ($root + '/') ($root + '/') $output) -ceq $output) 'source-root-trailing-separator-is-equivalent'
Check ((Resolve-SdkReleasePhysicalPath $msvc.compiler).Equals([IO.Path]::GetFullPath($msvc.compiler), [StringComparison]::OrdinalIgnoreCase)) 'native-tool-file-path-is-resolved'
$shim=Join-Path $output 'tools/shims/cmake.exe'; Save-Text $shim 'fixture-only-shim-not-executed'
function Invoke-ResolveToolFixture([string]$ToolPath) {
    function Get-Command { param($Name,$CommandType,$ErrorAction) [pscustomobject]@{Source=$ToolPath} }
    Resolve-SdkReleaseTool cmake
}
Must-Reject { Invoke-ResolveToolFixture $shim } 'scoop-shim-rejected-before-any-compilation'
Check ((Invoke-ResolveToolFixture $cmake.path).path -ceq $cmake.path) 'explicit-real-tool-path-accepted'
Must-Reject { Assert-SdkReleaseRoots $root (Join-Path $root 'another-source') $root } 'cannot-relabel-other-repository'
Must-Reject { Assert-SdkReleaseRoots $root $root (Join-Path $output 'unknown-work') } 'cannot-use-unknown-work-root'
foreach ($version in @('1.3.5','1.3.6','2147483647.2147483647.2147483647')) { $null=ConvertTo-SdkReleaseVersion $version; Check $true "canonical-$version" }
foreach ($version in @('1.3.05','1.3.5.0','1.3.5-rc1','2147483648.0.0')) { Must-Reject { ConvertTo-SdkReleaseVersion $version } "invalid-$version" }
Assert-SdkReleaseDependencyLocks '1.3.5'; Check $true 'locked-sdk-manifest-and-registry'
Must-Reject { Assert-SdkReleaseDependencyLocks '1.3.6' } 'manifest-version-binding'
Assert-SdkReleaseConfiguration $cache $msvc $build $dependencies $headers $testLibrary $ninja.path $vcpkg $cmake.path $ctest.path
Check (-not $cache.ContainsKey('AVIF_RELEASE_IPO_SUPPORTED')) 'ipo-normal-variable-not-required-in-cache'
foreach ($key in @('BUILD_TESTING','CMAKE_EXPORT_COMPILE_COMMANDS','VCPKG_MANIFEST_INSTALL','AWJ_SDK_NATIVE_CODECS','AWJ_ENABLE_JPEGLI','AWJ_SDK_D3D11','AWJ_SDK_SHM','AWJ_SDK_FFMPEG','AWJ_SDK_TOOLS')) {
    $saved=$cache[$key]; $cache[$key]='OFF'
    Must-Reject { Assert-SdkReleaseConfiguration $cache $msvc $build $dependencies $headers $testLibrary $ninja.path $vcpkg $cmake.path $ctest.path } "configuration-$key"
    $cache[$key]=$saved
}
foreach ($key in @('CMAKE_HOME_DIRECTORY','CMAKE_CXX_COMPILER','CMAKE_LINKER','CMAKE_COMMAND','CMAKE_CTEST_COMMAND','FETCHCONTENT_BASE_DIR','VCPKG_INSTALLED_DIR','AWJ_SDK_FFMPEG_TEST_AVUTIL')) {
    $saved=$cache[$key]; $cache[$key]=Join-Path $output 'old-source-or-input'
    Must-Reject { Assert-SdkReleaseConfiguration $cache $msvc $build $dependencies $headers $testLibrary $ninja.path $vcpkg $cmake.path $ctest.path } "path-binding-$key"
    $cache[$key]=$saved
}
$cache['FETCHCONTENT_SOURCE_DIR_LIBAVIF']=Join-Path $output 'reused-source'
Must-Reject { Assert-SdkReleaseConfiguration $cache $msvc $build $dependencies $headers $testLibrary $ninja.path $vcpkg $cmake.path $ctest.path } 'no-cross-path-source-cache'
$null=$cache.Remove('FETCHCONTENT_SOURCE_DIR_LIBAVIF')
Assert-SdkReleaseTestPlan $planPath $build $testLibrary; Check $true 'exact-twelve-test-plan'
$plan.tests[0].command[0]=Join-Path $output 'old-sdk-tests.exe'; Save-Json $plan $planPath
Must-Reject { Assert-SdkReleaseTestPlan $planPath $build $testLibrary } 'test-plan-cannot-execute-old-program'
$plan.tests[0].command[0]=Join-Path $build 'runtime/Release/awj_sdk_tests.exe'
$plan.tests[0].properties+=@{name='DISABLED';value=$true}; Save-Json $plan $planPath
Must-Reject { Assert-SdkReleaseTestPlan $planPath $build $testLibrary } 'test-plan-disabled'
$plan.tests[0].properties=@(@{name='WORKING_DIRECTORY';value=(Join-Path $build 'native')}); Save-Json $plan $planPath
$null=Assert-SdkReleasePreTestSnapshot $snapshotPath $build $before $msvc; Check $true 'pre-test-byte-binding'
$savedCreatedAt=$snapshot.created_at
$snapshot.created_at=123; Save-Json $snapshot $snapshotPath
Must-Reject { Assert-SdkReleasePreTestSnapshot $snapshotPath $build $before $msvc } 'snapshot-created-at-must-be-json-string'
$snapshot.created_at=$savedCreatedAt; Save-Json $snapshot $snapshotPath
$library=Join-Path $build 'runtime/Release/awj_sdk.dll'; $libraryText=[IO.File]::ReadAllText($library)
Save-Text $library 'replacement-dll-after-tests'
Must-Reject { Assert-SdkReleasePreTestSnapshot $snapshotPath $build $before $msvc } 'cannot-swap-dll-after-tests'
Save-Text $library $libraryText
$snapshot.files+=@{path='../escaped';size=0;sha256=('a' * 64)}; Save-Json $snapshot $snapshotPath
Must-Reject { Assert-SdkReleasePreTestSnapshot $snapshotPath $build $before $msvc } 'snapshot-no-unknown-input'
$snapshot.files=@($snapshot.files | Where-Object path -cne '../escaped'); Save-Json $snapshot $snapshotPath
$compiled=Get-SdkReleaseCompiledInputs $build $msvc
Check ($compiled.binaries.Count -eq 13) 'required-targets-are-real-validation-dependencies-without-wgc'
$validCommand=$compileCommands[0].command
$longCompiler=Join-Path $output 'tools/compiler path with spaces/cl.exe'; Save-Text $longCompiler 'fixture-compiler-identity-not-executed'
$longLinker=Join-Path $output 'tools/compiler path with spaces/link.exe'; Save-Text $longLinker 'fixture-linker-identity-not-executed'
$releaseFlags='/O2 /Ob2 /Oi /Ot /Gy /Gw /GF /GL /MT'
Assert-SdkReleaseCompileCommand ('"' + $longCompiler + '" ' + $releaseFlags) $build $longCompiler
Assert-SdkReleaseLinkDriver ('"' + $longLinker + '" /out:fixture') $build $longLinker
Assert-SdkReleaseLinkDriver ('"' + $cmake.path + '" -E vs_link_dll --intdir=fixture -- "' + $longLinker + '" /out:fixture') $build $longLinker
Check $true 'quoted-compiler-and-direct-or-cmake-link-driver-identity'
if (-not ('AwjSdkShortPathFixture' -as [type])) {
    Add-Type -TypeDefinition 'using System.Text; using System.Runtime.InteropServices; public static class AwjSdkShortPathFixture { [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern uint GetShortPathNameW(string path,StringBuilder output,uint capacity); }'
}
$shortCompiler=[Text.StringBuilder]::new(32768);$shortLinker=[Text.StringBuilder]::new(32768)
$shortCompilerLength=[AwjSdkShortPathFixture]::GetShortPathNameW($longCompiler,$shortCompiler,32768)
$shortLinkerLength=[AwjSdkShortPathFixture]::GetShortPathNameW($longLinker,$shortLinker,32768)
if ($shortCompilerLength -gt 0 -and $shortCompilerLength -lt 32768 -and $shortLinkerLength -gt 0 -and $shortLinkerLength -lt 32768 -and ($shortCompiler.ToString() -cne $longCompiler -or $shortLinker.ToString() -cne $longLinker)) {
    Assert-SdkReleaseCompileCommand ('"' + $shortCompiler.ToString() + '" ' + $releaseFlags) $build $longCompiler
    Assert-SdkReleaseLinkDriver ('"' + $shortLinker.ToString() + '" /out:fixture') $build $longLinker
    Check $true 'actual-eight-dot-three-tools-resolve-to-the-same-file'
} else {
    $script:checks.Add([ordered]@{name='actual-eight-dot-three-tools';result='not-available';reason='此卷未提供可用的短文件名，未启用或修改系统8.3设置。'})
}
foreach ($flag in @('/arch:AVX2','-arch:AVX2','/MD','-MTd','/GL-','/Od','/fp:fast')) {
    Must-Reject { Assert-SdkReleaseCompileCommand ($validCommand + ' ' + $flag) $build $msvc.compiler } "reject-actual-flag-$flag"
}
Must-Reject { Assert-SdkReleaseCompileCommand ($validCommand.Replace('/GL ','')) $build $msvc.compiler } 'missing-actual-ipo'
Must-Reject { Assert-SdkReleaseCompileCommand $validCommand $build (Join-Path $output 'clang-cl.exe') } 'no-clang-cl-substitution'
Save-Text (Join-Path $build 'sdk-validation-targets.dot') ($graphText.Replace('runtime/Release/awj_sdk_tests.exe','runtime/Release/unrelated.exe'))
Must-Reject { Get-SdkReleaseCompiledInputs $build $msvc } 'declared-program-must-be-in-validation-closure'
Save-Text (Join-Path $build 'sdk-validation-targets.dot') $graphText
Save-Text (Join-Path $build '.ninja_log') ($logText -replace '(?m)^.*runtime/Release/awj_sdk_tests\.exe.*(?:\n|$)','')
Must-Reject { Get-SdkReleaseCompiledInputs $build $msvc } 'program-must-have-fresh-final-link-record'
Save-Text (Join-Path $build '.ninja_log') $logText
Save-Text (Join-Path $build 'sdk-validation-commands.txt') ($linkCommand.Replace('/LTCG','/LTCG:OFF'))
Must-Reject { Get-SdkReleaseCompiledInputs $build $msvc } 'actual-link-must-retain-ltcg'
Save-Text (Join-Path $build 'sdk-validation-commands.txt') $linkCommand
# 精确调用真实生产回调，验证跨子脚本动态作用域和测试前封存，而非只检查参数名称。
$callback=$ast.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -ceq '$validateBeforeTests'}, $true)
if (-not $callback) { throw '生产编排没有测试前校验回调。' }
$preTestCapture=@{}
$validateBeforeTests = & ([scriptblock]::Create($callback.Right.Extent.Text))
function Invoke-FixtureChildScope($Validator) { & $Validator $snapshotPath $build $msvc | Out-Null }
Invoke-FixtureChildScope $validateBeforeTests
Check ($preTestCapture.Count -eq 3 -and $preTestCapture.file.sha256 -ceq (Hash $snapshotPath)) 'production-pre-test-callback-consumes-real-snapshot'
$lower=[Management.Automation.Language.Parser]::ParseFile((Join-Path $Project 'scripts/build-sdk.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'SDK 基础构建入口语法错误。' }
$text=$lower.Extent.Text
Check ($text.Contains('-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')) 'compile-commands-actually-requested'
Check ($text.IndexOf('Write-AwjSdkPreTestSnapshot -RepositoryRoot') -lt $text.IndexOf('& $PreTestValidator')) 'snapshot-before-validator'
Check ($text.IndexOf('& $PreTestValidator') -lt $text.IndexOf('& $CTestPath --test-dir $build --output-on-failure')) 'validator-before-any-test-execution'
$nativeInvocation=$ast.Find({param($n) $n -is [Management.Automation.Language.CommandAst] -and $n.Extent.Text.Contains("'build-sdk.ps1'")}, $true)
Check ($nativeInvocation.Extent.Text.Contains('-PreTestSnapshotPath $preTestPath -PreTestValidator $validateBeforeTests')) 'formal-builder-cannot-ignore-pre-test-snapshot'
# SourceState 的 Git 传输在此单元边界合成；不创建 clone、提交、tag 或来源证明。
$script:mockStatus=''; $script:mockTag='a' * 40; $script:mockIndexFlag='H'; $script:mockActualBlob='c' * 40
function Read-SdkReleaseGit([string[]]$Arguments) {
    switch ($Arguments[0]) {
        'rev-parse' {
            if ($Arguments[1] -ceq '--show-toplevel') { return $root }
            if ($Arguments[1] -ceq 'HEAD') { return ('a' * 40) }
            if ($Arguments[1].StartsWith('HEAD:', [StringComparison]::Ordinal)) { return ('c' * 40) }
            return $script:mockTag
        }
        'status' { return $script:mockStatus }
        'submodule' { return '' }
        'ls-files' {
            if ('-v' -cin $Arguments) { return "$($script:mockIndexFlag) VERSION$([char]0)H sdk/vcpkg.json$([char]0)" }
            return "VERSION$([char]0)sdk/vcpkg.json$([char]0)"
        }
        '-c' {
            if ('hash-object' -cnotin $Arguments) { throw 'fixture 收到未知 Git 覆盖请求' }
            return $script:mockActualBlob
        }
        default { throw 'fixture 收到未知 Git 请求' }
    }
}
function Get-AwjSourceFingerprint { param([string]$RepositoryRoot) return ('b' * 64) }
$null=Get-SdkReleaseSourceState; Check $true 'source-state-clean-exact-tag-contract'
foreach ($flag in @('h','S','s')) {
    $script:mockIndexFlag=$flag
    Must-Reject { Get-SdkReleaseSourceState } "hidden-index-flags-$flag-cannot-hide-source-changes"
}
$script:mockIndexFlag='H'; $script:mockActualBlob='d' * 40
Must-Reject { Get-SdkReleaseSourceState } 'actual-source-must-match-tagged-git-blob'
$script:mockActualBlob='c' * 40
$script:mockStatus='?? unknown-source.cpp'
Must-Reject { Get-SdkReleaseSourceState } 'unknown-untracked-input-blocks-formal-build'
$script:mockStatus=''; $script:mockTag='c' * 40
Must-Reject { Get-SdkReleaseSourceState } 'cannot-retag-old-library-source'
$savedEnvironment=[Environment]::GetEnvironmentVariable('CL','Process')
try { $env:CL='/Od'; Must-Reject { Assert-SdkReleaseEnvironment } 'implicit-cl-flags-rejected' }
finally { [Environment]::SetEnvironmentVariable('CL',$savedEnvironment,'Process') }
# 执行生产生成语句、真实 CMake 审核、原样 Tee 管道/退出守卫和证据采集。
# 安装树/JUnit 仍是合成夹具，不编译/执行 SDK，不签发来源证明。
$releaseEntry=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq 'Invoke-SdkReleaseBuild'}, $true)
$statements=@($releaseEntry.Body.EndBlock.Statements)
$auditIndex=-1
for ($index=0; $index -lt $statements.Count; ++$index) {
    if ($statements[$index].Extent.Text.StartsWith('[IO.File]::WriteAllText($auditPath,', [StringComparison]::Ordinal)) { $auditIndex=$index; break }
}
if ($auditIndex -lt 0 -or $auditIndex + 3 -ge $statements.Count -or
    $statements[$auditIndex + 1] -isnot [Management.Automation.Language.PipelineAst] -or
    $statements[$auditIndex + 2] -isnot [Management.Automation.Language.IfStatementAst] -or
    $statements[$auditIndex + 3] -isnot [Management.Automation.Language.AssignmentStatementAst] -or
    $statements[$auditIndex + 3].Left.Extent.Text -cne '$evidenceInputs') { throw '无法提取连续的生产审核/退出守卫/封存边界。' }
$auditText=($statements[$auditIndex..($auditIndex + 3)] | ForEach-Object { $_.Extent.Text }) -join "`n"
$auditScript=[scriptblock]::Create($auditText)
$auditRoot=Join-Path $output 'native-cmake-audit-fixtures'
Save-Text (Join-Path $auditRoot 'production-audit-fragment.ps1') $auditText
$realCMake=Resolve-SdkReleaseTool $CMakePath
$fileReader=(Get-Command Get-SdkReleaseFile).ScriptBlock
function Invoke-AuditFixture([string]$Case, [scriptblock]$FileReader) {
    $root=$Project
    $output=Join-Path $auditRoot $Case
    $build=Join-Path $output 'build'; $dependencies=Join-Path $output 'dependencies'
    $junit=Join-Path $output 'sdk-tests.xml'; $preTestPath=Join-Path $output 'pre-test-snapshot.json'
    $install=Join-Path $output 'install'; $auditPath=Join-Path $output 'audit-sdk-build.cmake'
    $cmake=$realCMake; $requiredTests=@(Get-SdkReleaseRequiredTests)
    foreach ($relative in @('CMakeCache.txt','compile_commands.json','.ninja_log','build.ninja','CMakeFiles/rules.ninja',
            'sdk-validation-targets.dot','sdk-validation-commands.txt','sdk-ctest-plan.json','awj-msvc-awj_sdk-Release.txt','msvc-build-profile.json')) {
        Save-Text (Join-Path $build $relative) "metadata-fixture-only:$relative"
    }
    foreach ($relative in @('pre-test-snapshot.json','ffmpeg-header-inputs.json','vcpkg-script-inputs.json','dependency-metadata.json',
            'dependencies/installed/vcpkg/status','build-and-tests.log','install.log')) {
        Save-Text (Join-Path $output $relative) "metadata-fixture-only:$relative"
    }
    foreach ($relative in @('bin/awj_sdk.dll','bin/awj-stream.exe','include/awj/awj.h','include/awj/awj_shm.h','include/awj/awj_ffmpeg.h','include/awj/awj_d3d11.h',
            'lib/cmake/AWJimage/AWJimageConfig.cmake','share/AWJimage/LICENSE','share/AWJimage/THIRD_PARTY_NOTICES.txt',
            'share/AWJimage/sdk.md','share/AWJimage/examples/basic/CMakeLists.txt')) {
        if ($Case -ceq 'install-missing-header' -and $relative -ceq 'include/awj/awj_d3d11.h') { continue }
        Save-Text (Join-Path $install $relative) "metadata-fixture-only-never-execute:$relative"
    }
    $xml="<testsuite name=`"FIXTURE_ONLY`" tests=`"$($requiredTests.Count)`" failures=`"0`" disabled=`"0`" skipped=`"0`">`n"
    foreach ($name in $requiredTests) {
        $content=if ($Case -ceq 'junit-hidden-skip' -and $name -ceq $requiredTests[0]) { '<skipped/>' } else { '<system-out>fixture only</system-out>' }
        $xml+="<testcase name=`"$name`" status=`"run`">$content</testcase>`n"
    }
    Save-Text $junit ($xml + '</testsuite>')
    $log=Join-Path $output 'audit-sdk-build.log'
    Check (-not (Test-Path -LiteralPath $log)) "$Case-audit-log-starts-absent"
    $capture=@{evidence_calls=0;inputs=@()}
    # 只观察真实采集函数是否被调用；所有普通文件/摘要校验仍由生产函数完成。
    function Get-SdkReleaseFile([string]$Path) { ++$capture.evidence_calls; & $FileReader $Path }
    $failure=''
    try { . $auditScript; $capture.inputs=@($evidenceInputs) } catch { $failure=$_.Exception.Message }
    $nativeExit=$LASTEXITCODE
    Check (Test-Path -LiteralPath $log -PathType Leaf) "$Case-real-cmake-tee-creates-log"
    $logText=[IO.File]::ReadAllText($log)
    $markers=@([regex]::Matches($logText, '(?m)^-- AWJ_SDK_BUILD_AUDIT_PASSED\r?$')).Count
    if ($Case -ceq 'success') {
        Check ($nativeExit -eq 0 -and -not $failure) 'production-audit-and-exit-guard-succeed'
        Check ($markers -eq 1) 'success-marker-after-both-real-cmake-validators'
        $collected=@($capture.inputs | Where-Object path -ceq $log)
        Check ($capture.evidence_calls -gt 0 -and $collected.Count -eq 1 -and $collected[0].size -gt 0 -and
            $collected[0].sha256 -ceq (Hash $log)) 'production-sealing-collects-nonempty-audit-log'
    } else {
        Check ($nativeExit -ne 0 -and $failure -ceq '正式 SDK JUnit 或安装树未通过既有正式规则。') "$Case-production-native-exit-guard-rejects"
        Check ($markers -eq 0) "$Case-no-success-marker"
        Check ($capture.evidence_calls -eq 0 -and $capture.inputs.Count -eq 0) "$Case-never-enters-production-sealing"
        if ($Case -ceq 'junit-hidden-skip') {
            Check ($logText -match 'SDK JUnit' -and $logText -match '跳过') 'junit-fails-in-real-validator'
        } else {
            Check ($logText -match '完整 SDK 安装缺少' -and $logText -match 'awj_d3d11.h') 'install-fails-in-real-validator'
        }
    }
    Check (-not (Test-Path -LiteralPath (Join-Path $output 'build-evidence.json'))) "$Case-no-sdk-proof-issued"
    Save-Json ([ordered]@{fixture_only=$true;case=$Case;native_exit=$nativeExit;guard_error=$failure;success_markers=$markers
        evidence_calls=$capture.evidence_calls;audit_log=(& $FileReader $log);target_execution='not-run';proof='not-issued'}) (Join-Path $output 'case-checks.json')
}
foreach ($case in @('success','junit-hidden-skip','install-missing-header')) { Invoke-AuditFixture $case $fileReader }
$sourceHashes=[ordered]@{}
foreach ($relative in @('scripts/build-sdk-release.ps1','tests/windows_sdk_release_builder_contract_tests.ps1','cmake/sdk-release-profiles.cmake','VERSION')) {
    $sourceHashes[$relative]=Hash (Join-Path $Project $relative)
}
[ordered]@{fixture_only=$true;fixture_root=$output;scope='windows-fresh-sdk-builder-contract';checks=$script:checks.ToArray()
    native_cmake=$realCMake;powershell_version=$PSVersionTable.PSVersion.ToString();source_sha256=$sourceHashes
    native_build='not-run';sdk_tests='not-run';packaging='not-run';source_clone='not-created';git_commit='not-run';git_tag='not-run'} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding utf8
"$($script:checks.Count) 个 Windows builder 契约检查通过（含真实 CMake 审核/管道）；未运行真实编译、SDK 测试、打包或 tag。结果：$output/summary.json"
