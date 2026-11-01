$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../scripts/release-safety.psm1') -Force
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$root = Join-Path $project "build/update-1.2.3-work/release-safety-tests-$([Guid]::NewGuid().ToString('N'))"
[void][IO.Directory]::CreateDirectory($root)
# 测试只创建自己的隔离目录，不删除真实版本或构建缓存。
foreach ($dir in @('bin/x64/Release','src','build')) { [void][IO.Directory]::CreateDirectory((Join-Path $root $dir)) }
[IO.File]::WriteAllText((Join-Path $root 'bin/x64/Release/sentinel.txt'), 'protected', [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $root 'VERSION'), "1.2.3`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $root '.gitattributes'), "* text=auto`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $root 'CMakeLists.txt'), "project(AWJimage)`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $root 'src/example.cpp'), "first`nsecond`n", [Text.UTF8Encoding]::new($false))
& git -C $root init --quiet
if ($LASTEXITCODE -ne 0) { throw '隔离 Git 初始化失败。' }
function Must-Reject([scriptblock]$Action, [string]$Label) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    if (-not $rejected) { throw "应拒绝但接受了：$Label" }
}
Must-Reject { Resolve-AwjRepoPath -RepositoryRoot $root -Path (Join-Path $root 'bin/x64/Release/new.exe') } 'protected bin/x64'
Must-Reject { Resolve-AwjRepoPath -RepositoryRoot $root -Path (Join-Path $root '../outside.txt') } 'outside repository'
Must-Reject { Resolve-AwjRepoPath -RepositoryRoot $root -Path $root } 'repository root'
Must-Reject { Resolve-AwjRepoPath -RepositoryRoot $root -Path (Join-Path $root 'release-signing.local.json') } 'signing local config'
Must-Reject { Assert-AwjBuildPath -RepositoryRoot $root -Path (Join-Path $root 'build') } 'entire build directory'
$first=Get-AwjSourceFingerprint -RepositoryRoot $root
[IO.File]::WriteAllText((Join-Path $root 'src/example.cpp'), "first`r`nsecond`r`n", [Text.UTF8Encoding]::new($false))
$second=Get-AwjSourceFingerprint -RepositoryRoot $root
if ($first -cne $second) { throw '源码指纹未规范化 CRLF。' }
[IO.File]::WriteAllText((Join-Path $root 'src/example.cpp'), "changed`n", [Text.UTF8Encoding]::new($false))
if ((Get-AwjSourceFingerprint -RepositoryRoot $root) -ceq $first) { throw '源码变化未改变指纹。' }
$unstaged=Get-AwjSourceFingerprint -RepositoryRoot $root
& git -C $root add -- src/example.cpp
if ($LASTEXITCODE -ne 0 -or (Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $unstaged) { throw '仅暂存源码改变了工作树指纹。' }
Move-Item -LiteralPath (Join-Path $root 'src/example.cpp') -Destination (Join-Path $root 'src/renamed.cpp')
$renamed=Get-AwjSourceFingerprint -RepositoryRoot $root
& git -C $root add -A -- src
if ($LASTEXITCODE -ne 0 -or (Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $renamed) { throw '源码重命名指纹依赖暂存状态。' }
Move-Item -LiteralPath (Join-Path $root 'src/renamed.cpp') -Destination (Join-Path $root 'build/retired-example.cpp')
$deleted=Get-AwjSourceFingerprint -RepositoryRoot $root
& git -C $root add -u -- src
if ($LASTEXITCODE -ne 0 -or (Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $deleted) { throw '源码删除指纹依赖暂存状态。' }
[IO.File]::WriteAllText((Join-Path $root 'src/中文-é.cpp'), "unicode`n", [Text.UTF8Encoding]::new($false))
$unicode=Get-AwjSourceFingerprint -RepositoryRoot $root
& git -C $root add -- 'src/中文-é.cpp'
if ($LASTEXITCODE -ne 0 -or (Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $unicode) { throw '中文源码指纹依赖暂存状态。' }
$proofBuild=Join-Path $root 'build/provenance'
[void][IO.Directory]::CreateDirectory($proofBuild)
[IO.File]::WriteAllText((Join-Path $proofBuild 'CMakeCache.txt'), 'fixture-cache', [Text.UTF8Encoding]::new($false))
$proofBinary=Join-Path $proofBuild 'AWJ.com'
[IO.File]::WriteAllText($proofBinary, 'owned-fixture-binary', [Text.UTF8Encoding]::new($false))
$proofFingerprint=Get-AwjSourceFingerprint -RepositoryRoot $root
$inputState=Initialize-AwjBuildProvenance -RepositoryRoot $root -BuildDirectory $proofBuild -SourceFingerprint $proofFingerprint -BinaryPaths @($proofBinary)
if ($inputState.reused_verified_inputs -or (Get-AwjSourceFingerprint -RepositoryRoot $root) -cne $proofFingerprint) { throw '未知缓存未刷新来源或触碰了源码内容。' }
Complete-AwjBuildProvenance -State $inputState
$reused=Initialize-AwjBuildProvenance -RepositoryRoot $root -BuildDirectory $proofBuild -SourceFingerprint $proofFingerprint -BinaryPaths @($proofBinary)
if (-not $reused.reused_verified_inputs) { throw '同源码同参数同字节缓存未复用。' }
[IO.File]::WriteAllText($proofBinary, 'changed-fixture-binary', [Text.UTF8Encoding]::new($false))
$changed=Initialize-AwjBuildProvenance -RepositoryRoot $root -BuildDirectory $proofBuild -SourceFingerprint $proofFingerprint -BinaryPaths @($proofBinary)
if ($changed.reused_verified_inputs) { throw '变化的二进制被错误复用来源证明。' }
$stage=New-AwjReleaseStage -RepositoryRoot $root -Version '1.2.3'
[void][IO.Directory]::CreateDirectory((Join-Path $stage 'verification'))
[void][IO.Directory]::CreateDirectory((Join-Path $stage 'Windows'))
$asset=Join-Path $stage 'Windows/AWJ.com'
[IO.File]::WriteAllText($asset,'fixture executable bytes',[Text.UTF8Encoding]::new($false))
$receipt=[ordered]@{schema=1;version='1.2.3';status='passed';source_fingerprint=(Get-AwjSourceFingerprint -RepositoryRoot $root);artifacts=@(@{path='Windows/AWJ.com';size=(Get-Item -LiteralPath $asset).Length;sha256=(Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()})}
$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'verification/acceptance.json') -Encoding utf8
Must-Reject { Test-AwjArtifactAcceptance -RepositoryRoot $root -Directory $stage -Version '1.2.2' } 'version mismatch'
foreach ($Field in @('status','source_fingerprint')) {
    $Original=$receipt[$Field]
    $receipt[$Field]=@()
    $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'verification/acceptance.json') -Encoding utf8
    Must-Reject { Test-AwjArtifactAcceptance -RepositoryRoot $root -Directory $stage -Version '1.2.3' } "验收标量类型：$Field"
    $receipt[$Field]=$Original
}
$receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'verification/acceptance.json') -Encoding utf8
$destination=Publish-AwjVersionStage -RepositoryRoot $root -Version '1.2.3' -Stage $stage
$null=Test-AwjArtifactAcceptance -RepositoryRoot $root -Directory $destination -Version '1.2.3'
$again=New-AwjReleaseStage -RepositoryRoot $root -Version '1.2.3'
[void][IO.Directory]::CreateDirectory((Join-Path $again 'verification'))
[void][IO.Directory]::CreateDirectory((Join-Path $again 'Windows'))
Copy-Item -LiteralPath (Join-Path $destination 'Windows/AWJ.com') -Destination (Join-Path $again 'Windows/AWJ.com')
Copy-Item -LiteralPath (Join-Path $destination 'verification/acceptance.json') -Destination (Join-Path $again 'verification/acceptance.json')
$null=Test-AwjArtifactAcceptance -RepositoryRoot $root -Directory $again -Version '1.2.3'
$collision=''
try { Publish-AwjVersionStage -RepositoryRoot $root -Version '1.2.3' -Stage $again | Out-Null } catch { $collision=$_.Exception.Message }
if ($collision -notlike '拒绝覆盖已存在的版本目录：*') { throw "未命中已存在目标保护：$collision" }
# 版本根已有被引用的候选时，显式保留它们，只投放无冲突的新成员。
$preserveStage=New-AwjReleaseStage -RepositoryRoot $root -Version '1.3.1'
[void][IO.Directory]::CreateDirectory((Join-Path $preserveStage 'Windows'))
[void][IO.Directory]::CreateDirectory((Join-Path $preserveStage 'verification'))
$preserveAsset=Join-Path $preserveStage 'Windows/AWJ.com'
[IO.File]::WriteAllText($preserveAsset,'new formal fixture',[Text.UTF8Encoding]::new($false))
$preserveReceipt=@{schema=1;version='1.3.1';status='passed';source_fingerprint=(Get-AwjSourceFingerprint -RepositoryRoot $root);artifacts=@(@{path='Windows/AWJ.com';size=(Get-Item -LiteralPath $preserveAsset).Length;sha256=(Get-FileHash -LiteralPath $preserveAsset -Algorithm SHA256).Hash.ToLowerInvariant()})}
$preserveReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $preserveStage 'verification/acceptance.json') -Encoding utf8
$existing=Join-Path $root 'bin/1.3.1/dev-msvc-v1'
[void][IO.Directory]::CreateDirectory($existing)
$sentinel=Join-Path $existing 'candidate.bin'
[IO.File]::WriteAllText($sentinel,'referenced candidate fixture',[Text.UTF8Encoding]::new($false))
$sentinelHash=(Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
$sentinelTime=(Get-Item -LiteralPath $sentinel).LastWriteTimeUtc
Must-Reject { Publish-AwjVersionStage -RepositoryRoot $root -Version '1.3.1' -Stage $preserveStage } '缺少显式保留开关'
$preserveDestination=Publish-AwjVersionStage -RepositoryRoot $root -Version '1.3.1' -Stage $preserveStage -PreserveExisting
$null=Test-AwjArtifactAcceptance -RepositoryRoot $root -Directory $preserveDestination -Version '1.3.1'
if ((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -cne $sentinelHash -or (Get-Item -LiteralPath $sentinel).LastWriteTimeUtc -ne $sentinelTime) { throw '候选内容或原位置被正式投放改变。' }
$collisionStage=New-AwjReleaseStage -RepositoryRoot $root -Version '1.3.1'
[void][IO.Directory]::CreateDirectory((Join-Path $collisionStage 'Windows'))
[void][IO.Directory]::CreateDirectory((Join-Path $collisionStage 'verification'))
Copy-Item -LiteralPath (Join-Path $preserveDestination 'Windows/AWJ.com') -Destination (Join-Path $collisionStage 'Windows/AWJ.com')
Copy-Item -LiteralPath (Join-Path $preserveDestination 'verification/acceptance.json') -Destination (Join-Path $collisionStage 'verification/acceptance.json')
Must-Reject { Publish-AwjVersionStage -RepositoryRoot $root -Version '1.3.1' -Stage $collisionStage -PreserveExisting } '显式保留仍拒绝任何同名正式成员'
if (-not (Test-Path -LiteralPath (Join-Path $collisionStage '.awj-owned-stage.json'))) { throw '冲突拒绝前已移动暂存成员。' }
$old=Join-Path $root 'bin/1.2.2'
[void][IO.Directory]::CreateDirectory((Join-Path $old 'preset'))
[IO.File]::WriteAllText((Join-Path $old 'AWJ.com'), 'old executable', [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $old 'AWJ.jsonc'), '{"menu_jpgli_chroma_index":4}', [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $old 'preset/custom.jsonc'), '{"name":"custom"}', [Text.UTF8Encoding]::new($false))
$plan=New-AwjReleaseCleanupPlan -RepositoryRoot $root -Version '1.2.3'
if ($plan.mode -ne 'non-destructive-plan' -or @($plan.candidates).Count -ne 1) { throw '只读清理计划错误。' }
$backups=@(Backup-AwjReleaseUserData -RepositoryRoot $root -Plan $plan)
if ($backups.Count -ne 2 -or -not (Test-Path -LiteralPath (Join-Path $old 'AWJ.com'))) { throw '备份动作错误地改变了原版本。' }
if (Test-Path -LiteralPath (Join-Path $destination 'user-data-backup/1.2.2/AWJ.com')) { throw '旧可执行文件不应进入用户数据备份。' }
$earlier=Join-Path $root 'bin/1.2.1'
[void][IO.Directory]::CreateDirectory($earlier)
[IO.File]::WriteAllText((Join-Path $earlier 'AWJ.jsonc'), 'original-version-data', [Text.UTF8Encoding]::new($false))
[void][IO.Directory]::CreateDirectory((Join-Path $old 'user-data-backup/1.2.1'))
Copy-Item -LiteralPath (Join-Path $earlier 'AWJ.jsonc') -Destination (Join-Path $old 'user-data-backup/1.2.1/AWJ.jsonc')
[void][IO.Directory]::CreateDirectory((Join-Path $old 'user-data-backup/1.1.0'))
[IO.File]::WriteAllText((Join-Path $old 'user-data-backup/1.1.0/only-in-backup.jsonc'), 'inherited-only', [Text.UTF8Encoding]::new($false))
$nextPlan=New-AwjReleaseCleanupPlan -RepositoryRoot $root -Version '1.2.3'
$nextResults=@(Backup-AwjReleaseUserData -RepositoryRoot $root -Plan $nextPlan)
if ($nextResults.Count -ne 4 -or -not (Test-Path -LiteralPath (Join-Path $destination 'user-data-backup/1.1.0/only-in-backup.jsonc'))) { throw '继承备份未按原始来源去重保留。' }
if (Test-Path -LiteralPath (Join-Path $destination 'user-data-backup/1.2.2/user-data-backup')) { throw '历史备份被递归套娃复制。' }
if (-not (Test-Path -LiteralPath (Join-Path $old 'AWJ.com'))) { throw '继承备份错误地删除了旧版本。' }
if ((Get-Content -LiteralPath (Join-Path $root 'bin/x64/Release/sentinel.txt') -Raw) -cne 'protected') { throw '受保护目录发生变化。' }
$hiddenBackupStatus='not-applicable-non-windows'
if ($IsWindows) {
    function Get-FixtureFileState([string]$Path) {
        $Item=Get-Item -LiteralPath $Path -Force
        return [pscustomobject]@{sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();
            size=$Item.Length; attributes=[int]$Item.Attributes; last_write_ticks=$Item.LastWriteTimeUtc.Ticks}
    }
    function Assert-FixtureFileState([string]$Path,$Before,[string]$Label) {
        $After=Get-FixtureFileState $Path
        if ($After.sha256 -cne $Before.sha256 -or $After.size -ne $Before.size -or
            $After.attributes -ne $Before.attributes -or $After.last_write_ticks -ne $Before.last_write_ticks) {
            throw "隐藏文件备份改变了$Label 的内容、属性或写入时间。"
        }
    }
    $HiddenConfig=Join-Path $old 'AWJ.jsonc'
    $HiddenStateName='.awj-update-security-state.json'
    $HiddenState=Join-Path $old $HiddenStateName
    [IO.File]::WriteAllText($HiddenState,'{"fixture":"old-state","sequence":1}',[Text.UTF8Encoding]::new($false))
    $FixtureTime=[DateTime]::Parse('2024-01-02T03:04:05Z',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
    foreach ($Path in @($HiddenConfig,$HiddenState)) {
        [IO.File]::SetAttributes($Path,[IO.File]::GetAttributes($Path) -bor [IO.FileAttributes]::Hidden)
        [IO.File]::SetLastWriteTimeUtc($Path,$FixtureTime)
        if (-not ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::Hidden)) { throw '没有构造真实 Windows Hidden 文件。' }
    }
    $HiddenConfigBefore=Get-FixtureFileState $HiddenConfig
    $HiddenStateBefore=Get-FixtureFileState $HiddenState
    # 当前版本模拟已有配置及更高 sequence，备份不得激活或合并旧状态。
    $ActiveConfig=Join-Path $destination 'Windows/AWJ.jsonc'
    $ActiveState=Join-Path $destination 'Windows/.awj-update-security-state.json'
    [IO.File]::WriteAllText($ActiveConfig,'{"fixture":"active-config"}',[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($ActiveState,'{"fixture":"active-state","sequence":9001}',[Text.UTF8Encoding]::new($false))
    [IO.File]::SetAttributes($ActiveState,[IO.File]::GetAttributes($ActiveState) -bor [IO.FileAttributes]::Hidden)
    $ActiveConfigBefore=Get-FixtureFileState $ActiveConfig
    $ActiveStateBefore=Get-FixtureFileState $ActiveState
    # 匹配的既有备份也设为 Hidden，覆盖第二次读取和幂等路径。
    $HiddenConfigBackup=Join-Path $destination 'user-data-backup/1.2.2/AWJ.jsonc'
    [IO.File]::SetAttributes($HiddenConfigBackup,[IO.File]::GetAttributes($HiddenConfigBackup) -bor [IO.FileAttributes]::Hidden)
    $HiddenConfigBackupBefore=Get-FixtureFileState $HiddenConfigBackup
    # 不同内容的隐藏备份必须保留，新内容只能写入 hash 命名的 history。
    $ConflictStateBackup=Join-Path $destination "user-data-backup/1.2.2/$HiddenStateName"
    [IO.File]::WriteAllText($ConflictStateBackup,'{"fixture":"existing-conflict"}',[Text.UTF8Encoding]::new($false))
    [IO.File]::SetAttributes($ConflictStateBackup,[IO.File]::GetAttributes($ConflictStateBackup) -bor [IO.FileAttributes]::Hidden)
    $ConflictStateBackupBefore=Get-FixtureFileState $ConflictStateBackup
    $OldExecutableBefore=Get-FixtureFileState (Join-Path $old 'AWJ.com')
    $NewExecutableBefore=Get-FixtureFileState (Join-Path $destination 'Windows/AWJ.com')
    $HiddenPlan=New-AwjReleaseCleanupPlan -RepositoryRoot $root -Version '1.2.3'
    $HiddenFirst=@(Backup-AwjReleaseUserData -RepositoryRoot $root -Plan $HiddenPlan)
    if ($HiddenFirst.Count -ne ($nextResults.Count+1)) { throw '隐藏配置和安全状态未完整备份，或继承备份重复。' }
    $StateRecords=@($HiddenFirst | Where-Object { $_.version -ceq '1.2.2' -and $_.path -ceq $HiddenStateName })
    $ExpectedHistory="1.2.2/.history/$($HiddenStateBefore.sha256)/$HiddenStateName"
    if ($StateRecords.Count -ne 1 -or $StateRecords[0].backup_path -cne $ExpectedHistory) { throw '冲突的隐藏备份未按内容版本保存。' }
    $HiddenHistory=Join-Path $destination ('user-data-backup/'+$StateRecords[0].backup_path)
    if ((Get-FixtureFileState $HiddenHistory).sha256 -cne $HiddenStateBefore.sha256 -or
        (Get-FixtureFileState $HiddenConfigBackup).sha256 -cne $HiddenConfigBefore.sha256) { throw '隐藏源与备份 SHA256 不匹配。' }
    [IO.File]::SetAttributes($HiddenHistory,[IO.File]::GetAttributes($HiddenHistory) -bor [IO.FileAttributes]::Hidden)
    $HiddenHistoryBefore=Get-FixtureFileState $HiddenHistory
    $HiddenSecond=@(Backup-AwjReleaseUserData -RepositoryRoot $root -Plan $HiddenPlan)
    $FirstIdentities=@($HiddenFirst | ForEach-Object { $_.backup_path+':'+$_.sha256 } | Sort-Object)
    $SecondIdentities=@($HiddenSecond | ForEach-Object { $_.backup_path+':'+$_.sha256 } | Sort-Object)
    if ($HiddenSecond.Count -ne $HiddenFirst.Count -or @(Compare-Object $FirstIdentities $SecondIdentities).Count) { throw '隐藏备份重复调用不幂等。' }
    Assert-FixtureFileState $HiddenConfig $HiddenConfigBefore '原配置'
    Assert-FixtureFileState $HiddenState $HiddenStateBefore '原安全状态'
    Assert-FixtureFileState $HiddenConfigBackup $HiddenConfigBackupBefore '匹配的既有隐藏备份'
    Assert-FixtureFileState $ConflictStateBackup $ConflictStateBackupBefore '冲突的既有隐藏备份'
    Assert-FixtureFileState $HiddenHistory $HiddenHistoryBefore '隐藏 history 备份'
    Assert-FixtureFileState $ActiveConfig $ActiveConfigBefore '当前版本配置'
    Assert-FixtureFileState $ActiveState $ActiveStateBefore '当前版本安全状态'
    Assert-FixtureFileState (Join-Path $old 'AWJ.com') $OldExecutableBefore '旧可执行文件'
    Assert-FixtureFileState (Join-Path $destination 'Windows/AWJ.com') $NewExecutableBefore '新可执行文件'
    $hiddenBackupStatus='passed'
    $link=Join-Path $root 'build/protected-alias'
    New-Item -ItemType Junction -Path $link -Target (Join-Path $root 'bin/x64') | Out-Null
    Must-Reject { Resolve-AwjRepoPath -RepositoryRoot $root -Path (Join-Path $link 'Release/sentinel.txt') } 'junction alias'
    [void][IO.Directory]::CreateDirectory((Join-Path $root 'build/runtime'))
    $leaf=Join-Path $root 'build/runtime/Release'
    New-Item -ItemType Junction -Path $leaf -Target (Join-Path $root 'bin/x64/Release') | Out-Null
    $null=Assert-AwjBuildPath -RepositoryRoot $root -Path (Join-Path $root 'build/runtime')
    Must-Reject { Assert-AwjBuildPath -RepositoryRoot $root -Path $leaf } 'runtime Release junction'
}
[pscustomobject]@{status='passed';fixture_root=$root;cleanup='not-executed';protected='unchanged';source_normalization='passed';hidden_backup=$hiddenBackupStatus} | ConvertTo-Json -Compress
