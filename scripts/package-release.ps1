[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$WindowsExePath,
    [Parameter(Mandatory)] [string]$WindowsComPath,
    [Parameter(Mandatory)] [string]$LinuxPackagePath,
    [Parameter(Mandatory)] [string]$LinuxArchivePath,
    [string]$Version = "",
    [ValidateSet("stable", "prerelease")] [string]$Channel = "prerelease",
    [UInt64]$ArchiveManifestSequence = 0,
    [UInt64]$LegacyManifestSequence = 0,
    [switch]$BridgeRelease,
    [string]$PublishedAtUtc = "",
    [string]$UpdateSigningSeedFile = $env:AWJ_UPDATE_SIGNING_SEED_FILE,
    [string]$UpdatePublicKeyHex = $env:AWJ_UPDATE_PUBLIC_KEY_HEX,
    [string]$ManifestKeyId = "",
    [string]$ManifestExpiresAtUtc = "",
    [string]$UpdateKeyringPath = "",
    [string]$ExistingManifestPublicKeyHex = "",
    [string]$SignerPath = "",
    [switch]$SkipManifests,
    [string]$WindowsShellExtensionPath = "",
    [string]$WindowsSparsePackagePath = ""
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
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
    return [IO.Path]::GetFullPath($Path)
}

function Assert-File([string]$Path, [string]$Label) {
    $Resolved = Get-RepoPath $Path
    if (-not (Test-Path -LiteralPath $Resolved -PathType Leaf)) { throw "$Label 不存在: $Resolved" }
    return $Resolved
}

function Assert-Directory([string]$Path, [string]$Label) {
    $Resolved = Get-RepoPath $Path
    if (-not (Test-Path -LiteralPath $Resolved -PathType Container)) { throw "$Label 不存在: $Resolved" }
    return $Resolved
}

function Remove-OnlyInsideRepo([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $Root = (Get-RepoPath $Repo).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $Resolved = Get-RepoPath $Path
    if (-not $Resolved.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝删除仓库外目录: $Resolved"
    }
    Remove-Item -LiteralPath $Resolved -Recurse -Force
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $Temporary = "$Path.tmp-$PID"
    [IO.File]::WriteAllText($Temporary, $Text, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $Temporary -Destination $Path -Force
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
    $TemporaryDirectory = Join-Path $Repo "build\keyring-verify-$PID"
    Remove-OnlyInsideRepo $TemporaryDirectory
    New-Item -ItemType Directory -Path $TemporaryDirectory -Force | Out-Null
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
        Remove-OnlyInsideRepo $TemporaryDirectory
    }
}

function Assert-ManifestSigningKey {
    if ($ManifestKeyId -notmatch '^[a-z0-9-]{1,64}$') {
        throw "-ManifestKeyId 只能包含小写字母、数字和连字符。"
    }
    if (-not $UpdateKeyringPath) { $UpdateKeyringPath = Join-Path $Repo "update-keyring-v1.json" }
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
    if ((Compare-Object $Expected $Actual).Count) {
        throw "发行包成员不符合固定结构: $Directory; expected=$($Expected -join ','); actual=$($Actual -join ',')"
    }
}

function Assert-ArchiveRoundTrip([string]$ArchivePath, [string]$PackageDirectory, [string]$VerifyDirectory) {
    Remove-OnlyInsideRepo $VerifyDirectory
    New-Item -ItemType Directory -Path $VerifyDirectory -Force | Out-Null
    & 7z.exe x $ArchivePath "-o$VerifyDirectory" -y | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "7z 解压验证失败: $ArchivePath" }
    $OriginalFiles = @(Get-ChildItem -LiteralPath $PackageDirectory -File -Recurse)
    $ExtractedFiles = @(Get-ChildItem -LiteralPath $VerifyDirectory -File -Recurse)
    $OriginalNames = @($OriginalFiles | ForEach-Object { $_.FullName.Substring($PackageDirectory.Length).TrimStart('\', '/') } | Sort-Object)
    $ExtractedNames = @($ExtractedFiles | ForEach-Object { $_.FullName.Substring($VerifyDirectory.Length).TrimStart('\', '/') } | Sort-Object)
    if ((Compare-Object $OriginalNames $ExtractedNames).Count) {
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
if (-not $WindowsShellExtensionPath) {
    $WindowsShellExtensionPath = Join-Path (Split-Path -Parent $WindowsExePath) "AWJ.ShellExtension.dll"
}
$WindowsShellExtensionPath = Assert-File $WindowsShellExtensionPath "AWJ.ShellExtension.dll"
if (-not $WindowsSparsePackagePath) {
    $WindowsSparsePackagePath = Join-Path (Split-Path -Parent $WindowsExePath) "AWJ.ContextMenu.msix"
}
$WindowsSparsePackagePath = Assert-File $WindowsSparsePackagePath "AWJ.ContextMenu.msix"
$LinuxPackagePath = Assert-Directory $LinuxPackagePath "native Linux package"
$LinuxArchivePath = Assert-File $LinuxArchivePath "native Linux archive"
if (([IO.Path]::GetFileName($WindowsExePath) -ne "AWJ.exe") -or
    ([IO.Path]::GetFileName($WindowsComPath) -ne "AWJ.com") -or
    ([IO.Path]::GetFileName($WindowsShellExtensionPath) -ne "AWJ.ShellExtension.dll") -or
    ([IO.Path]::GetFileName($WindowsSparsePackagePath) -ne "AWJ.ContextMenu.msix")) {
    throw "输入文件名必须严格为 AWJ.exe、AWJ.com、AWJ.ShellExtension.dll 和 AWJ.ContextMenu.msix。"
}
if (-not (Test-Path -LiteralPath (Join-Path $LinuxPackagePath "AWJ") -PathType Leaf)) {
    throw "native Linux package 必须包含 AWJ。"
}
if (-not (Get-Command 7z.exe -ErrorAction SilentlyContinue)) { throw "未找到 7z.exe。" }
if (-not $SkipManifests) {
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
}

$Stage = Join-Path $Repo "build\release\$Version"
Remove-OnlyInsideRepo $Stage
$Assets = Join-Path $Stage "assets"
$WinPackage = Join-Path $Stage "package\AWJ_Win"
$LinuxPackage = $LinuxPackagePath
New-Item -ItemType Directory -Path $Assets, $WinPackage -Force | Out-Null

$WindowsSourceDirectory = Split-Path -Parent $WindowsExePath
$WindowsLicensePath = Assert-File (Join-Path $WindowsSourceDirectory "LICENSE") "Windows LICENSE"
$WindowsNoticePath = Assert-File (Join-Path $WindowsSourceDirectory "NOTICE.txt") "Windows NOTICE.txt"
Copy-Item -LiteralPath $WindowsExePath, $WindowsComPath, $WindowsShellExtensionPath, $WindowsSparsePackagePath, $WindowsLicensePath, $WindowsNoticePath -Destination $WinPackage -Force
Assert-ExactPackage $WinPackage @("AWJ.exe", "AWJ.com", "AWJ.ShellExtension.dll", "AWJ.ContextMenu.msix", "LICENSE", "NOTICE.txt")
Assert-ExactPackage $LinuxPackage @("AWJ", "LICENSE", "NOTICE.txt")

$WindowsArchive = Join-Path $Assets "AWJ_Win.7z"
$LinuxArchive = Join-Path $Assets "AWJ_Linux.7z"
Add-Archive $WinPackage $WindowsArchive
Copy-Item -LiteralPath $LinuxArchivePath -Destination $LinuxArchive -Force
& 7z.exe t $LinuxArchive | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Linux 7z 完整性检查失败: $LinuxArchive" }
$WinVerify = Join-Path $Stage "verify\AWJ_Win"
$LinuxVerify = Join-Path $Stage "verify\AWJ_Linux"
Assert-ArchiveRoundTrip $WindowsArchive $WinPackage $WinVerify
Assert-ArchiveRoundTrip $LinuxArchive $LinuxPackage $LinuxVerify
$GuiSmoke = Start-Process -FilePath (Join-Path $WinVerify "AWJ.exe") -ArgumentList "--help" -PassThru -Wait -WindowStyle Hidden
if ($GuiSmoke.ExitCode -ne 0) { throw "Windows AWJ.exe --help smoke 失败。" }
& (Join-Path $WinVerify "AWJ.com") --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Windows AWJ.com --help smoke 失败。" }
$ShellExtensionVerifyPath = Join-Path $WinVerify "AWJ.ShellExtension.dll"
$ShellExtensionModule = [Runtime.InteropServices.NativeLibrary]::Load($ShellExtensionVerifyPath)
try {
    foreach ($Export in @("DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer", "DllUnregisterServer")) {
        [void][Runtime.InteropServices.NativeLibrary]::GetExport($ShellExtensionModule, $Export)
    }
} finally {
    [Runtime.InteropServices.NativeLibrary]::Free($ShellExtensionModule)
}
$SparsePackageVerifyPath = Join-Path $WinVerify "AWJ.ContextMenu.msix"
if (-not (Test-Path -LiteralPath $SparsePackageVerifyPath -PathType Leaf)) {
    throw "Windows 归档往返验证缺少 AWJ.ContextMenu.msix。"
}
if ($BridgeRelease) {
    Copy-Item -LiteralPath $WindowsExePath, $WindowsComPath -Destination $Assets -Force
}

if (-not $SkipManifests) {
    $ReleaseBase = "https://github.com/Dominic485649/AWJimage/releases"
    $Changelog = [ordered]@{ 'zh-CN' = Get-ChangelogBody (Join-Path $Repo "CHANGELOG.md"); en = Get-ChangelogBody (Join-Path $Repo "CHANGELOG.en.md") }
    if (-not $BridgeRelease) {
        if ($ArchiveManifestSequence -eq 0) { throw "1.0.4 归档更新需要 -ArchiveManifestSequence。" }
        $ManifestPath = Join-Path $Repo "update-manifest-v2.json"
        $SignaturePath = "$ManifestPath.sig"
        $Old = if (Test-Path -LiteralPath $ManifestPath) { Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -AsHashtable } else { $null }
        if ($Old) {
            & $SignerPath --verify $ManifestPath $ExistingManifestPublicKeyHex $SignaturePath
            if ($LASTEXITCODE -ne 0) { throw "现有 v2 manifest 签名无效。" }
            if ($ArchiveManifestSequence -le [UInt64]$Old.sequence) { throw "v2 sequence 必须递增。" }
        }
        $WinUrl = "$ReleaseBase/download/$Version/AWJ_Win.7z"
        $LinuxUrl = "$ReleaseBase/download/$Version/AWJ_Linux.7z"
        $Entry = [ordered]@{
            version = $Version; channel = $Channel; published_at = $PublishedAtUtc
            release_url = "$ReleaseBase/tag/$Version"; minimum_updater_version = "1.0.4"; revoked = $false
            assets = [ordered]@{
                windows_x64_archive = [ordered]@{ archive = Get-Asset $WindowsArchive $WinUrl; members = Get-ArchiveMembers $WinPackage $WinUrl }
                linux_x64_archive = [ordered]@{ archive = Get-Asset $LinuxArchive $LinuxUrl; members = Get-ArchiveMembers $LinuxPackage $LinuxUrl }
            }
            changelog = $Changelog
        }
        [object[]]$Entries = if ($Old) { @($Old.entries) } else { @() }
        $Entries += ,$Entry
        if (@($Entries | Where-Object { $_.version -eq $Version }).Count -ne 1) { throw "v2 manifest 版本重复。" }
        $SortedEntries = [object[]]@(Get-SortedEntries $Entries)
        $Json = ([ordered]@{ schema = 2; sequence = $ArchiveManifestSequence; key_id = $ManifestKeyId; issued_at = $PublishedAtUtc; expires_at = $ManifestExpiresAtUtc; entries = $SortedEntries } | ConvertTo-Json -Depth 32).Replace("`r`n", "`n") + "`n"
        if (-not ((ConvertFrom-Json -InputObject $Json).entries -is [System.Array])) { throw "v2 manifest entries 必须序列化为数组。" }
        Write-Utf8NoBom $ManifestPath $Json
        Sign-Manifest $ManifestPath $SignaturePath
    }
    if ($BridgeRelease) {
        if ($LegacyManifestSequence -eq 0) { throw "1.0.5 桥接更新需要 -LegacyManifestSequence。" }
        $LegacyPath = Join-Path $Repo "update-manifest.json"
        $LegacySignature = "$LegacyPath.sig"
        & $SignerPath --verify $LegacyPath $ExistingManifestPublicKeyHex $LegacySignature
        if ($LASTEXITCODE -ne 0) { throw "现有 v1 manifest 签名无效。" }
        $Old = Get-Content -LiteralPath $LegacyPath -Raw | ConvertFrom-Json -AsHashtable
        if ($Old.schema -ne 1 -or $LegacyManifestSequence -le [UInt64]$Old.sequence) { throw "v1 sequence 必须递增。" }
        $Entry = [ordered]@{
            version = $Version; channel = $Channel; published_at = $PublishedAtUtc
            release_url = "$ReleaseBase/tag/$Version"; minimum_updater_version = "1.0.1"; revoked = $false
            assets = [ordered]@{
                windows_x64_exe = Get-Asset (Join-Path $Assets "AWJ.exe") "$ReleaseBase/download/$Version/AWJ.exe"
                windows_x64_com = Get-Asset (Join-Path $Assets "AWJ.com") "$ReleaseBase/download/$Version/AWJ.com"
                linux_x64 = Get-Asset $LinuxArchive "$ReleaseBase/download/$Version/AWJ_Linux.7z"
            }
            changelog = $Changelog
        }
        [object[]]$Entries = @($Old.entries)
        $Entries += ,$Entry
        if (@($Entries | Where-Object { $_.version -eq $Version }).Count -ne 1) { throw "v1 manifest 版本重复。" }
        $SortedEntries = [object[]]@(Get-SortedEntries $Entries)
        $Json = ([ordered]@{ schema = 1; sequence = $LegacyManifestSequence; key_id = $ManifestKeyId; issued_at = $PublishedAtUtc; expires_at = $ManifestExpiresAtUtc; entries = $SortedEntries } | ConvertTo-Json -Depth 20).Replace("`r`n", "`n") + "`n"
        if (-not ((ConvertFrom-Json -InputObject $Json).entries -is [System.Array])) { throw "v1 manifest entries 必须序列化为数组。" }
        Write-Utf8NoBom $LegacyPath $Json
        Sign-Manifest $LegacyPath $LegacySignature
    }
}

Get-ChildItem -LiteralPath $Assets -File | Select-Object Name, Length, @{n='SHA256'; e={(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}} | Format-Table -AutoSize
Write-Host "Release stage: $Stage"
