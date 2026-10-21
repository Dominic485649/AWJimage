$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $Project 'scripts/release-safety.psm1') -Force
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
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $Project 'scripts/package-release.ps1'), [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw '跨平台打包脚本语法错误。' }
# 仅加载校验函数。不会执行完整打包、启动假二进制或访问签名密钥。
foreach ($Name in @('Get-RepoPath', 'Assert-File', 'Assert-Directory', 'Get-NormalizedLockHash', 'Assert-BuildEvidence', 'Assert-VerificationReport', 'Assert-ArchiveMemberList', 'Assert-ExactPackage')) {
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
if ((Get-Content -LiteralPath (Join-Path $Repo 'bin/x64/Release/sentinel') -Raw) -cne 'unchanged') { throw '保护目录被改变。' }
[ordered]@{status='passed'; fixture_root=$Repo; cleanup='not-executed'; scope='evidence-and-layout-contract'; protected='unchanged'} | ConvertTo-Json -Compress
