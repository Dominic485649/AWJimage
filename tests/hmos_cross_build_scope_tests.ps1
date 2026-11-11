param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $Project 'scripts/release-safety.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $Project 'scripts/windows-msvc.psm1') -Force
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $Project "build/release-contract-tests/hmos-cross-build-$([Guid]::NewGuid().ToString('N'))" }
$Repo = Assert-AwjBuildPath -RepositoryRoot $Project -Path $OutputDirectory
$allowed = [IO.Path]::GetFullPath((Join-Path $Project 'build/release-contract-tests')) + [IO.Path]::DirectorySeparatorChar
if (-not $Repo.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $Repo)) {
    throw '夹具必须使用本项目 build/release-contract-tests 下的全新目录。'
}
[void][IO.Directory]::CreateDirectory($Repo)
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Project 'scripts/package-release.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw '发行脚本语法无效。' }
# 只装载真实校验/复制函数；不运行打包主流程、签名或任何目标程序。
foreach ($name in @('ConvertTo-AwjReleaseVersion','Test-AwjHmosCrossBuildPolicy','Read-AwjReleaseJsonSnapshot','Get-RepoPath','Assert-File',
        'Assert-AwjHmosNoRuntimeEvidence','Assert-AwjHmosVerification','Assert-AwjHmosCrossBuildAttachments',
        'Get-AwjHmosReleaseScope','Get-AwjAdditionalAssetBindings','Assert-AwjAdditionalAssetCopies',
        'Copy-AwjAdditionalReleaseAssets','Assert-AdditionalReleaseAssets')) {
    $node = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name }, $true)
    if (-not $node) { throw "缺少生产校验函数：$name" }
    . ([scriptblock]::Create($node.Extent.Text))
}
$GitHead = 'c' * 40; $SourceFingerprint = 'a' * 64
$Version = '1.3.5'; $LocalCandidate = $false; $HmosSdkPreview = $false
$script:checks = [Collections.Generic.List[object]]::new()
function Check([bool]$Condition, [string]$Label) {
    if (-not $Condition) { throw "夹具失败：$Label" }
    $script:checks.Add([ordered]@{name=$Label;result='passed'})
}
function Must-Reject([scriptblock]$Action, [string]$Label) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    if (-not $message) { throw "应拒绝但接受了：$Label" }
    if ($message -match 'not recognized|CommandNotFound|找不到.*命令|缺少生产校验函数') { throw "夹具依赖错误：$message" }
    $script:checks.Add([ordered]@{name=$Label;result='rejected';reason=$message})
}
function Save-Json($Value, [string]$Path) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 24), [Text.UTF8Encoding]::new($false))
}
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function New-MsvcEvidence {
    @{compiler_id='MSVC';compiler_version='19.51.36260';compiler_sha256=('a' * 64);linker_version='14.51.36260.0';linker_sha256=('b' * 64)
        flags_source='cmake-generated-target-profile';targets=@(@{name='awj_sdk';configuration='Release';runtime='MultiThreaded';ipo=$true
            minimum_cpu='x64';profile_sha256=('c' * 64);compile_flags=@('/O2','/Ob2','/Oi','/Ot','/Gy','/Gw','/GF','/GL','/MT')
            link_flags=@('/LTCG','/OPT:REF','/OPT:ICF','/INCREMENTAL:NO')})}
}
function Save-CrossChain($State) {
    # 语义负例也重算引用摘要，不能仅靠摘要不匹配碰巧拒绝。
    Save-Json $State.build $State.buildPath
    $State.consumers.build_evidence_sha256 = Hash $State.buildPath
    Save-Json $State.consumers $State.consumerPath
    $State.receipt.build_evidence_sha256 = Hash $State.buildPath
    $State.receipt.cross_build_report_sha256 = Hash $State.buildPath
    $State.receipt.cross_link_consumer_report_sha256 = Hash $State.consumerPath
    Save-Json $State.receipt $State.paths[2]
    foreach ($index in @(0,1,3)) { Save-Json $State.other[$index.ToString()] $State.paths[$index] }
}
function New-CrossFixture([string]$FixtureVersion) {
    $directory = Join-Path $Repo "version-$FixtureVersion"
    [void][IO.Directory]::CreateDirectory($directory)
    $paths = @(); $other = @{}; $receipt = $null
    $specs = @(
        @{platform='windows-x64';kind='sdk';name='AWJ_SDK_Win_x64.zip';profile='windows-x64-msvc';compiler='19.51.36260';triple='x86_64-pc-windows-msvc'},
        @{platform='linux-x64';kind='sdk';name='AWJ_SDK_Linux_x64.tar.gz';profile='linux-x64-clang23';compiler='23.1.5';triple='x86_64-pc-linux-gnu'},
        @{platform='hmos-arm64-api26';kind='sdk';name='AWJ_SDK_HMOS_arm64_api26.tar.gz';profile='hmos-arm64-api26-clang23';compiler='23.1.5';triple='aarch64-unknown-linux-ohos'},
        @{platform='source';kind='source';name="AWJimage-$FixtureVersion-source.zip"})
    for ($index = 0; $index -lt $specs.Count; ++$index) {
        $spec = $specs[$index]
        $assetDirectory = Join-Path $directory $spec.platform
        [void][IO.Directory]::CreateDirectory($assetDirectory)
        $archive = Join-Path $assetDirectory $spec.name
        [IO.File]::WriteAllText($archive, "metadata-fixture-only:$($spec.name)", [Text.UTF8Encoding]::new($false))
        $proof = @{schema=1;kind=$spec.kind;mode='formal';status='passed';version=$FixtureVersion;platform=$spec.platform
            source_dirty=$false;git_head=$GitHead;source_fingerprint=$SourceFingerprint
            archive=@{name=$spec.name;size=(Get-Item -LiteralPath $archive).Length;sha256=(Hash $archive)}}
        if ($spec.kind -ceq 'sdk') {
            $proof.profile=$spec.profile; $proof.compiler_version=$spec.compiler; $proof.target_triple=$spec.triple
            $proof.test_report_sha256='b' * 64; $proof.consumer_report_sha256='d' * 64
            $proof.verification=@{sdk_tests='passed';installed_consumers='passed';exports='passed';archive_roundtrip='passed'
                compiler_identity='passed';target_identity='passed';relocated_stream='passed'}
            if ($index -eq 0) { $proof.toolchain=New-MsvcEvidence; $proof.verification.msvc_release_optimization='passed' }
        } else { $proof.verification=@{source_commit='passed';source_layout='passed'} }
        $path = Join-Path $assetDirectory 'asset-evidence.json'
        $paths += $path
        if ($index -eq 2) { $receipt=$proof } else { $other[$index.ToString()]=$proof }
    }
    foreach ($key in @('test_report_sha256','consumer_report_sha256')) { $receipt.Remove($key) }
    $receipt.status='cross-linked'; $receipt.validation_tier='cross-build'; $receipt.target_execution='not-run'; $receipt.target_loader='not-run'
    $receipt.abi=1; $receipt.api=26; $receipt.native_sdk_version='26.0.0.105'; $receipt.libcxx_version=15004; $receipt.libcxx_abi_version=1
    $receipt.libcxx_abi_namespace='__n1'; $receipt.runtime_linkage='native-static'; $receipt.library_sha256='e' * 64; $receipt.validation_notice_sha256='f' * 64
    $receipt.verification=@{cross_build='passed';test_programs_linked='passed';installed_consumers_linked='passed';compiler_identity='passed'
        target_identity='passed';native_inputs='passed';exports='passed';archive_roundtrip='passed';validation_notice='passed';target_execution='not-run';target_loader='not-run'}
    $build=@{}; $consumers=@{}
    foreach ($key in @('schema','mode','status','validation_tier','target_execution','target_loader','profile','platform','target_triple','abi',
            'version','git_head','source_fingerprint','library_sha256')) { $build[$key]=$receipt[$key]; $consumers[$key]=$receipt[$key] }
    $build.kind='hmos-cross-build'; $build.source_dirty=$false
    foreach ($key in @('compiler_version','api','native_sdk_version','libcxx_version','libcxx_abi_version','libcxx_abi_namespace','runtime_linkage')) { $build[$key]=$receipt[$key] }
    $build.verification=@{cross_build='passed';test_programs_linked='passed';compiler_identity='passed';target_identity='passed';native_inputs='passed';target_execution='not-run';target_loader='not-run'}
    $build.test_programs=@{}
    foreach ($name in @('awj_sdk_tests','awj_sdk_c_consumer','awj_sdk_encode_tests','awj_sdk_pixel_tests','awj_sdk_yuv_readonly_tests','awj_sdk_stdlib_tests','awj_sdk_worker_failure_tests')) {
        $importsSdk=$name -cin @('awj_sdk_tests','awj_sdk_c_consumer','awj_sdk_encode_tests','awj_sdk_yuv_readonly_tests')
        $program=@{sha256=('1' * 64);profile_sha256=('2' * 64);link_map_sha256=('3' * 64);link_inputs_sha256=('4' * 64);execution='not-run'
            role=$(if ($importsSdk) { 'sdk-consumer' } else { 'standalone-helper' })}
        if ($importsSdk) { $program.sdk_library_sha256=$receipt.library_sha256 }
        $build.test_programs[$name]=$program
    }
    $consumers.kind='hmos-cross-link-consumers'; $consumers.verification=@{installed_consumers_linked='passed';sdk_library_binding='passed';target_execution='not-run';target_loader='not-run'}
    $consumers.consumers=@{}
    foreach ($spec in @(@{name='installed_c_consumer';standard='c99'},@{name='installed_cxx_consumer';standard='c++11'},@{name='installed_cxx17_consumer';standard='c++17'})) {
        $consumers.consumers[$spec.name]=@{standard=$spec.standard;sha256=('5' * 64);sdk_library_sha256=$receipt.library_sha256}
    }
    $state=@{paths=$paths;other=$other;receipt=$receipt;build=$build;consumers=$consumers
        buildPath=(Join-Path (Split-Path $paths[2] -Parent) 'hmos-build.json');consumerPath=(Join-Path (Split-Path $paths[2] -Parent) 'hmos-cross-link-consumers.json')}
    Save-CrossChain $state
    return $state
}
foreach ($versionCase in @('1.3.4','1.3.5','1.3.6','1.10.0','2.0.0','2147483647.2147483647.2147483647')) {
    $expected=(ConvertTo-AwjReleaseVersion $versionCase) -ge [version]'1.3.5'
    Check ((Test-AwjHmosCrossBuildPolicy $versionCase 'hmos-arm64-api26-clang23') -eq $expected) "policy-$versionCase"
    foreach ($profile in @('windows-x64-msvc','linux-x64-clang23')) { Check (-not (Test-AwjHmosCrossBuildPolicy $versionCase $profile)) "desktop-policy-$versionCase-$profile" }
}
foreach ($invalid in @('','1.3','1.3.5.0','01.3.5','1.03.5','1.3.05','1.3.5-rc1','1.3.5+build',' 1.3.5','1.3.5 ','-1.3.5','2147483648.0.0','0.2147483648.0','0.0.2147483648','99999999999999999999.0.0')) {
    Must-Reject { Test-AwjHmosCrossBuildPolicy $invalid 'hmos-arm64-api26-clang23' } "invalid-version-$invalid"
}
foreach ($unknown in @('','hmos-arm64-api26','HMOS-arm64-api26-clang23','hmos-arm64-api26-clang24','windows-x64-clang21')) {
    Must-Reject { Test-AwjHmosCrossBuildPolicy '1.3.5' $unknown } "unknown-profile-$unknown"
}
foreach ($Version in @('1.3.5','1.3.6','1.10.0','2.0.0')) {
    $state=New-CrossFixture $Version
    $assets=@(Assert-AdditionalReleaseAssets $state.paths)
    Check ($assets.Count -eq 4) "four-roles-$Version"
    Check ((Get-AwjHmosReleaseScope $assets) -ceq 'cross-build') "actual-scope-$Version"
    Check (@($assets | Where-Object role -ceq 'sdk:hmos-arm64-api26').Count -eq 1) "formal-hmos-role-$Version"
}
$Version='1.3.5'; $state=New-CrossFixture '1.3.5'
$baseline=$state | ConvertTo-Json -Depth 24
$readerPath=Join-Path $Repo 'bound-json-reader.json'
$readerText='{"fixture_only":true,"value":"validated-bytes"}'
[IO.File]::WriteAllText($readerPath,$readerText,[Text.UTF8Encoding]::new($false))
$readerSnapshot=Read-AwjReleaseJsonSnapshot $readerPath
$readerHash=Hash $readerPath
[IO.File]::WriteAllText($readerPath,'{"fixture_only":true,"value":"replaced-bytes"}',[Text.UTF8Encoding]::new($false))
Check ($readerSnapshot.record.value -ceq 'validated-bytes' -and $readerSnapshot.sha256 -ceq $readerHash -and $readerSnapshot.sha256 -cne (Hash $readerPath)) 'json-semantics-and-digest-share-one-byte-buffer'
function Reject-Mutation([scriptblock]$Mutate, [string]$Label) {
    $state=$baseline | ConvertFrom-Json -AsHashtable
    . $Mutate
    Save-CrossChain $state
    Must-Reject { Assert-AdditionalReleaseAssets $state.paths } $Label
}
foreach ($field in @('mode','status','validation_tier','profile','target_execution','target_loader','native_sdk_version','libcxx_abi_namespace','runtime_linkage')) {
    Reject-Mutation { $state.receipt[$field]='mismatch' } "receipt-identity-$field"
}
foreach ($field in @('api','libcxx_version','libcxx_abi_version','abi')) {
    Reject-Mutation { $state.receipt[$field]=@($state.receipt[$field]) } "receipt-nonscalar-$field"
}
foreach ($stage in @('build','consumers')) {
    foreach ($field in @('kind','mode','status','validation_tier','profile','platform','target_triple','version','git_head','source_fingerprint','library_sha256')) {
        Reject-Mutation { $state[$stage][$field]='mismatch' } "attachment-$stage-$field"
    }
    Reject-Mutation { $state[$stage].abi='1' } "attachment-$stage-abi-type"
    Reject-Mutation { $state[$stage].verification.extra='passed' } "attachment-$stage-extra-verification"
    foreach ($field in @('target_execution','target_loader')) {
        Reject-Mutation { $state[$stage].verification[$field]='passed' } "attachment-$stage-runtime-$field"
    }
}
foreach ($field in @('cross_build','test_programs_linked','installed_consumers_linked','compiler_identity','target_identity','native_inputs','exports','archive_roundtrip','validation_notice')) {
    Reject-Mutation { $state.receipt.verification.Remove($field) } "asset-missing-$field"
    Reject-Mutation { $state.receipt.verification[$field]=@('passed') } "asset-array-$field"
}
Reject-Mutation { $state.receipt.verification.extra='passed' } 'asset-extra-verification'
Reject-Mutation { $state.receipt['Keys']=@(); $state.receipt['sdk_tests']='passed' } 'json-keys-cannot-hide-runtime-claim'
Reject-Mutation { $state.receipt.verification['Count']=11 } 'json-count-cannot-hide-asset-verification-extra'
Reject-Mutation { $state.build.verification['Count']=7 } 'json-count-cannot-hide-build-verification-extra'
Reject-Mutation { $state.consumers.verification['Count']=4 } 'json-count-cannot-hide-consumer-verification-extra'
Reject-Mutation { $state.build.test_programs['Count']=7 } 'json-count-cannot-hide-extra-program'
Reject-Mutation { $state.consumers.consumers['Count']=3 } 'json-count-cannot-hide-extra-consumer'
Reject-Mutation { $state.build.test_programs.Remove('awj_sdk_tests') } 'missing-target-program'
Reject-Mutation { $state.build.test_programs.unknown=@{sha256=('a' * 64)} } 'unknown-target-program'
Reject-Mutation { $state.build.test_programs.awj_sdk_tests.sdk_library_sha256='0' * 64 } 'target-program-wrong-sdk'
Reject-Mutation { $state.build.test_programs.awj_sdk_tests.execution='passed' } 'target-program-runtime-claim'
Reject-Mutation { $state.build.test_programs.awj_sdk_stdlib_tests.role='sdk-consumer' } 'target-program-role'
Reject-Mutation { $state.consumers.consumers.Remove('installed_cxx17_consumer') } 'missing-cxx17-consumer'
Reject-Mutation { $state.consumers.consumers.extra=@{} } 'unknown-consumer'
Reject-Mutation { $state.consumers.consumers.installed_c_consumer.standard='c++11' } 'wrong-consumer-standard'
Reject-Mutation { $state.consumers.consumers.installed_cxx_consumer.sdk_library_sha256='0' * 64 } 'consumer-wrong-library'
Reject-Mutation { $state.consumers.consumers.installed_cxx17_consumer.sha256=@('5' * 64) } 'consumer-hash-array'
foreach ($stage in @('receipt','build','consumers')) {
    foreach ($field in @('test_report_sha256','sdk_test_report','consumer_report_path','target_execution_report','target_loader_report_sha256','target_runner','runner','sdk_test_log','consumer_log_path','loader_log','sdk_tests','installed_consumers','compiled_source_verified','passed','run_junit','exit_code','consumer_exit_code','execution_summary')) {
        foreach ($value in @($null,'')) {
            Reject-Mutation { $state[$stage].forged=@(@{nested=@{$field=$value}}) } "runtime-field-$stage-$field-$($null -eq $value)"
        }
    }
}
foreach ($index in @(0,1)) {
    Reject-Mutation { $state.other[$index.ToString()].Remove('test_report_sha256') } "desktop-missing-junit-$index"
    Reject-Mutation { $state.other[$index.ToString()].verification.installed_consumers='not-run' } "desktop-consumers-not-run-$index"
    Reject-Mutation { $state.other[$index.ToString()].verification.relocated_stream='not-run' } "desktop-stream-not-run-$index"
}
Reject-Mutation { $state.other['0'].toolchain.targets[0].compile_flags+= '/arch:AVX2'; $state.other['0'].toolchain.targets[0].ipo=$false } 'desktop-msvc-lto-still-required'
Reject-Mutation { $state.other['0'].toolchain.targets[0].compile_flags+= '/arch:AVX2' } 'desktop-sdk-x64-rejects-avx2-even-with-valid-ipo'
Reject-Mutation { $state.other['0'].toolchain.targets[0].compile_flags+= '-arch:AVX' } 'desktop-sdk-x64-rejects-dash-arch-upgrade'
$state=$baseline | ConvertFrom-Json -AsHashtable; Save-CrossChain $state
$state.consumers.build_evidence_sha256='0' * 64; Save-Json $state.consumers $state.consumerPath
$state.receipt.cross_link_consumer_report_sha256=Hash $state.consumerPath; Save-Json $state.receipt $state.paths[2]
Must-Reject { Assert-AdditionalReleaseAssets $state.paths } 'consumer-chain-does-not-bind-B'
$state=$baseline | ConvertFrom-Json -AsHashtable; Save-CrossChain $state
$state.receipt.build_evidence_sha256='0' * 64; Save-Json $state.receipt $state.paths[2]
Must-Reject { Assert-AdditionalReleaseAssets $state.paths } 'receipt-build-hash-disagrees'
$state=$baseline | ConvertFrom-Json -AsHashtable; Save-CrossChain $state
[IO.File]::AppendAllText($state.buildPath, "`n")
Must-Reject { Assert-AdditionalReleaseAssets $state.paths } 'actual-B-bytes-changed'
Save-CrossChain $state
[IO.File]::AppendAllText($state.consumerPath, "`n")
Must-Reject { Assert-AdditionalReleaseAssets $state.paths } 'actual-C-bytes-changed'
Save-CrossChain $state
foreach ($path in @($state.buildPath,$state.consumerPath)) {
    Move-Item -LiteralPath $path -Destination "$path.saved"
    try { Must-Reject { Assert-AdditionalReleaseAssets $state.paths } "fixed-attachment-missing-$([IO.Path]::GetFileName($path))" }
    finally { Move-Item -LiteralPath "$path.saved" -Destination $path }
}
Must-Reject { Assert-AdditionalReleaseAssets @($state.paths[0],$state.paths[1],$state.paths[2],$state.paths[2]) } 'duplicate-hmos-role'
$HmosSdkPreview=$true
Must-Reject { Assert-AdditionalReleaseAssets $state.paths } 'new-version-cannot-use-preview-flag'
$HmosSdkPreview=$false
$LocalCandidate=$true
Check (@(Assert-AdditionalReleaseAssets @()).Count -eq 0) 'candidate-no-additional-assets'
Check ((Get-AwjHmosReleaseScope @()) -ceq 'not-included') 'candidate-hmos-not-included'
$LocalCandidate=$false
$state=$baseline | ConvertFrom-Json -AsHashtable; Save-CrossChain $state
$sourceReceipt=$state.paths[3]
$sourceRecord=$state.other['3']
$sourceArchive=Join-Path (Split-Path $sourceReceipt -Parent) $sourceRecord.archive.name
$validatedReceiptHash=Hash $sourceReceipt
$changedSourceRecord=$sourceRecord | ConvertTo-Json -Depth 24 | ConvertFrom-Json -AsHashtable
$changedSourceRecord.status='unvalidated-replacement'
$replacementJson=$changedSourceRecord | ConvertTo-Json -Depth 24
$script:receiptSwapTriggered=$false
$script:receiptSwapTargetReceipt=$sourceReceipt
$script:receiptSwapTargetArchive=$sourceArchive
$script:receiptSwapReplacementJson=$replacementJson
function Invoke-ReceiptSwapDuringArchiveHash($Paths) {
    function Get-FileHash {
        param([string]$LiteralPath,[string]$Algorithm)
        # 显式脚本作用域避免生产函数中的 $archive/$receipt 遮蔽模拟目标。
        if (-not $script:receiptSwapTriggered -and $LiteralPath -ceq $script:receiptSwapTargetArchive) {
            [IO.File]::WriteAllText($script:receiptSwapTargetReceipt,$script:receiptSwapReplacementJson,[Text.UTF8Encoding]::new($false))
            $script:receiptSwapTriggered=$true
        }
        Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath $LiteralPath -Algorithm $Algorithm
    }
    @(Assert-AdditionalReleaseAssets $Paths)
}
$racedAssets=@(Invoke-ReceiptSwapDuringArchiveHash $state.paths)
$racedSource=@($racedAssets | Where-Object role -ceq 'source:source')
Check ($script:receiptSwapTriggered -and $racedSource.Count -eq 1 -and $racedSource[0].receipt_sha256 -ceq $validatedReceiptHash -and $validatedReceiptHash -cne (Hash $sourceReceipt)) 'receipt-seal-keeps-the-validated-buffer-hash-during-replacement'
$raceStage=Join-Path $Repo 'receipt-race-sealed';[void][IO.Directory]::CreateDirectory((Join-Path $raceStage 'verification'))
Must-Reject { Copy-AwjAdditionalReleaseAssets $racedAssets $raceStage } 'unvalidated-receipt-replacement-cannot-be-sealed'
Save-CrossChain $state
$assets=@(Assert-AdditionalReleaseAssets $state.paths)
$stage=Join-Path $Repo 'sealed'; [void][IO.Directory]::CreateDirectory((Join-Path $stage 'verification'))
$verificationFiles=@(Copy-AwjAdditionalReleaseAssets $assets $stage)
Check ($verificationFiles.Count -eq 6) 'four-receipts-plus-two-private-attachments'
Check (@($verificationFiles | Where-Object { $_ -cin @('verification/hmos-build.json','verification/hmos-cross-link-consumers.json') }).Count -eq 2) 'fixed-attachments-in-verification'
Assert-AwjAdditionalAssetCopies $assets $stage
[IO.File]::AppendAllText((Join-Path $stage 'verification/hmos-build.json'), 'changed-after-copy')
Must-Reject { Assert-AwjAdditionalAssetCopies $assets $stage } 'sealed-attachment-substitution'
$acceptanceInputs = $ast.Find({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -ceq '$Artifacts' }, $true)
Check ($acceptanceInputs.Right.Extent.Text.Contains('$AdditionalVerificationFiles')) 'private-attachments-in-acceptance-inputs'
[ordered]@{fixture_only=$true;fixture_root=$Repo;scope='formal-hmos-cross-build-policy-and-attachment-contract';checks=$script:checks.ToArray()
    device_execution='not-run';sdk_build='not-run';packaging='not-run';signing='not-run'} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Repo 'summary.json') -Encoding utf8
"$($script:checks.Count) 个 HMOS 元数据契约检查通过；仅 fixture，不是构建、设备运行或正式发行验收。结果：$Repo/summary.json"
