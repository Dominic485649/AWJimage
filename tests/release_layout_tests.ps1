$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $Project 'scripts/release-safety.psm1') -Force
Import-Module (Join-Path $Project 'scripts/windows-msvc.psm1') -Force
$Repo = Join-Path $Project "build/update-1.2.3-work/release-layout-tests-$([Guid]::NewGuid().ToString('N'))"
foreach ($Name in @('cmake', 'build/windows', 'build/linux', 'bin/x64/Release')) {
    [void][IO.Directory]::CreateDirectory((Join-Path $Repo $Name))
}
[IO.File]::WriteAllText((Join-Path $Repo 'cmake/codec-dependencies.json'), "{`"awj_version`":`"1.2.3`"}`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $Repo 'bin/x64/Release/sentinel'), 'unchanged', [Text.UTF8Encoding]::new($false))
$Version = '1.2.3'
$GitHead = 'c' * 40
$SourceFingerprint = 'a' * 64
$LocalCandidate = $true
$HmosSdkPreview = $false
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Project 'scripts/package-release.ps1'), [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw '跨平台打包脚本语法错误。' }
# 仅加载校验函数。不会执行完整打包、启动假二进制或访问签名密钥。
foreach ($Name in @('Get-RepoPath', 'Assert-File', 'Assert-Directory', 'Get-NormalizedLockHash', 'Assert-BuildEvidence', 'Assert-VerificationReport', 'Assert-ArchiveMemberList', 'Assert-ExactPackage', 'Assert-AdditionalReleaseAssets')) {
    $Node = $Ast.Find({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq $Name }, $true)
    if (-not $Node) { throw "缺少校验函数：$Name" }
    . ([scriptblock]::Create($Node.Extent.Text))
}
function Must-Reject([scriptblock]$Action, [string]$Label) {
    $Rejected = $false
    try { & $Action | Out-Null } catch { $Rejected = $true }
    if (-not $Rejected) { throw "应拒绝但接受了：$Label" }
}
function Save-Proof($Proof, [string]$Path) { $Proof | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $Path -Encoding utf8 }
function New-MsvcFixtureEvidence([string[]]$Targets, [string]$Cpu) {
    # 纯契约夹具，不代表编译、LTO 或真实目标执行。
    @{
        compiler_id='MSVC'; compiler_version='19.51.36260'; compiler_sha256=('a' * 64)
        linker_version='14.51.36260.0'; linker_sha256=('b' * 64); flags_source='cmake-generated-target-profile'
        targets=@(foreach ($Target in $Targets) {
            $Flags=@('/O2','/Ob2','/Oi','/Ot','/Gy','/Gw','/GF','/GL','/MT')
            if ($Cpu -ceq 'AVX2') { $Flags+='/arch:AVX2' }
            @{name=$Target;configuration='Release';runtime='MultiThreaded';ipo=$true;minimum_cpu=$Cpu
                profile_sha256=('c' * 64);compile_flags=$Flags;link_flags=@('/LTCG','/OPT:REF','/OPT:ICF','/INCREMENTAL:NO')}
        })
    }
}
$ModeGate=@($Ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -ceq '$LocalCandidate' })
if ($ModeGate.Count -ne 1) { throw '本地／正式模式必须具有唯一顶层边界。' }
$LocalCandidate=$false; $SkipManifests=$true; $TrackedChanges=@(); $UntrackedSources=@()
Must-Reject { . ([scriptblock]::Create($ModeGate[0].Extent.Text)) } '正式模式不可 SkipManifests'
$SkipManifests=$false; $TrackedChanges=@(' M src/example.cpp')
Must-Reject { . ([scriptblock]::Create($ModeGate[0].Extent.Text)) } '正式模式不可 dirty'
$LocalCandidate=$true; $Channel='prerelease'; $ArchiveManifestSequence=0
$UpdateSigningSeedFile=''; $UpdatePublicKeyHex=''; $ManifestKeyId=''; $ManifestExpiresAtUtc=''; $SignerPath=''; $UpdateKeyringPath=''
. ([scriptblock]::Create($ModeGate[0].Extent.Text))
if (-not $SkipManifests) { throw '本地候选没有隔离正式清单。' }
$Channel='stable'
Must-Reject { . ([scriptblock]::Create($ModeGate[0].Extent.Text)) } '本地候选不可 stable'
$Channel='prerelease'; $UpdateSigningSeedFile='Z:/never-read/signing-seed.hex'
Must-Reject { . ([scriptblock]::Create($ModeGate[0].Extent.Text)) } '本地候选不可携带签名 seed'
$UpdateSigningSeedFile=''
$Windows = Join-Path $Repo 'build/windows'
$Linux = Join-Path $Repo 'build/linux'
foreach ($Name in @('AWJ.exe', 'AWJ.com')) { [IO.File]::WriteAllText((Join-Path $Windows $Name), "fixture:$Name", [Text.UTF8Encoding]::new($false)) }
[IO.File]::WriteAllText((Join-Path $Linux 'AWJ'), 'fixture:AWJ', [Text.UTF8Encoding]::new($false))
$Binaries = foreach ($Name in @('AWJ.exe', 'AWJ.com')) {
    $Path = Join-Path $Windows $Name
    @{path=$Name; size=(Get-Item -LiteralPath $Path).Length; sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$Proof = @{
    schema=1; mode='local-candidate'; platform='windows-x64'; version=$Version
    source_fingerprint_algorithm='sha256/git-normalized-source-v1'; source_fingerprint=$SourceFingerprint
    git_head=$GitHead; source_dirty=$true; dependency_lock_sha256=(Get-NormalizedLockHash)
    profile=@{build_type='Release'; minimum_cpu='AVX2'; ipo=$true; static_runtime=$true}
    verification=@{compiled_source_verified=$true; binary_version=$true}; binaries=@($Binaries)
}
$ProofPath = Join-Path $Windows 'build-evidence.json'
Save-Proof $Proof $ProofPath
$null=Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com')
$Proof.source_fingerprint='b' * 64
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com') } '不同源码指纹'
$Proof.source_fingerprint=$SourceFingerprint
$Proof.dependency_lock_sha256='d' * 64
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com') } '不同依赖锁'
$Proof.dependency_lock_sha256=Get-NormalizedLockHash
$Proof.profile.ipo=$false
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com') } 'LTO 被禁用'
$Proof.profile.ipo=$true
$Proof.binaries[0].path='../AWJ.exe'
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com') } '二进制路径越界'
$Proof.binaries[0].path='AWJ.exe'
$Proof.binaries[0].sha256='0' * 64
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com') } '真实二进制不匹配'
$Proof.binaries[0].sha256=(Get-FileHash -LiteralPath (Join-Path $Windows 'AWJ.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
$LocalCandidate=$false
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com') } 'dirty 候选不能混入正式模式'
$Proof.mode='formal'; $Proof.source_dirty=$false
Save-Proof $Proof $ProofPath
$null=Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe', 'AWJ.com')
$LocalCandidate=$true
$Proof.mode='local-candidate'; $Proof.source_dirty=$true
Save-Proof $Proof $ProofPath
$null=Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe','AWJ.com')
foreach ($Case in @(@{record=$Proof;field='source_fingerprint'}, @{record=$Proof;field='dependency_lock_sha256'}, @{record=$Proof.profile;field='ipo'}, @{record=$Proof.verification;field='compiled_source_verified'})) {
    $Original=$Case.record[$Case.field]
    $Case.record[$Case.field]=@()
    Save-Proof $Proof $ProofPath
    Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe','AWJ.com') } "空数组不能代替标量：$($Case.field)"
    $Case.record[$Case.field]=@($Original)
    Save-Proof $Proof $ProofPath
    Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe','AWJ.com') } "数组不能代替标量：$($Case.field)"
    $Case.record[$Case.field]=$Original
}
Save-Proof $Proof $ProofPath
$LinuxBinary=Join-Path $Linux 'AWJ'
$LinuxProof=@{
    schema=1; mode='local-candidate'; platform='linux-x64'; version=$Version
    source_fingerprint_algorithm='sha256/git-normalized-source-v1'; source_fingerprint=$SourceFingerprint
    git_head=('e' * 40); source_dirty=$true; dependency_lock_sha256=(Get-NormalizedLockHash)
    profile=@{build_type='Release'; minimum_cpu='x86-64-v3'; ipo=$true; cxx_runtime='libc++'}
    verification=@{compiled_source_verified=$true; native_execution=$true; executable_mode=$true; archive_roundtrip=$true}
    binaries=@(@{path='AWJ'; size=(Get-Item -LiteralPath $LinuxBinary).Length; sha256=(Get-FileHash -LiteralPath $LinuxBinary -Algorithm SHA256).Hash.ToLowerInvariant()})
}
$LinuxProofPath=Join-Path $Linux 'build-evidence.json'
Save-Proof $LinuxProof $LinuxProofPath
$null=Assert-BuildEvidence $LinuxProofPath 'linux-x64' $Linux @('AWJ')
$LinuxProof.verification.executable_mode=$false
Save-Proof $LinuxProof $LinuxProofPath
Must-Reject { Assert-BuildEvidence $LinuxProofPath 'linux-x64' $Linux @('AWJ') } '缺少 Linux 可执行位验证'
$Checks=@{}
foreach ($Name in @('windows_tests','linux_tests','hdr_math','iso_protocol','sampling','ui','original_jxr')) { $Checks[$Name]='passed' }
$VerifiedArtifacts=@(
    foreach ($Binary in $Proof.binaries) { @{platform='windows-x64'; path=$Binary.path; size=$Binary.size; sha256=$Binary.sha256} }
    foreach ($Binary in $LinuxProof.binaries) { @{platform='linux-x64'; path=$Binary.path; size=$Binary.size; sha256=$Binary.sha256} }
)
$ReportedArtifacts=@($VerifiedArtifacts | ForEach-Object { @{platform=$_.platform; path=$_.path; size=$_.size; sha256=$_.sha256} })
$Report=@{schema=1; version=$Version; source_fingerprint=$SourceFingerprint; status='passed'; checks=$Checks; artifacts=$ReportedArtifacts}
$ReportPath=Join-Path $Repo 'build/report.json'
Save-Proof $Report $ReportPath
$null=Assert-VerificationReport $ReportPath $VerifiedArtifacts
$SavedHash=$Report.artifacts[0].sha256
$Report.artifacts[0].sha256='0' * 64
Save-Proof $Report $ReportPath
Must-Reject { Assert-VerificationReport $ReportPath $VerifiedArtifacts } '其他二进制的验收报告不可复用'
$Report.artifacts[0].sha256=$SavedHash
$Checks.ui='not-executed'
Save-Proof $Report $ReportPath
Must-Reject { Assert-VerificationReport $ReportPath $VerifiedArtifacts } '未运行 UI 验收'
$Checks.ui=@()
Save-Proof $Report $ReportPath
Must-Reject { Assert-VerificationReport $ReportPath $VerifiedArtifacts } '空数组不能代替验收通过'
$Checks.ui='passed'; $Report.source_fingerprint=@()
Save-Proof $Report $ReportPath
Must-Reject { Assert-VerificationReport $ReportPath $VerifiedArtifacts } '验收指纹必须是字符串'
$Package=Join-Path $Repo 'build/archive-members'
[void][IO.Directory]::CreateDirectory($Package)
foreach ($Name in @('AWJ','LICENSE','NOTICE.txt')) { [IO.File]::WriteAllText((Join-Path $Package $Name), $Name, [Text.UTF8Encoding]::new($false)) }
Assert-ExactPackage $Package @('AWJ','LICENSE','NOTICE.txt')
if (Get-Command 7z.exe -ErrorAction SilentlyContinue) {
    $Archive=Join-Path $Repo 'build/allowed.7z'
    & 7z.exe a -t7z $Archive (Join-Path $Package 'AWJ') (Join-Path $Package 'LICENSE') (Join-Path $Package 'NOTICE.txt') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '隔离归档创建失败。' }
    Assert-ArchiveMemberList $Archive @('AWJ','LICENSE','NOTICE.txt')
    [IO.File]::WriteAllText((Join-Path $Package 'unexpected.json'), '{}', [Text.UTF8Encoding]::new($false))
    $Unexpected=Join-Path $Repo 'build/unknown-member.7z'
    & 7z.exe a -t7z $Unexpected (Join-Path $Package 'AWJ') (Join-Path $Package 'unexpected.json') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '负例归档创建失败。' }
    Must-Reject { Assert-ArchiveMemberList $Unexpected @('AWJ','LICENSE','NOTICE.txt') } '未知归档成员'
}
if (-not (Test-Path -LiteralPath (Join-Path $Package 'unexpected.json'))) {
    [IO.File]::WriteAllText((Join-Path $Package 'unexpected.json'), '{}', [Text.UTF8Encoding]::new($false))
}
Must-Reject { Assert-ExactPackage $Package @('AWJ','LICENSE','NOTICE.txt') } '未知平台包成员'
$LocalCandidate=$false; $Version='1.3.0'; $SourceFingerprint='a' * 64
Must-Reject { Assert-AdditionalReleaseAssets @() } '正式 1.3.0 缺少 SDK 和对应源码'
$ExtraPaths=@(); $ExtraProofs=@()
foreach ($Spec in @(@{kind='sdk';platform='windows-x64';name='AWJ_SDK_Win_x64.zip'},
    @{kind='sdk';platform='linux-x64';name='AWJ_SDK_Linux_x64.tar.gz'},
    @{kind='source';platform='source';name='AWJimage-1.3.0-source.zip'})) {
    $Directory=Join-Path $Repo "build/extra-$($Spec.platform)"
    [void][IO.Directory]::CreateDirectory($Directory)
    $Archive=Join-Path $Directory $Spec.name
    [IO.File]::WriteAllText($Archive, "fixture:$($Spec.name)", [Text.UTF8Encoding]::new($false))
    $Checks=if ($Spec.kind -ceq 'sdk') {
        @{sdk_tests='passed';installed_consumers='passed';exports='passed';archive_roundtrip='passed';relocated_stream='passed'}
    } else { @{source_commit='passed';source_layout='passed'} }
    $ExtraProof=@{schema=1;kind=$Spec.kind;platform=$Spec.platform;mode='formal';status='passed';version=$Version
        git_head=$GitHead;source_dirty=$false;source_fingerprint=$SourceFingerprint;verification=$Checks
        archive=@{name=$Spec.name;size=(Get-Item -LiteralPath $Archive).Length;sha256=(Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()}
        test_report_sha256=('b' * 64);consumer_report_sha256=('d' * 64)}
    $ExtraPath=Join-Path $Directory 'asset-evidence.json'
    Save-Proof $ExtraProof $ExtraPath
    $ExtraPaths+=,$ExtraPath; $ExtraProofs+=,$ExtraProof
}
if (@(Assert-AdditionalReleaseAssets $ExtraPaths).Count -ne 3) { throw '完整附加资产未通过。' }
Must-Reject { Assert-AdditionalReleaseAssets @($ExtraPaths[0],$ExtraPaths[0],$ExtraPaths[2]) } '重复平台 SDK'
Must-Reject { Assert-AdditionalReleaseAssets @($ExtraPaths[0],$ExtraPaths[1]) } '缺少对应源码'
foreach ($Field in @('git_head','source_fingerprint','mode','source_dirty','status')) {
    $Saved=$ExtraProofs[0][$Field]
    $ExtraProofs[0][$Field]=if ($Field -ceq 'source_dirty') { $true } else { 'mismatch' }
    Save-Proof $ExtraProofs[0] $ExtraPaths[0]
    Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths } "SDK 错误来源或状态：$Field"
    $ExtraProofs[0][$Field]=$Saved
}
$ExtraProofs[0].verification.relocated_stream='not-executed'
Save-Proof $ExtraProofs[0] $ExtraPaths[0]
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths } 'SDK 未完成迁移运行验证'
$ExtraProofs[0].verification.relocated_stream='passed'
$ExtraProofs[0].test_report_sha256=@('b' * 64)
Save-Proof $ExtraProofs[0] $ExtraPaths[0]
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths } 'SDK 测试摘要不能为数组'
$ExtraProofs[0].test_report_sha256='b' * 64
$SavedName=$ExtraProofs[0].archive.name
$ExtraProofs[0].archive.name='../AWJ_SDK_Win_x64.zip'
Save-Proof $ExtraProofs[0] $ExtraPaths[0]
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths } 'SDK 归档路径越界'
$ExtraProofs[0].archive.name=$SavedName
Save-Proof $ExtraProofs[0] $ExtraPaths[0]
$ChangedArchive=Join-Path (Split-Path -Parent $ExtraPaths[1]) $ExtraProofs[1].archive.name
[IO.File]::AppendAllText($ChangedArchive, 'changed')
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths } 'SDK 归档在验收后变化'
# 1.3.1 四角色使用元数据夹具；不代表真实 SDK、消费者或设备已通过。
$Version='1.3.1'; $ExtraPaths131=@(); $ExtraProofs131=@()
foreach ($Spec in @(
    @{kind='sdk';platform='windows-x64';name='AWJ_SDK_Win_x64.zip';profile='windows-x64-msvc';compiler='19.51.36260';triple='x86_64-pc-windows-msvc'},
    @{kind='sdk';platform='linux-x64';name='AWJ_SDK_Linux_x64.tar.gz';profile='linux-x64-clang23';compiler='23.1.5';triple='x86_64-pc-linux-gnu'},
    @{kind='sdk';platform='hmos-arm64-api26';name='AWJ_SDK_HMOS_arm64_api26.tar.gz';profile='hmos-arm64-api26-clang23';compiler='23.1.5';triple='aarch64-unknown-linux-ohos'},
    @{kind='source';platform='source';name='AWJimage-1.3.1-source.zip'})) {
    $Directory=Join-Path $Repo "build/extra-131-$($Spec.platform)"
    [void][IO.Directory]::CreateDirectory($Directory)
    $Archive=Join-Path $Directory $Spec.name
    [IO.File]::WriteAllText($Archive, "metadata-fixture:$($Spec.name)", [Text.UTF8Encoding]::new($false))
    $Checks=if ($Spec.kind -ceq 'sdk') {
        @{sdk_tests='passed';installed_consumers='passed';exports='passed';archive_roundtrip='passed';compiler_identity='passed';target_identity='passed'}
    } else { @{source_commit='passed';source_layout='passed'} }
    $Proof131=@{schema=1;kind=$Spec.kind;platform=$Spec.platform;mode='formal';status='passed';version=$Version
        git_head=$GitHead;source_dirty=$false;source_fingerprint=$SourceFingerprint;verification=$Checks
        archive=@{name=$Spec.name;size=(Get-Item -LiteralPath $Archive).Length;sha256=(Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()}
        test_report_sha256=('b' * 64);consumer_report_sha256=('d' * 64)}
    if ($Spec.kind -ceq 'sdk') {
        $Proof131.profile=$Spec.profile; $Proof131.compiler_version=$Spec.compiler; $Proof131.target_triple=$Spec.triple
        if ($Spec.platform -ceq 'windows-x64') {
            $Proof131.toolchain=New-MsvcFixtureEvidence @('awj_sdk') x64
            $Checks.msvc_release_optimization='passed'
        }
        if ($Spec.platform -ceq 'hmos-arm64-api26') {
            $Proof131.native_sdk_version='26.0.0.105'; $Proof131.api=26; $Proof131.libcxx_version=15004
            $Proof131.libcxx_abi_namespace='__n1'; $Proof131.target_execution_report_sha256='e' * 64
            $Checks.target_execution='passed'; $Checks.target_loader='passed'
        } else { $Checks.relocated_stream='passed' }
    }
    $Receipt=Join-Path $Directory 'asset-evidence.json'
    Save-Proof $Proof131 $Receipt
    $ExtraPaths131+=,$Receipt; $ExtraProofs131+=,$Proof131
}
if (@(Assert-AdditionalReleaseAssets $ExtraPaths131).Count -ne 4) { throw '1.3.1 四角色夹具未通过。' }
Must-Reject { Assert-AdditionalReleaseAssets @($ExtraPaths131[0],$ExtraPaths131[1],$ExtraPaths131[3]) } '1.3.1 缺少 HMOS SDK'
Must-Reject { Assert-AdditionalReleaseAssets @($ExtraPaths131[0],$ExtraPaths131[1],$ExtraPaths131[2],$ExtraPaths131[2]) } '1.3.1 重复 HMOS 并缺少源码'
foreach ($Case in @(
    @{index=0;field='platform';value='Windows-x64'},
    @{index=0;field='profile';value='windows-x64-clang21'},
    @{index=0;field='compiler_version';value='21.1.8'},
    @{index=1;field='compiler_version';value='21.1.8'},
    @{index=2;field='compiler_version';value='21.1.8'},
    @{index=2;field='profile';value='linux-x64-clang23'},
    @{index=2;field='target_triple';value='x86_64-pc-linux-gnu'},
    @{index=2;field='native_sdk_version';value='26.0.0.104'},
    @{index=2;field='api';value=25},
    @{index=2;field='api';value='26'},
    @{index=2;field='libcxx_version';value=15003},
    @{index=2;field='libcxx_abi_namespace';value='__1'},
    @{index=2;field='target_execution_report_sha256';value=@('e' * 64)})) {
    $Saved=$ExtraProofs131[$Case.index][$Case.field]
    $ExtraProofs131[$Case.index][$Case.field]=$Case.value
    Save-Proof $ExtraProofs131[$Case.index] $ExtraPaths131[$Case.index]
    Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } "1.3.1 错误目标身份：$($Case.field)"
    $ExtraProofs131[$Case.index][$Case.field]=$Saved
    Save-Proof $ExtraProofs131[$Case.index] $ExtraPaths131[$Case.index]
}
$MsvcTarget=$ExtraProofs131[0].toolchain.targets[0]
foreach ($Field in @('ipo','runtime')) {
    $Saved=$MsvcTarget[$Field]
    $MsvcTarget[$Field]=if ($Field -ceq 'ipo') { $false } else { 'MultiThreadedDLL' }
    Save-Proof $ExtraProofs131[0] $ExtraPaths131[0]
    Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } "Windows SDK 优化或运行库降档：$Field"
    $MsvcTarget[$Field]=$Saved
}
$SavedFlags=$MsvcTarget.compile_flags
$MsvcTarget.compile_flags=@($SavedFlags | Where-Object { $_ -cne '/GL' })
Save-Proof $ExtraProofs131[0] $ExtraPaths131[0]
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } 'Windows SDK 缺少 GL'
$MsvcTarget.compile_flags=$SavedFlags
Save-Proof $ExtraProofs131[0] $ExtraPaths131[0]
$LocalCandidate=$true
$Proof.version=$Version; $Proof.toolchain=New-MsvcFixtureEvidence @('AWJ','AWJ-com') AVX2
Save-Proof $Proof $ProofPath
$null=Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe','AWJ.com')
$Proof.toolchain.compiler_id='Clang'
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe','AWJ.com') } 'MSVC ABI 兼容不能代替生产 MSVC'
$Proof.toolchain.compiler_id='MSVC'; $Proof.toolchain.targets[1].link_flags=@('/OPT:REF','/OPT:ICF','/INCREMENTAL:NO')
Save-Proof $Proof $ProofPath
Must-Reject { Assert-BuildEvidence $ProofPath 'windows-x64' $Windows @('AWJ.exe','AWJ.com') } 'COM 最终链接缺少 LTCG'
$LocalCandidate=$false
foreach ($Check in @('compiler_identity','target_identity','target_execution','target_loader')) {
    $ExtraProofs131[2].verification[$Check]='not-executed'
    Save-Proof $ExtraProofs131[2] $ExtraPaths131[2]
    Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } "HMOS 缺少真实通过项：$Check"
    $ExtraProofs131[2].verification[$Check]='passed'
    Save-Proof $ExtraProofs131[2] $ExtraPaths131[2]
}
# HMOS 预览是独立角色；只有本次显式范围开关才接收，不能伪装运行通过。
$Preview = ($ExtraProofs131[2] | ConvertTo-Json -Depth 12 | ConvertFrom-Json -AsHashtable)
$Preview.kind='sdk-preview'; $Preview.mode='preview'; $Preview.status='cross-linked'
$Preview.validation_tier='cross-build-preview'; $Preview.target_execution='not-run'; $Preview.target_loader='not-run'
$Preview.runtime_linkage='native-static'; $Preview.libcxx_abi_version=1
foreach ($Field in @('test_report_sha256','consumer_report_sha256','target_execution_report_sha256')) { $Preview.Remove($Field) }
foreach ($Field in @('cross_build_report_sha256','cross_link_consumer_report_sha256','preview_notice_sha256')) { $Preview[$Field]='a' * 64 }
$Preview.verification=@{cross_build='passed';test_programs_linked='passed';installed_consumers_linked='passed';compiler_identity='passed';target_identity='passed';native_inputs='passed';exports='passed';archive_roundtrip='passed';preview_notice='passed';target_execution='not-run';target_loader='not-run'}
Save-Proof $Preview $ExtraPaths131[2]
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } '未显式选择却混入 HMOS 预览'
$HmosSdkPreview=$true
if (@(Assert-AdditionalReleaseAssets $ExtraPaths131).Count -ne 4) { throw '明确预览范围的四角色未通过。' }
foreach ($Case in @(@{field='mode';value='formal'},@{field='status';value='passed'},@{field='validation_tier';value='target-verified'},@{field='target_execution';value='passed'},@{field='target_loader';value='passed'},@{field='profile';value='linux-x64-clang23'},@{field='libcxx_abi_namespace';value='__1'},@{field='preview_notice_sha256';value=@('a' * 64)})) {
    $Saved=$Preview[$Case.field]; $Preview[$Case.field]=$Case.value
    Save-Proof $Preview $ExtraPaths131[2]
    Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } "预览范围伪装或错误身份：$($Case.field)"
    $Preview[$Case.field]=$Saved
}
foreach ($Field in @('test_report_sha256','consumer_report_sha256','target_execution_report_sha256')) {
    $Preview[$Field]='a' * 64; Save-Proof $Preview $ExtraPaths131[2]
    Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } "预览夹带运行验收：$Field"
    $Preview.Remove($Field)
}
foreach ($Check in @('target_execution','target_loader','preview_notice','installed_consumers_linked')) {
    $Saved=$Preview.verification[$Check]; $Preview.verification[$Check]='passed-runtime'
    Save-Proof $Preview $ExtraPaths131[2]
    Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } "预览缺少明确验证状态：$Check"
    $Preview.verification[$Check]=$Saved
}
Save-Proof $Preview $ExtraPaths131[2]
$Version='1.3.2'
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } '预览开关越过本次版本授权'
$Version='1.3.1'; $LocalCandidate=$true
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } '预览发行与本地候选模式混用'
$LocalCandidate=$false
Save-Proof $ExtraProofs131[2] $ExtraPaths131[2]
Must-Reject { Assert-AdditionalReleaseAssets $ExtraPaths131 } '选择预览范围却夹入正式目标运行角色'
$HmosSdkPreview=$false
if (@(Assert-AdditionalReleaseAssets $ExtraPaths131).Count -ne 4) { throw '预览验证破坏默认正式范围。' }
if ((Get-Content -LiteralPath (Join-Path $Repo 'bin/x64/Release/sentinel') -Raw) -cne 'unchanged') { throw '保护目录被改变。' }
[ordered]@{status='passed'; fixture_root=$Repo; cleanup='not-executed'; scope='evidence-and-layout-contract'; protected='unchanged'; sdk_roles_131=4} | ConvertTo-Json -Compress
