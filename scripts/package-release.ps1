[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$WindowsExePath,
    [Parameter(Mandatory)] [string]$WindowsComPath,
    [Parameter(Mandatory)] [string]$LinuxPackagePath,
    [Parameter(Mandatory)] [string]$LinuxArchivePath,
    [string]$Version = "",
    [ValidateSet("stable", "prerelease")] [string]$Channel = "prerelease",
    [UInt64]$ArchiveManifestSequence = 0,
    [string]$PublishedAtUtc = "",
    [string]$UpdateSigningSeedFile = $env:AWJ_UPDATE_SIGNING_SEED_FILE,
    [string]$UpdatePublicKeyHex = $env:AWJ_UPDATE_PUBLIC_KEY_HEX,
    [string]$ManifestKeyId = "",
    [string]$ManifestExpiresAtUtc = "",
    [string]$UpdateKeyringPath = "",
    [string]$ExistingManifestPublicKeyHex = "",
    [string]$SignerPath = "",
    [string]$WindowsEvidencePath = "",
    [string]$LinuxEvidencePath = "",
    [Parameter(Mandatory)] [string]$VerificationReportPath,
    [string[]]$AdditionalAssetEvidencePaths = @(),
    [switch]$LocalCandidate,
    [switch]$SkipManifests
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Import-Module (Join-Path $Repo 'scripts/release-safety.psm1') -Force
Import-Module (Join-Path $Repo 'scripts/windows-msvc.psm1') -Force
$GitHead = (git -C $Repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $GitHead -notmatch '^[0-9a-f]{40}$') { throw '无法读取打包源码 HEAD。' }
$TrackedChanges = @(git -C $Repo status --porcelain=v1 --untracked-files=no)
if ($LASTEXITCODE -ne 0) { throw '无法读取源码状态。' }
$UntrackedSources = @(git -C $Repo ls-files --others --exclude-standard -- src ui cmake tests scripts docs licenses vcpkg-overlays)
if ($LASTEXITCODE -ne 0) { throw '无法核对未跟踪源码。' }
if ($LocalCandidate) {
    if ($Channel -eq 'stable' -or $UpdateSigningSeedFile -or $UpdatePublicKeyHex -or $ManifestKeyId -or $ManifestExpiresAtUtc -or $ArchiveManifestSequence -or $SignerPath -or $UpdateKeyringPath) {
        throw '本地候选不得混用正式渠道或签名参数。'
    }
    $SkipManifests = $true
} elseif ($SkipManifests) {
    throw 'SkipManifests 仅用于显式 LocalCandidate，正式交付不可跳过受信签名与递增 sequence。'
} elseif ($TrackedChanges.Count -gt 0 -or $UntrackedSources.Count -gt 0) {
    throw '正式打包要求干净源码，SkipManifests 不能绕过；本地工作区须显式使用 LocalCandidate。'
}
$SourceFingerprint = Get-AwjSourceFingerprint -RepositoryRoot $Repo
if (-not $Version) { $Version = (Get-Content -LiteralPath (Join-Path $Repo "VERSION") -Raw).Trim() }
if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw "VERSION 必须是 MAJOR.MINOR.PATCH。"
}
if (-not $PublishedAtUtc) { $PublishedAtUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ") }
if ($PublishedAtUtc -notmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$') {
    throw "-PublishedAtUtc 必须是 YYYY-MM-DDTHH:MM:SSZ。"
}

function Parse-UtcStamp([string]$Value, [string]$Name) {
    try {
        return [DateTimeOffset]::ParseExact(
            $Value, "yyyy-MM-dd'T'HH:mm:ss'Z'",
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal)
    } catch {
        throw "-$Name 必须是 YYYY-MM-DDTHH:MM:SSZ。"
    }
}

function Get-RepoPath([string]$Path) {
    return Resolve-AwjRepoPath -RepositoryRoot $Repo -Path $Path
}

function Assert-File([string]$Path, [string]$Label) {
    $Resolved = Get-RepoPath $Path
    if (-not (Test-Path -LiteralPath $Resolved -PathType Leaf)) { throw "$Label 不存在: $Resolved" }
    return $Resolved
}

function Assert-AdditionalReleaseAssets([string[]]$EvidencePaths) {
    $withHmos = [version]$Version -ge [version]'1.3.1'
    if (-not $EvidencePaths.Count) {
        if (-not $LocalCandidate -and [version]$Version -ge [version]'1.3.0') {
            throw '正式发布必须提供该版本要求的全部平台 SDK 与对应源码。'
        }
        return
    }
    $expected = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    $expected.Add('sdk:windows-x64', 'AWJ_SDK_Win_x64.zip')
    $expected.Add('sdk:linux-x64', 'AWJ_SDK_Linux_x64.tar.gz')
    $expected.Add('source:source', "AWJimage-$Version-source.zip")
    if ($withHmos) { $expected.Add('sdk:hmos-arm64-api26', 'AWJ_SDK_HMOS_arm64_api26.tar.gz') }
    if ($EvidencePaths.Count -ne $expected.Count) { throw '正式附加资产必须恰好覆盖该版本要求的全部 SDK 与对应源码。' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($path in $EvidencePaths) {
        $receipt = Assert-File $path '附加资产验收记录'
        $proof = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json -AsHashtable
        Assert-AwjJsonFields $proof @('kind','mode','status','version','platform','git_head','source_fingerprint') @('source_dirty')
        if (-not (Test-AwjJsonUnsignedInteger $proof.schema) -or $proof.schema -ne 1 -or
            $proof.mode -cne 'formal' -or $proof.status -cne 'passed' -or $proof.source_dirty -ne $false -or
            $proof.version -cne $Version -or $proof.git_head -cne $GitHead -or $proof.source_fingerprint -cne $SourceFingerprint) {
            throw '附加资产必须通过验收并来自同一干净正式提交。'
        }
        $role = $proof.kind + ':' + $proof.platform
        if (-not $expected.ContainsKey($role) -or -not $seen.Add($role)) { throw '附加资产角色未知或重复。' }
        Assert-AwjJsonFields $proof.archive @('name','sha256')
        if ($proof.archive.name -cne $expected[$role] -or -not (Test-AwjJsonUnsignedInteger $proof.archive.size) -or
            $proof.archive.sha256 -notmatch '^[0-9a-f]{64}$') { throw '附加资产归档信息无效。' }
        $checks = if ($proof.kind -ceq 'sdk') {
            Assert-AwjJsonFields $proof @('test_report_sha256','consumer_report_sha256')
            if ($proof.test_report_sha256 -notmatch '^[0-9a-f]{64}$' -or $proof.consumer_report_sha256 -notmatch '^[0-9a-f]{64}$') {
                throw 'SDK 缺少可追溯的实际测试摘要。'
            }
            $sdkChecks = @('sdk_tests','installed_consumers','exports','archive_roundtrip')
            if ($withHmos) {
                Assert-AwjJsonFields $proof @('profile','compiler_version','target_triple')
                $expectedProfile = switch -CaseSensitive ($proof.platform) {
                    'windows-x64' { @('windows-x64-msvc', '^19\.[0-9]+\.[0-9]+$', 'x86_64-pc-windows-msvc') }
                    'linux-x64' { @('linux-x64-clang23', '^23\.[0-9]+\.[0-9]+$', 'x86_64-pc-linux-gnu') }
                    'hmos-arm64-api26' { @('hmos-arm64-api26-clang23', '^23\.[0-9]+\.[0-9]+$', 'aarch64-unknown-linux-ohos') }
                    default { throw '未知 SDK 目标平台。' }
                }
                if ($proof.profile -cne $expectedProfile[0] -or $proof.compiler_version -cnotmatch $expectedProfile[1] -or
                    $proof.target_triple -cne $expectedProfile[2]) { throw 'SDK 编译器或真实目标与 profile 不匹配。' }
                $sdkChecks += @('compiler_identity','target_identity')
                if ($proof.platform -ceq 'windows-x64') {
                    Assert-AwjMsvcReleaseEvidence $proof.toolchain @('awj_sdk') x64
                    if ($proof.toolchain.compiler_version -cne $proof.compiler_version) { throw 'Windows SDK MSVC 版本绑定不一致。' }
                    $sdkChecks += 'msvc_release_optimization'
                }
            }
            if ($proof.platform -ceq 'hmos-arm64-api26') {
                Assert-AwjJsonFields $proof @('native_sdk_version','libcxx_abi_namespace','target_execution_report_sha256')
                if ($proof.native_sdk_version -cne '26.0.0.105' -or $proof.libcxx_abi_namespace -cne '__n1' -or
                    -not (Test-AwjJsonUnsignedInteger $proof.api) -or $proof.api -ne 26 -or
                    -not (Test-AwjJsonUnsignedInteger $proof.libcxx_version) -or $proof.libcxx_version -ne 15004 -or
                    $proof.target_execution_report_sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'HMOS 目标 SDK、ABI 或执行证据不匹配。' }
                $sdkChecks += @('target_execution','target_loader')
            } else { $sdkChecks += 'relocated_stream' }
            $sdkChecks
        } else { @('source_commit','source_layout') }
        Assert-AwjJsonFields $proof.verification $checks
        foreach ($check in $checks) {
            if ($proof.verification[$check] -cne 'passed') { throw "附加资产缺少通过项：$check" }
        }
        $archive = Assert-File (Join-Path (Split-Path -Parent $receipt) $proof.archive.name) '附加资产归档'
        if ((Get-Item -LiteralPath $archive).Length -ne [UInt64]$proof.archive.size -or
            (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $proof.archive.sha256) {
            throw '附加资产归档与验收记录不匹配。'
        }
        [pscustomobject]@{name=$proof.archive.name; path=$archive; sha256=$proof.archive.sha256; receipt=$receipt;
            receipt_sha256=(Get-FileHash -LiteralPath $receipt -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
}

function Assert-Directory([string]$Path, [string]$Label) {
    $Resolved = Get-RepoPath $Path
    if (-not (Test-Path -LiteralPath $Resolved -PathType Container)) { throw "$Label 不存在: $Resolved" }
    return $Resolved
}

function Assert-BinaryVersion([string]$Path) {
    $Info = [Diagnostics.ProcessStartInfo]::new($Path)
    $Info.UseShellExecute = $false
    $Info.CreateNoWindow = $true
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    $Info.ArgumentList.Add('--version')
    $Process = [Diagnostics.Process]::new()
    $Process.StartInfo = $Info
    try {
        if (-not $Process.Start()) { throw "无法启动版本检查: $Path" }
        $Stdout = $Process.StandardOutput.ReadToEndAsync()
        $Stderr = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(15000)) {
            $Process.Kill($true)
            $Process.WaitForExit()
            throw "版本检查超时: $Path"
        }
        $Actual = $Stdout.GetAwaiter().GetResult().Trim()
        if ($Process.ExitCode -ne 0 -or $Actual -cne "AWJimage $Version") {
            throw "二进制版本不匹配: $Path; expected=AWJimage $Version; actual=$Actual"
        }
    } finally { $Process.Dispose() }
}

function New-VerificationDirectory([string]$Prefix) {
    $Path = Assert-AwjBuildPath -RepositoryRoot $Repo -Path (Join-Path $Repo "build/$Prefix-$([Guid]::NewGuid().ToString('N'))")
    if (Test-Path -LiteralPath $Path) { throw '验证目录必须全新。' }
    [void][IO.Directory]::CreateDirectory($Path)
    return $Path
}

function Get-NormalizedLockHash {
    $Text = (Get-Content -LiteralPath (Join-Path $Repo 'cmake/codec-dependencies.json') -Raw).Replace("`r`n", "`n")
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()
}

function Assert-BuildEvidence([string]$Path, [string]$Platform, [string]$Directory, [string[]]$ExpectedBinaries) {
    $Path = Assert-File $Path "$Platform build evidence"
    $Proof = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
    Assert-AwjJsonFields $Proof @('mode', 'platform', 'version', 'source_fingerprint_algorithm', 'source_fingerprint', 'git_head', 'dependency_lock_sha256') @('source_dirty')
    Assert-AwjJsonFields $Proof.profile @('build_type', 'minimum_cpu') @('ipo')
    Assert-AwjJsonFields $Proof.verification @() @('compiled_source_verified')
    if ($Platform -ceq 'windows-x64') {
        Assert-AwjJsonFields $Proof.profile @() @('static_runtime')
        Assert-AwjJsonFields $Proof.verification @() @('binary_version')
    } else {
        Assert-AwjJsonFields $Proof.profile @('cxx_runtime')
        Assert-AwjJsonFields $Proof.verification @() @('native_execution', 'executable_mode', 'archive_roundtrip')
    }
    if ($Proof.binaries -isnot [Array]) { throw '构建二进制清单必须是数组。' }
    if (-not (Test-AwjJsonUnsignedInteger $Proof.schema) -or $Proof.schema -ne 1 -or $Proof.platform -cne $Platform -or $Proof.version -cne $Version -or
        $Proof.source_fingerprint_algorithm -cne 'sha256/git-normalized-source-v1' -or
        $Proof.source_fingerprint -cne $SourceFingerprint -or $Proof.git_head -notmatch '^[0-9a-f]{40}$' -or
        $Proof.dependency_lock_sha256 -cne (Get-NormalizedLockHash) -or
        $Proof.verification.compiled_source_verified -ne $true -or
        $Proof.profile.build_type -cne 'Release' -or $Proof.profile.ipo -ne $true) {
        throw "$Platform 缺少匹配源码、版本、依赖锁和优化配置的构建证据。"
    }
    if ($LocalCandidate) {
        if ($Proof.mode -cne 'local-candidate') { throw "$Platform 不是明确的本地候选。" }
    } elseif ($Proof.mode -cnotin @('formal', 'clean-candidate') -or $Proof.source_dirty -ne $false -or $Proof.git_head -cne $GitHead) {
        throw "$Platform 正式构建证据必须来自相同干净 HEAD。"
    }
    if ($Platform -ceq 'windows-x64') {
        if ($Proof.profile.minimum_cpu -cne 'AVX2' -or $Proof.profile.static_runtime -ne $true -or $Proof.verification.binary_version -ne $true) {
            throw 'Windows 必须保留 AVX2、静态运行库并通过实际版本检查。'
        }
        if ([version]$Version -ge [version]'1.3.1') {
            Assert-AwjMsvcReleaseEvidence $Proof.toolchain @('AWJ','AWJ-com') AVX2
        }
    } elseif ($Proof.profile.minimum_cpu -cne 'x86-64-v3' -or $Proof.profile.cxx_runtime -cne 'libc++' -or
              $Proof.verification.native_execution -ne $true -or $Proof.verification.executable_mode -ne $true -or
              $Proof.verification.archive_roundtrip -ne $true) {
        throw 'Linux 缺少原生执行、可执行位、归档往返或既定工具链证据。'
    }
    $Names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($Binary in @($Proof.binaries)) {
        Assert-AwjJsonFields $Binary @('path', 'sha256')
        if (-not (Test-AwjJsonUnsignedInteger $Binary.size)) { throw '二进制尺寸必须是非负整数。' }
        $Name = [string]$Binary.path
        if ($Name -cnotin $ExpectedBinaries -or -not $Names.Add($Name) -or $Binary.sha256 -notmatch '^[0-9a-f]{64}$') { throw '构建证据含未知或重复二进制。' }
        $Actual = Assert-File (Join-Path $Directory $Name) "$Platform binary"
        if ((Get-Item -LiteralPath $Actual).Length -ne [UInt64]$Binary.size -or
            (Get-FileHash -LiteralPath $Actual -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Binary.sha256) { throw "二进制与构建证据不匹配：$Actual" }
    }
    if ($Names.Count -ne $ExpectedBinaries.Count) { throw '构建证据缺少二进制。' }
    return $Proof
}

function Assert-VerificationReport([string]$Path, [Parameter(Mandatory)][object[]]$ExpectedArtifacts) {
    $Path = Assert-File $Path 'verification report'
    $Report = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
    Assert-AwjJsonFields $Report @('version', 'source_fingerprint', 'status')
    Assert-AwjJsonFields $Report.checks @('windows_tests', 'linux_tests', 'hdr_math', 'iso_protocol', 'sampling', 'ui', 'original_jxr')
    if (-not (Test-AwjJsonUnsignedInteger $Report.schema) -or $Report.schema -ne 1 -or $Report.version -cne $Version -or $Report.source_fingerprint -cne $SourceFingerprint -or $Report.status -cne 'passed') {
        throw '验收报告必须对应本次稳定源码和版本。'
    }
    foreach ($Name in @('windows_tests', 'linux_tests', 'hdr_math', 'iso_protocol', 'sampling', 'ui', 'original_jxr')) {
        if ($Report.checks[$Name] -cne 'passed') { throw "验收报告缺少通过项：$Name" }
    }
    if ($Report.artifacts -isnot [Array] -or $Report.artifacts.Count -ne $ExpectedArtifacts.Count) { throw '验收报告必须绑定本次全部平台二进制。' }
    $Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($Artifact in $Report.artifacts) {
        Assert-AwjJsonFields $Artifact @('platform', 'path', 'sha256')
        if (-not (Test-AwjJsonUnsignedInteger $Artifact.size) -or $Artifact.sha256 -notmatch '^[0-9a-f]{64}$' -or
            -not $Seen.Add($Artifact.platform + ':' + $Artifact.path)) { throw '验收产物记录非法或重复。' }
    }
    foreach ($Expected in $ExpectedArtifacts) {
        $Found = @($Report.artifacts | Where-Object { $_.platform -ceq $Expected.platform -and $_.path -ceq $Expected.path })
        if ($Found.Count -ne 1 -or $Found[0].sha256 -cne $Expected.sha256 -or $Found[0].size -ne $Expected.size) {
            throw '验收报告对应的不是当前打包二进制，必须重新验收。'
        }
    }
    return $Report
}

function Assert-ArchiveMemberList([string]$ArchivePath, [string[]]$ExpectedFiles) {
    $Listing = @(& 7z.exe l -slt -ba -p- $ArchivePath)
    if ($LASTEXITCODE -ne 0) { throw "无法读取归档成员：$ArchivePath" }
    $Names = @($Listing | Where-Object { $_ -cmatch '^Path = ' } | ForEach-Object { $_.Substring(7) })
    if ($Names.Count -ne $ExpectedFiles.Count -or @($Names | Where-Object { $_ -cnotin $ExpectedFiles }).Count -gt 0 -or
        @($Names | Sort-Object -Unique).Count -ne $Names.Count -or
        @($Listing | Where-Object { $_ -match '^(?:Folder|Encrypted) = \+|^(?:Symbolic Link|Hard Link|Alternate Stream) = .+' }).Count -gt 0) {
        throw "归档含未知路径、目录、链接或加密成员：$ArchivePath"
    }
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $Temporary = "$Path.tmp-$PID"
    [IO.File]::WriteAllText($Temporary, $Text, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $Temporary -Destination $Path -Force
}

function Copy-ExactAlias([string]$Source, [string]$Destination) {
    $Source = Assert-File $Source "alias source"
    if ((Get-RepoPath $Source) -ne (Get-RepoPath $Destination)) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
    $Destination = Assert-File $Destination "alias destination"
    if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash) {
        throw "兼容别名与源文件字节不一致: $Destination"
    }
}

function Get-Asset([string]$Path, [string]$Url) {
    return [ordered]@{
        url = $Url
        size = [UInt64](Get-Item -LiteralPath $Path).Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-ChangelogBody([string]$Path) {
    $Text = (Get-Content -LiteralPath $Path -Raw).Replace("`r`n", "`n")
    $Match = [regex]::Match($Text, "(?ms)^## $([regex]::Escape($Version)) - [^`n]+`n(?<body>.*?)(?=^## |\z)")
    if (-not $Match.Success -or -not $Match.Groups['body'].Value.Trim()) {
        throw "更新日志缺少 $Version 的独立条目: $Path"
    }
    return $Match.Groups['body'].Value.Trim()
}

function Get-SortedEntries($Entries) {
    return @($Entries | Sort-Object {
        $p = ([string]$_.version).Split('.') | ForEach-Object { [UInt32]$_ }
        '{0:D10}.{1:D10}.{2:D10}' -f $p[0], $p[1], $p[2]
    })
}

# A normal stable release declares one target. A prerelease also retains the
# latest stable declaration so stable-only clients still have an upgrade path.
# Keep its revoked flag: do not revive an older target when the latest is revoked.
function Get-CurrentArchiveEntries($PreviousEntries, $Entry) {
    if (@($PreviousEntries | Where-Object { $_.version -eq $Entry.version }).Count) {
        throw "v2 manifest 版本重复。"
    }
    if ($Entry.channel -eq 'prerelease') {
        $Stable = @(Get-SortedEntries @($PreviousEntries | Where-Object { $_.channel -eq 'stable' }))
        if ($Stable.Count) { $Stable[-1] }
    }
    $Entry
}

function Sign-Manifest([string]$ManifestPath, [string]$SignaturePath) {
    if (-not $UpdateSigningSeedFile -or -not (Test-Path -LiteralPath $UpdateSigningSeedFile -PathType Leaf)) {
        throw "签名需要仓库外的 -UpdateSigningSeedFile。"
    }
    if ($UpdatePublicKeyHex -notmatch '^[0-9a-f]{64}$') { throw "需要 64 位小写 -UpdatePublicKeyHex。" }
    & $SignerPath $ManifestPath $UpdateSigningSeedFile $SignaturePath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "manifest 签名失败。" }
    & $SignerPath --verify $ManifestPath $UpdatePublicKeyHex $SignaturePath
    if ($LASTEXITCODE -ne 0) { throw "manifest 签名自检失败。" }
}

function Get-CmakePublicKey([string]$Name) {
    $Cmake = Get-Content -LiteralPath (Join-Path $Repo "CMakeLists.txt") -Raw
    $Match = [regex]::Match($Cmake, '(?m)^set\(' + [regex]::Escape($Name) + '\s+"(?<value>[0-9a-f]{64})"')
    if (-not $Match.Success) { throw "无法从 CMakeLists.txt 读取 $Name。" }
    return $Match.Groups['value'].Value
}

function Assert-TrustedUpdateKeyring([string]$KeyringPath) {
    $SignaturePath = Assert-File "$KeyringPath.sig" "update keyring signature envelope"
    try { $Envelope = Get-Content -LiteralPath $SignaturePath -Raw | ConvertFrom-Json -AsHashtable -DateKind String }
    catch { throw "update keyring 签名封套不是有效 JSON: $SignaturePath" }
    if ($Envelope.schema -ne 1 -or -not $Envelope.signatures -or
        @($Envelope.signatures).Count -lt 2 -or @($Envelope.signatures).Count -gt 3) {
        throw "update keyring 签名封套 schema 或签名数量非法。"
    }
    $Roots = [ordered]@{
        'root-legacy-2026' = Get-CmakePublicKey 'AWJ_UPDATE_PUBLIC_KEY_HEX'
        'root-recovery-a-2026' = Get-CmakePublicKey 'AWJ_UPDATE_ROOT_RECOVERY_A_PUBLIC_KEY_HEX'
        'root-recovery-b-2026' = Get-CmakePublicKey 'AWJ_UPDATE_ROOT_RECOVERY_B_PUBLIC_KEY_HEX'
    }
    $TemporaryDirectory = New-VerificationDirectory 'keyring-verify'
    try {
        $Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($Item in @($Envelope.signatures)) {
            if ($Item.key_id -notmatch '^[a-z0-9-]{1,64}$' -or
                -not $Roots.Contains([string]$Item.key_id) -or
                -not $Seen.Add([string]$Item.key_id) -or
                $Item.signature -isnot [string] -or $Item.signature.Length -gt 256) {
                throw "update keyring 签名封套包含非法、未知或重复的根签名。"
            }
            $SignatureFile = Join-Path $TemporaryDirectory "$($Item.key_id).sig"
            [IO.File]::WriteAllText($SignatureFile, [string]$Item.signature,
                                    [Text.UTF8Encoding]::new($false))
            & $SignerPath --verify $KeyringPath $Roots[[string]$Item.key_id] $SignatureFile | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "update keyring 的 $($Item.key_id) 根签名验证失败。"
            }
        }
    } finally {
        # 保留本次新建的验证证据，不触碰未知目录或旧缓存。
    }
}

function Assert-ManifestSigningKey {
    if ($ManifestKeyId -notmatch '^[a-z0-9-]{1,64}$') {
        throw "-ManifestKeyId 只能包含小写字母、数字和连字符。"
    }
    $KeyringPath = Assert-File $UpdateKeyringPath "update keyring"
    # Authenticate the raw keyring before its contents choose a release key.
    Assert-TrustedUpdateKeyring $KeyringPath
    try { $Keyring = Get-Content -LiteralPath $KeyringPath -Raw | ConvertFrom-Json -AsHashtable -DateKind String }
    catch { throw "update keyring 不是有效 JSON: $KeyringPath" }
    if ($Keyring.schema -ne 1 -or [UInt64]$Keyring.sequence -eq 0 -or
        -not $Keyring.release_keys -or @($Keyring.release_keys).Count -gt 16) {
        throw "update keyring schema、sequence 或 release_keys 非法。"
    }
    $KeyringIssued = Parse-UtcStamp ([string]$Keyring.issued_at) "keyring.issued_at"
    $KeyringExpires = Parse-UtcStamp ([string]$Keyring.expires_at) "keyring.expires_at"
    if ($KeyringExpires -le $KeyringIssued -or
        $KeyringExpires - $KeyringIssued -gt [TimeSpan]::FromDays(180) -or
        $KeyringExpires -le $ManifestIssuedAt -or
        $KeyringIssued -gt $ManifestIssuedAt.AddHours(24)) {
        throw "update keyring 的有效期不适用于本次 manifest。"
    }
    $Key = @($Keyring.release_keys | Where-Object { $_.key_id -eq $ManifestKeyId })
    if ($Key.Count -ne 1 -or $Key[0].revoked -eq $true -or
        $Key[0].public_key -cne $UpdatePublicKeyHex) {
        throw "-ManifestKeyId 必须引用 keyring 中唯一、未撤销且与 -UpdatePublicKeyHex 匹配的发布密钥。"
    }
    $KeyNotBefore = Parse-UtcStamp ([string]$Key[0].not_before) "release_keys.not_before"
    $KeyExpires = Parse-UtcStamp ([string]$Key[0].expires_at) "release_keys.expires_at"
    if ($KeyExpires -le $KeyNotBefore -or
        $KeyExpires - $KeyNotBefore -gt [TimeSpan]::FromDays(366) -or
        $ManifestIssuedAt -lt $KeyNotBefore -or $ManifestIssuedAt -ge $KeyExpires) {
        throw "-ManifestKeyId 在本次 manifest 的发布时间无效。"
    }
}

function Get-ArchiveMembers([string]$Directory, [string]$ArchiveUrl) {
    return @(
        Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName | ForEach-Object {
            $Relative = $_.FullName.Substring($Directory.Length).TrimStart('\', '/').Replace('\', '/')
            [ordered]@{ path = $Relative; url = $ArchiveUrl; size = [UInt64]$_.Length;
                         sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
}

function Add-Archive([string]$PackageDirectory, [string]$ArchivePath) {
    Push-Location $PackageDirectory
    try {
        & 7z.exe a -t7z $ArchivePath .\* -m0=lzma2 -mx=9 -mmt=1 -mf=off -mtc=off -mta=off -mtm=off | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "7z 创建失败: $ArchivePath" }
    } finally { Pop-Location }
    & 7z.exe t $ArchivePath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "7z 完整性检查失败: $ArchivePath" }
}

function Assert-ExactPackage([string]$Directory, [string[]]$ExpectedFiles) {
    $Directories = @(Get-ChildItem -LiteralPath $Directory -Directory -Recurse)
    if ($Directories.Count -ne 0) {
        throw "发行包必须是扁平结构，不允许子目录: $Directory"
    }
    $Actual = @(Get-ChildItem -LiteralPath $Directory -File -Recurse | ForEach-Object {
        $_.FullName.Substring($Directory.Length).TrimStart('\', '/').Replace('\', '/')
    } | Sort-Object)
    $Expected = @($ExpectedFiles | Sort-Object)
    if (@(Compare-Object $Expected $Actual).Count -gt 0) {
        throw "发行包成员不符合固定结构: $Directory; expected=$($Expected -join ','); actual=$($Actual -join ',')"
    }
}

function Assert-ArchiveRoundTrip([string]$ArchivePath, [string]$PackageDirectory, [string]$VerifyDirectory) {
    $VerifyDirectory = Assert-AwjBuildPath -RepositoryRoot $Repo -Path $VerifyDirectory
    if (Test-Path -LiteralPath $VerifyDirectory) { throw '归档验证目录必须全新。' }
    $OriginalFiles = @(Get-ChildItem -LiteralPath $PackageDirectory -File -Recurse)
    Assert-ArchiveMemberList $ArchivePath @($OriginalFiles | ForEach-Object { $_.Name })
    [void][IO.Directory]::CreateDirectory($VerifyDirectory)
    & 7z.exe x $ArchivePath "-o$VerifyDirectory" -p- -y | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "7z 解压验证失败: $ArchivePath" }
    $ExtractedFiles = @(Get-ChildItem -LiteralPath $VerifyDirectory -File -Recurse)
    $OriginalNames = @($OriginalFiles | ForEach-Object { $_.FullName.Substring($PackageDirectory.Length).TrimStart('\', '/') } | Sort-Object)
    $ExtractedNames = @($ExtractedFiles | ForEach-Object { $_.FullName.Substring($VerifyDirectory.Length).TrimStart('\', '/') } | Sort-Object)
    if (@(Compare-Object $OriginalNames $ExtractedNames).Count -gt 0) {
        throw "归档包含缺失或额外成员: $ArchivePath"
    }
    foreach ($Original in $OriginalFiles) {
        $Relative = $Original.FullName.Substring($PackageDirectory.Length).TrimStart('\', '/')
        $Extracted = Join-Path $VerifyDirectory $Relative
        if (-not (Test-Path -LiteralPath $Extracted -PathType Leaf) -or
            (Get-FileHash -LiteralPath $Original.FullName -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $Extracted -Algorithm SHA256).Hash) {
            throw "归档往返哈希不匹配: $Relative"
        }
    }
}

$WindowsExePath = Assert-File $WindowsExePath "AWJ.exe"
$WindowsComPath = Assert-File $WindowsComPath "AWJ.com"
if ($Version -cne (Get-Content -LiteralPath (Join-Path $Repo 'VERSION') -Raw).Trim()) {
    throw '打包版本与仓库 VERSION 不一致。'
}
if ((Split-Path -Parent $WindowsExePath) -ne (Split-Path -Parent $WindowsComPath)) { throw 'Windows EXE/COM 必须来自同一隔离构建目录。' }
Assert-BinaryVersion $WindowsExePath
Assert-BinaryVersion $WindowsComPath
if (-not $LocalCandidate) {
    $ActualTag = (git -C $Repo describe --exact-match --tags HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $ActualTag -cne $Version) { throw '正式打包必须位于匹配版本的 tag，SkipManifests 不能绕过。' }
}
$LinuxPackagePath = Assert-Directory $LinuxPackagePath "native Linux package"
$LinuxArchivePath = Assert-File $LinuxArchivePath "native Linux archive"
if (([IO.Path]::GetFileName($WindowsExePath) -ne "AWJ.exe") -or
    ([IO.Path]::GetFileName($WindowsComPath) -ne "AWJ.com")) {
    throw "输入文件名必须严格为 AWJ.exe 和 AWJ.com。"
}
if (-not (Test-Path -LiteralPath (Join-Path $LinuxPackagePath "AWJ") -PathType Leaf)) {
    throw "native Linux package 必须包含 AWJ。"
}
if (-not (Get-Command 7z.exe -ErrorAction SilentlyContinue)) { throw "未找到 7z.exe。" }
$WindowsSourceDirectory = Split-Path -Parent $WindowsExePath
if (-not $WindowsEvidencePath) { $WindowsEvidencePath = Join-Path $WindowsSourceDirectory 'build-evidence.json' }
if (-not $LinuxEvidencePath) { $LinuxEvidencePath = Join-Path (Split-Path -Parent (Split-Path -Parent $LinuxPackagePath)) 'build-evidence.json' }
$WindowsEvidencePath = Assert-File $WindowsEvidencePath 'Windows build evidence'
$LinuxEvidencePath = Assert-File $LinuxEvidencePath 'Linux build evidence'
$VerificationReportPath = Assert-File $VerificationReportPath 'verification report'
$WindowsProof = Assert-BuildEvidence $WindowsEvidencePath 'windows-x64' $WindowsSourceDirectory @('AWJ.exe', 'AWJ.com')
$LinuxProof = Assert-BuildEvidence $LinuxEvidencePath 'linux-x64' $LinuxPackagePath @('AWJ')
Assert-AwjJsonFields $LinuxProof.archive @('path', 'sha256')
if (-not (Test-AwjJsonUnsignedInteger $LinuxProof.archive.size)) { throw 'Linux 归档尺寸必须是非负整数。' }
if ($LinuxProof.archive.path -cne 'AWJ_Linux.7z' -or $LinuxProof.archive.sha256 -notmatch '^[0-9a-f]{64}$' -or
    (Get-Item -LiteralPath $LinuxArchivePath).Length -ne [UInt64]$LinuxProof.archive.size -or
    (Get-FileHash -LiteralPath $LinuxArchivePath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $LinuxProof.archive.sha256) {
    throw 'Linux 归档与原生验证证据不匹配。'
}
$ExpectedVerifiedArtifacts = @(
    foreach ($Binary in $WindowsProof.binaries) { [ordered]@{platform='windows-x64'; path=$Binary.path; size=$Binary.size; sha256=$Binary.sha256} }
    foreach ($Binary in $LinuxProof.binaries) { [ordered]@{platform='linux-x64'; path=$Binary.path; size=$Binary.size; sha256=$Binary.sha256} }
)
$TestReport = Assert-VerificationReport $VerificationReportPath $ExpectedVerifiedArtifacts
$AdditionalAssets = @(Assert-AdditionalReleaseAssets $AdditionalAssetEvidencePaths)
if (-not $SkipManifests) {
    if (-not $UpdateKeyringPath) {
        $UpdateKeyringPath = Join-Path $Repo 'update-keyring.json'
    }
    if (-not $SignerPath) { $SignerPath = Join-Path (Split-Path -Parent $WindowsExePath) "awj_update_manifest_sign.exe" }
    $SignerPath = Assert-File $SignerPath "manifest 签名工具"
    if ($UpdatePublicKeyHex -notmatch '^[0-9a-f]{64}$') { throw "需要 64 位小写 -UpdatePublicKeyHex。" }
    if (-not $ManifestExpiresAtUtc) { throw "签名 manifest 需要 -ManifestExpiresAtUtc。" }
    $ManifestIssuedAt = Parse-UtcStamp $PublishedAtUtc "PublishedAtUtc"
    $ManifestExpiresAt = Parse-UtcStamp $ManifestExpiresAtUtc "ManifestExpiresAtUtc"
    if ($ManifestExpiresAt -le $ManifestIssuedAt -or
        $ManifestExpiresAt - $ManifestIssuedAt -gt [TimeSpan]::FromDays(180)) {
        throw "manifest 有效期必须为 1 秒到 180 天。"
    }
    if (-not $ExistingManifestPublicKeyHex) { $ExistingManifestPublicKeyHex = $UpdatePublicKeyHex }
    if ($ExistingManifestPublicKeyHex -notmatch '^[0-9a-f]{64}$') {
        throw "-ExistingManifestPublicKeyHex 必须是 64 位小写十六进制。"
    }
    Assert-ManifestSigningKey

    $CanonicalKeyringPath = Join-Path $Repo "update-keyring.json"
    Copy-ExactAlias $UpdateKeyringPath $CanonicalKeyringPath
    Copy-ExactAlias "$UpdateKeyringPath.sig" "$CanonicalKeyringPath.sig"
}

$Stage = New-AwjReleaseStage -RepositoryRoot $Repo -Version $Version
$Assets = $Stage
$WinPackage = Join-Path $Stage 'Windows'
$LinuxPackage = Join-Path $Stage 'Linux'
$Verification = Join-Path $Stage 'verification'
New-Item -ItemType Directory -Path $WinPackage, $LinuxPackage, $Verification | Out-Null
Assert-ExactPackage $LinuxPackagePath @('AWJ', 'LICENSE', 'NOTICE.txt')
foreach ($Name in @('AWJ', 'LICENSE', 'NOTICE.txt')) {
    Copy-Item -LiteralPath (Assert-File (Join-Path $LinuxPackagePath $Name) "Linux $Name") -Destination (Join-Path $LinuxPackage $Name)
}
Copy-Item -LiteralPath $WindowsEvidencePath -Destination (Join-Path $Verification 'windows-build.json')
Copy-Item -LiteralPath $LinuxEvidencePath -Destination (Join-Path $Verification 'linux-build.json')
Copy-Item -LiteralPath $VerificationReportPath -Destination (Join-Path $Verification 'tests.json')
$AdditionalVerificationFiles = @(
    foreach ($Asset in $AdditionalAssets) {
        $ReceiptName = "verification/$($Asset.name).json"
        Copy-Item -LiteralPath $Asset.path -Destination (Join-Path $Stage $Asset.name)
        Copy-Item -LiteralPath $Asset.receipt -Destination (Join-Path $Stage $ReceiptName)
        if ((Get-FileHash -LiteralPath (Join-Path $Stage $Asset.name) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Asset.sha256 -or
            (Get-FileHash -LiteralPath (Join-Path $Stage $ReceiptName) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Asset.receipt_sha256) {
            throw '附加资产在正式收口期间变化。'
        }
        $ReceiptName
    }
)

$WindowsSourceDirectory = Split-Path -Parent $WindowsExePath
$WindowsLicensePath = Assert-File (Join-Path $WindowsSourceDirectory "LICENSE") "Windows LICENSE"
$WindowsNoticePath = Assert-File (Join-Path $WindowsSourceDirectory "NOTICE.txt") "Windows NOTICE.txt"
Copy-Item -LiteralPath $WindowsExePath, $WindowsComPath, $WindowsLicensePath, $WindowsNoticePath -Destination $WinPackage -Force
Assert-ExactPackage $WinPackage @("AWJ.exe", "AWJ.com", "LICENSE", "NOTICE.txt")
Assert-ExactPackage $LinuxPackage @("AWJ", "LICENSE", "NOTICE.txt")

$WindowsArchive = Join-Path $Assets "AWJ_Win.7z"
$LinuxArchive = Join-Path $Assets "AWJ_Linux.7z"
Add-Archive $WinPackage $WindowsArchive
Copy-Item -LiteralPath $LinuxArchivePath -Destination $LinuxArchive -Force
& 7z.exe t $LinuxArchive | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Linux 7z 完整性检查失败: $LinuxArchive" }
$VerifyWork = New-VerificationDirectory 'release-roundtrip'
$WinVerify = Join-Path $VerifyWork 'Windows'
$LinuxVerify = Join-Path $VerifyWork 'Linux'
Assert-ArchiveRoundTrip $WindowsArchive $WinPackage $WinVerify
Assert-BinaryVersion (Join-Path $WinVerify 'AWJ.exe')
Assert-ArchiveRoundTrip $LinuxArchive $LinuxPackage $LinuxVerify
$GuiSmoke = Start-Process -FilePath (Join-Path $WinVerify "AWJ.exe") -ArgumentList "--help" -PassThru -Wait -WindowStyle Hidden
if ($GuiSmoke.ExitCode -ne 0) { throw "Windows AWJ.exe --help smoke 失败。" }
& (Join-Path $WinVerify "AWJ.com") --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Windows AWJ.com --help smoke 失败。" }

if (-not $SkipManifests) {
    $ReleaseBase = "https://github.com/Dominic485649/AWJimage/releases"
    $Changelog = [ordered]@{ 'zh-CN' = Get-ChangelogBody (Join-Path $Repo "CHANGELOG.md"); en = Get-ChangelogBody (Join-Path $Repo "CHANGELOG.en.md") }
        if ($ArchiveManifestSequence -eq 0) { throw "归档更新需要 -ArchiveManifestSequence。" }
        $ManifestPath = Join-Path $Repo "update-archive.json"
        $SignaturePath = "$ManifestPath.sig"
        $ExistingManifestPath = $ManifestPath
        $ExistingSignaturePath = "$ExistingManifestPath.sig"
        $Old = if (Test-Path -LiteralPath $ExistingManifestPath) { Get-Content -LiteralPath $ExistingManifestPath -Raw | ConvertFrom-Json -AsHashtable } else { $null }
        if ($Old) {
            & $SignerPath --verify $ExistingManifestPath $ExistingManifestPublicKeyHex $ExistingSignaturePath
            if ($LASTEXITCODE -ne 0) { throw "现有 v2 manifest 签名无效。" }
            if ($ArchiveManifestSequence -le [UInt64]$Old.sequence) { throw "v2 sequence 必须递增。" }
        }
        $WinUrl = "$ReleaseBase/download/$Version/AWJ_Win.7z"
        $LinuxUrl = "$ReleaseBase/download/$Version/AWJ_Linux.7z"
        $Entry = [ordered]@{
            version = $Version; channel = $Channel; published_at = $PublishedAtUtc
            release_url = "$ReleaseBase/tag/$Version"; minimum_updater_version = "1.0.13"; revoked = $false
            assets = [ordered]@{
                windows_x64_archive = [ordered]@{ archive = Get-Asset $WindowsArchive $WinUrl; members = Get-ArchiveMembers $WinPackage $WinUrl }
                linux_x64_archive = [ordered]@{ archive = Get-Asset $LinuxArchive $LinuxUrl; members = Get-ArchiveMembers $LinuxPackage $LinuxUrl }
            }
            changelog = $Changelog
        }
        [object[]]$PreviousEntries = if ($Old) { @($Old.entries) } else { @() }
        $SortedEntries = [object[]]@(Get-SortedEntries @(
            Get-CurrentArchiveEntries $PreviousEntries $Entry))
        $Json = ([ordered]@{ schema = 2; sequence = $ArchiveManifestSequence; key_id = $ManifestKeyId; issued_at = $PublishedAtUtc; expires_at = $ManifestExpiresAtUtc; entries = $SortedEntries } | ConvertTo-Json -Depth 32).Replace("`r`n", "`n") + "`n"
        if (-not ((ConvertFrom-Json -InputObject $Json).entries -is [System.Array])) { throw "v2 manifest entries 必须序列化为数组。" }
        Write-Utf8NoBom $ManifestPath $Json
        Sign-Manifest $ManifestPath $SignaturePath

}

if ((Get-AwjSourceFingerprint -RepositoryRoot $Repo) -cne $SourceFingerprint -or (git -C $Repo rev-parse HEAD).Trim() -cne $GitHead) {
    throw '源码在打包期间变化，保留 staging，不投放、不清理旧版本。'
}
$HashFiles = @('Windows/AWJ.exe', 'Windows/AWJ.com', 'Windows/LICENSE', 'Windows/NOTICE.txt', 'Linux/AWJ', 'Linux/LICENSE', 'Linux/NOTICE.txt', 'AWJ_Win.7z', 'AWJ_Linux.7z') + @($AdditionalAssets | ForEach-Object { $_.name })
$HashLines = foreach ($Relative in $HashFiles) {
    $Hash = (Get-FileHash -LiteralPath (Join-Path $Stage $Relative) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$Hash  $Relative"
}
Write-Utf8NoBom (Join-Path $Stage 'SHA256SUMS') (($HashLines -join "`n") + "`n")
$Artifacts = foreach ($Relative in ($HashFiles + @('SHA256SUMS', '.awj-owned-stage.json', 'verification/windows-build.json', 'verification/linux-build.json', 'verification/tests.json') + $AdditionalVerificationFiles)) {
    $Path = Join-Path $Stage $Relative
    [ordered]@{path=$Relative; size=[UInt64](Get-Item -LiteralPath $Path).Length; sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
[ordered]@{
    schema=1; version=$Version; status='passed'; mode=$(if ($LocalCandidate) { 'local-candidate' } else { 'formal' })
    source_fingerprint=$SourceFingerprint; accepted_at=[DateTimeOffset]::UtcNow.ToString('o'); artifacts=@($Artifacts)
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Verification 'acceptance.json') -Encoding utf8
$Destination = Publish-AwjVersionStage -RepositoryRoot $Repo -Version $Version -Stage $Stage
Assert-BinaryVersion (Join-Path $Destination 'Windows/AWJ.exe')
Assert-BinaryVersion (Join-Path $Destination 'Windows/AWJ.com')
$null = Test-AwjArtifactAcceptance -RepositoryRoot $Repo -Directory $Destination -Version $Version
$CleanupPlan = New-AwjReleaseCleanupPlan -RepositoryRoot $Repo -Version $Version
$CleanupPlan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $Destination 'verification/cleanup-plan.json') -Encoding utf8
$BackupResults = @(Backup-AwjReleaseUserData -RepositoryRoot $Repo -Plan $CleanupPlan)
[ordered]@{schema=1; mode='backup-only'; cleanup='not-executed'; files=@($BackupResults)} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $Destination 'verification/backup-results.json') -Encoding utf8
[ordered]@{version=$Version; mode=$(if ($LocalCandidate) { 'local-candidate' } else { 'formal' }); directory=$Destination; source_fingerprint=$SourceFingerprint; cleanup='permission-required'} | ConvertTo-Json -Compress
