Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-AwjPathComparison {
    if ($IsWindows) { return [StringComparison]::OrdinalIgnoreCase }
    return [StringComparison]::Ordinal
}

function Test-AwjPathUnder([string]$Root, [string]$Path) {
    $r = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $p = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $comparison = Get-AwjPathComparison
    return $p.Equals($r, $comparison) -or $p.StartsWith($r + [IO.Path]::DirectorySeparatorChar, $comparison)
}

function Test-AwjSensitiveName([string]$Path) {
    $name = [IO.Path]::GetFileName($Path)
    return $name -match '(?i)(?:^|[-_.])(?:secrets?|seed|private[-_]?key)(?:[-_.]|$)|\.(?:pem|pfx|key|seed)$|update-ed25519.*\.(?:hex|seed)$|^release-signing\.local\.json$'
}

function Resolve-AwjRepoPath {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot,
          [Parameter(Mandatory)][string]$Path,
          [switch]$AllowRepositoryRoot)
    $root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $absolute = [IO.Path]::GetFullPath($Path, $root)
    $comparison = Get-AwjPathComparison
    if (-not (Test-AwjPathUnder $root $absolute)) { throw "拒绝仓库外路径：$absolute" }
    if (-not $AllowRepositoryRoot -and $absolute.TrimEnd('\', '/').Equals($root, $comparison)) { throw '不能把仓库根目录作为输出或清理目标。' }
    $protected = Join-Path $root 'bin/x64'
    if (Test-AwjPathUnder $protected $absolute) { throw "bin/x64 由用户手动管理，拒绝写入或清理：$absolute" }
    if (Test-AwjSensitiveName $absolute) { throw "拒绝将密钥或本地签名配置作为产物目标：$absolute" }
    $cursor = $absolute
    while (Test-AwjPathUnder $root $cursor) {
        try {
            $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or $item.LinkType) {
                throw "拒绝链接、junction 或 reparse point：$cursor"
            }
        } catch {
            if ($_.CategoryInfo.Category -ne [Management.Automation.ErrorCategory]::ObjectNotFound) { throw }
        }
        if ($cursor.TrimEnd('\', '/').Equals($root, $comparison)) { break }
        $parent = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\', '/'))
        if (-not $parent -or $parent -eq $cursor) { throw "路径祖先无效：$absolute" }
        $cursor = $parent
    }
    return $absolute
}

function Assert-AwjBuildPath {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot, [Parameter(Mandatory)][string]$Path)
    $absolute = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path $Path
    $build = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'build'))
    if (-not (Test-AwjPathUnder $build $absolute) -or $absolute.TrimEnd('\', '/') -eq $build.TrimEnd('\', '/')) {
        throw "构建和临时输出必须位于本项目 build 子目录：$absolute"
    }
    return $absolute
}

function Invoke-AwjGit {
    param([Parameter(Mandatory)][string]$RepositoryRoot, [Parameter(Mandatory)][string[]]$Arguments)
    $info = [Diagnostics.ProcessStartInfo]::new('git')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($arg in (@('-C', $RepositoryRoot) + $Arguments)) { $info.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw '无法启动 Git 源码指纹检查。' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $output = $stdout.GetAwaiter().GetResult()
        $errorText = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Git 源码检查失败：$($errorText.Substring(0, [Math]::Min(2048, $errorText.Length)))" }
        return $output
    } finally { $process.Dispose() }
}

function Test-AwjSourcePath([string]$Path) {
    foreach ($part in $Path.Replace('\', '/').Split('/')) {
        if (Test-AwjSensitiveName $part) { return $false }
    }
    return $Path -cmatch '^(?:src/|include/|sdk/|examples/|ui/|cmake/|vcpkg-overlays/|licenses/|tests/|scripts/|docs/)' -or
        $Path -cmatch '^(?:CMakeLists\.txt|CMakePresets\.json|release\.ps1|VERSION|\.gitattributes|\.gitignore|LICENSE|THIRD_PARTY_NOTICES\.txt|README(?:\.[^.]+)?\.md|CHANGELOG(?:\.[^.]+)?\.md|vcpkg[^/]*\.json)$'
}

function Get-AwjSourceFingerprint {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot)
    $root = [IO.Path]::GetFullPath($RepositoryRoot)
    $listing = Invoke-AwjGit -RepositoryRoot $root -Arguments @('ls-files', '--cached', '--others', '--exclude-standard', '-z')
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    $utf8 = [Text.UTF8Encoding]::new($false)
    $ordered = [Collections.Generic.SortedDictionary[string,string]]::new([StringComparer]::Ordinal)
    foreach ($relative in $listing.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
        if (Test-AwjSourcePath $relative) { $ordered[[Convert]::ToHexString($utf8.GetBytes($relative))] = $relative }
    }
    try {
        foreach ($relative in $ordered.Values) {
            $path = Resolve-AwjRepoPath -RepositoryRoot $root -Path $relative
            if (-not (Test-Path -LiteralPath $path)) { continue }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "源码清单路径不是普通文件：$relative" }
            $blob = (Invoke-AwjGit -RepositoryRoot $root -Arguments @('-c', 'core.autocrlf=input', '-c', 'core.safecrlf=false', 'hash-object', "--path=$relative", '--', $relative)).Trim()
            if ($blob -notmatch '^[0-9a-f]{40,64}$') { throw "源码 blob 摘要无效：$relative" }
            $hash.AppendData($utf8.GetBytes($relative))
            $hash.AppendData([byte[]]@(0))
            $hash.AppendData([Text.Encoding]::ASCII.GetBytes($blob + "`n"))
        }
        return [Convert]::ToHexString($hash.GetHashAndReset()).ToLowerInvariant()
    } finally { $hash.Dispose() }
}

function Test-AwjJsonUnsignedInteger($Value) {
    if ($Value -isnot [int] -and $Value -isnot [long] -and $Value -isnot [UInt64]) { return $false }
    return $Value -ge 0
}

function Assert-AwjJsonFields {
    param([Parameter(Mandatory)]$Object, [string[]]$StringFields=@(), [string[]]$BoolFields=@())
    if ($Object -isnot [Collections.IDictionary]) { throw 'JSON 记录必须是对象。' }
    foreach ($name in $StringFields) {
        if ($Object[$name] -isnot [string]) { throw "JSON 字段必须是字符串：$name" }
    }
    foreach ($name in $BoolFields) {
        if ($Object[$name] -isnot [bool]) { throw "JSON 字段必须是布尔值：$name" }
    }
}

function Initialize-AwjBuildProvenance {
    param([Parameter(Mandatory)][string]$RepositoryRoot, [Parameter(Mandatory)][string]$BuildDirectory,
          [Parameter(Mandatory)][string]$SourceFingerprint, [Parameter(Mandatory)][string[]]$BinaryPaths)
    if ($SourceFingerprint -notmatch '^[0-9a-f]{64}$') { throw '构建来源指纹无效。' }
    $build = Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path $BuildDirectory
    $cache = Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $build 'CMakeCache.txt')
    $marker = Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $build '.awj-build-provenance.json')
    $paths = @($BinaryPaths | ForEach-Object { Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path $_ })
    $cacheHash = (Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash.ToLowerInvariant()
    $reusable = $false
    if (Test-Path -LiteralPath $marker -PathType Leaf) {
        $previous = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json -AsHashtable
        Assert-AwjJsonFields $previous @('kind', 'project_root', 'build_directory', 'source_fingerprint', 'cache_sha256')
        if (-not (Test-AwjJsonUnsignedInteger $previous.schema) -or $previous.schema -ne 1 -or $previous.kind -cne 'build-provenance' -or
            [IO.Path]::GetFullPath($previous.project_root) -ne [IO.Path]::GetFullPath($RepositoryRoot) -or $previous.build_directory -ne $build) {
            throw '已有来源记录归属不明，保留不覆盖，请指定新的构建目录。'
        }
        $reusable = $previous.source_fingerprint -ceq $SourceFingerprint -and $previous.cache_sha256 -ceq $cacheHash -and
            $previous.binaries -is [Array] -and $previous.binaries.Count -eq $paths.Count
        foreach ($path in $paths) {
            $entry = @($previous.binaries | Where-Object { $_.path -ceq $path })
            if ($entry.Count -ne 1 -or -not (Test-Path -LiteralPath $path -PathType Leaf) -or
                (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry[0].sha256) { $reusable = $false }
        }
    }
    if (-not $reusable) {
        $listing = Invoke-AwjGit -RepositoryRoot $RepositoryRoot -Arguments @('ls-files', '--cached', '--others', '--exclude-standard', '-z')
        $now = [DateTime]::UtcNow
        foreach ($relative in $listing.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
            if ($relative -cnotmatch '^(src/|ui/)' -or -not (Test-AwjSourcePath $relative)) { continue }
            $path = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path $relative
            if (Test-Path -LiteralPath $path -PathType Leaf) { [IO.File]::SetLastWriteTimeUtc($path, $now) }
        }
    }
    return [ordered]@{schema=1; kind='build-provenance'; project_root=[IO.Path]::GetFullPath($RepositoryRoot);
        build_directory=$build; source_fingerprint=$SourceFingerprint; cache_path=$cache; cache_sha256=$cacheHash;
        marker_path=$marker; binary_paths=$paths; reused_verified_inputs=$reusable}
}

function Complete-AwjBuildProvenance {
    param([Parameter(Mandatory)]$State)
    if ((Get-AwjSourceFingerprint -RepositoryRoot $State.project_root) -cne $State.source_fingerprint) { throw '编译期间源码变化，拒绝绑定来源。' }
    $binaries = foreach ($path in $State.binary_paths) {
        $safe = Assert-AwjBuildPath -RepositoryRoot $State.project_root -Path $path
        [ordered]@{path=$safe; size=[UInt64](Get-Item -LiteralPath $safe).Length; sha256=(Get-FileHash -LiteralPath $safe -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    [ordered]@{schema=1; kind='build-provenance'; project_root=$State.project_root; build_directory=$State.build_directory;
        source_fingerprint=$State.source_fingerprint; cache_sha256=(Get-FileHash -LiteralPath $State.cache_path -Algorithm SHA256).Hash.ToLowerInvariant(); binaries=@($binaries)} |
        ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $State.marker_path -Encoding utf8
}

function Assert-AwjVersion([string]$Version) {
    if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw '版本必须是规范的 MAJOR.MINOR.PATCH。' }
}

function New-AwjReleaseStage {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot, [Parameter(Mandatory)][string]$Version)
    Assert-AwjVersion $Version
    $stage = Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $RepositoryRoot "build/release-staging/$Version-$([Guid]::NewGuid().ToString('N'))")
    if (Test-Path -LiteralPath $stage) { throw "暂存目录必须全新：$stage" }
    [void][IO.Directory]::CreateDirectory($stage)
    [ordered]@{ schema=1; project_root=[IO.Path]::GetFullPath($RepositoryRoot); version=$Version; kind='release-stage'; created_at=[DateTimeOffset]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage '.awj-owned-stage.json') -Encoding utf8
    return $stage
}

function Test-AwjArtifactAcceptance {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot,
          [Parameter(Mandatory)][string]$Directory,
          [Parameter(Mandatory)][string]$Version)
    Assert-AwjVersion $Version
    $directoryPath = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path $Directory
    $receiptPath = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $directoryPath 'verification/acceptance.json')
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -AsHashtable
    Assert-AwjJsonFields $receipt @('version', 'status', 'source_fingerprint')
    if (-not (Test-AwjJsonUnsignedInteger $receipt.schema) -or $receipt.schema -ne 1 -or $receipt.version -cne $Version -or $receipt.status -cne 'passed' -or
        $receipt.source_fingerprint -notmatch '^[0-9a-f]{64}$' -or $receipt.artifacts -isnot [Array] -or -not $receipt.artifacts) {
        throw '新版本缺少匹配且已通过验收的产物清单。'
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($asset in $receipt.artifacts) {
        Assert-AwjJsonFields $asset @('path', 'sha256')
        if (-not (Test-AwjJsonUnsignedInteger $asset.size)) { throw '验收文件尺寸必须是非负整数。' }
        $relative = [string]$asset.path
        if (-not $relative -or [IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)' -or
            -not $names.Add($relative) -or $asset.sha256 -notmatch '^[0-9a-f]{64}$') { throw '验收清单包含非法或重复路径。' }
        $path = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $directoryPath $relative)
        if (-not (Test-AwjPathUnder $directoryPath $path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "验收文件缺失：$relative" }
        $item = Get-Item -LiteralPath $path
        if ($item.Length -ne [UInt64]$asset.size -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $asset.sha256) { throw "验收文件已变化：$relative" }
    }
    return $receipt
}

function Publish-AwjVersionStage {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot,
          [Parameter(Mandatory)][string]$Version,
          [Parameter(Mandatory)][string]$Stage,
          [switch]$PreserveExisting)
    $stagePath = Assert-AwjBuildPath -RepositoryRoot $RepositoryRoot -Path $Stage
    $marker = Get-Content -LiteralPath (Join-Path $stagePath '.awj-owned-stage.json') -Raw | ConvertFrom-Json -AsHashtable
    Assert-AwjJsonFields $marker @('version', 'kind', 'project_root')
    if (-not (Test-AwjJsonUnsignedInteger $marker.schema) -or $marker.schema -ne 1 -or $marker.version -cne $Version -or $marker.kind -cne 'release-stage' -or
        [IO.Path]::GetFullPath($marker.project_root) -ne [IO.Path]::GetFullPath($RepositoryRoot)) { throw '暂存目录不属于本项目的当前版本。' }
    $null = Test-AwjArtifactAcceptance -RepositoryRoot $RepositoryRoot -Directory $stagePath -Version $Version
    $destination = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $RepositoryRoot "bin/$Version")
    $exists = Test-Path -LiteralPath $destination
    if ($exists -and -not $PreserveExisting) { throw "拒绝覆盖已存在的版本目录：$destination" }
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
    if ([IO.Path]::GetPathRoot($stagePath) -ne [IO.Path]::GetPathRoot($destination)) { throw '投放暂存与目标必须同卷。' }
    if ($exists) {
        if (-not (Test-Path -LiteralPath $destination -PathType Container)) { throw '既有版本位置不是目录。' }
        $entries = @(Get-ChildItem -LiteralPath $stagePath -Force)
        foreach ($entry in $entries) {
            if (Test-Path -LiteralPath (Join-Path $destination $entry.Name)) {
                throw "保留既有版本成员，拒绝覆盖：$($entry.Name)"
            }
        }
        # 已有候选保持原路径；只投放全新正式成员，验收目录最后提交。
        foreach ($entry in @($entries | Where-Object Name -CNE 'verification') + @($entries | Where-Object Name -CEQ 'verification')) {
            $target = Join-Path $destination $entry.Name
            if ($entry.PSIsContainer) { [IO.Directory]::Move($entry.FullName, $target) }
            else { [IO.File]::Move($entry.FullName, $target) }
        }
        Remove-Item -LiteralPath $stagePath -Confirm:$false -ErrorAction Stop
    } else {
        [IO.Directory]::Move($stagePath, $destination)
    }
    $null = Test-AwjArtifactAcceptance -RepositoryRoot $RepositoryRoot -Directory $destination -Version $Version
    return $destination
}

function Get-AwjTreeFiles {
    param([Parameter(Mandatory)][string]$RepositoryRoot, [Parameter(Mandatory)][string]$Directory)
    $root = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path $Directory
    $stack = [Collections.Generic.Stack[string]]::new()
    $stack.Push($root)
    while ($stack.Count) {
        $path = $stack.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $path -Force -ErrorAction Stop) {
            $safe = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path $entry.FullName
            if ($entry.PSIsContainer) { $stack.Push($safe) }
            else { $entry }
        }
    }
}

function New-AwjReleaseCleanupPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot,
          [Parameter(Mandatory)][string]$Version,
          [string[]]$CacheCandidates=@(), [string[]]$ActiveBuildRoots=@())
    $root = [IO.Path]::GetFullPath($RepositoryRoot)
    $current = Resolve-AwjRepoPath -RepositoryRoot $root -Path (Join-Path $root "bin/$Version")
    $null = Test-AwjArtifactAcceptance -RepositoryRoot $root -Directory $current -Version $Version
    $items = [Collections.Generic.List[object]]::new()
    foreach ($directory in Get-ChildItem -LiteralPath (Join-Path $root 'bin') -Directory -Force) {
        if ($directory.Name -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or [version]$directory.Name -ge [version]$Version) { continue }
        $record = [ordered]@{ path=$directory.FullName; kind='old-version'; version=$directory.Name; status='permission-required'; bytes=0L; backup_files=@(); reason='尚未执行删除；需按已核对清单获得实际清理权限。' }
        try {
            $files = @(Get-AwjTreeFiles -RepositoryRoot $root -Directory $directory.FullName)
            if ($files | Where-Object { Test-AwjSensitiveName $_.Name }) { throw '目录包含疑似密钥或本地签名配置，保留且不读取其内容。' }
            $record.bytes = ($files | Measure-Object Length -Sum).Sum
            $managed = $false
            $oldMarker = Join-Path $directory.FullName '.awj-owned-stage.json'
            if (Test-Path -LiteralPath $oldMarker -PathType Leaf) {
                try {
                    $owner = Get-Content -LiteralPath $oldMarker -Raw | ConvertFrom-Json -AsHashtable
                    Assert-AwjJsonFields $owner @('version', 'kind', 'project_root')
                    if ($owner.version -ceq $directory.Name -and $owner.kind -ceq 'release-stage' -and [IO.Path]::GetFullPath($owner.project_root) -eq $root) {
                        $null = Test-AwjArtifactAcceptance -RepositoryRoot $root -Directory $directory.FullName -Version $directory.Name
                        $managed = $true
                    }
                } catch { $managed = $false }
            }
            $backups = foreach ($file in $files) {
                $relative = $file.FullName.Substring($directory.FullName.Length).TrimStart('\', '/').Replace('\', '/')
                if ($managed -and ($relative -ceq '.awj-owned-stage.json' -or $relative.StartsWith('verification/', [StringComparison]::Ordinal))) { continue }
                $originVersion = $directory.Name
                $originPath = $relative
                while ($originPath -cmatch '^user-data-backup/(?<version>(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))/(?<path>.+)$') {
                    $originVersion = $Matches.version
                    $originPath = $Matches.path
                    if ([version]$originVersion -gt [version]$directory.Name) { throw '继承备份来源版本不属于旧版本范围，保留原目录。' }
                }
                $distributed = $file.Name -match '^(AWJ(?:\.exe|\.com)?|AWJ_(?:Win|Linux)\.7z|LICENSE|NOTICE\.txt|THIRD_PARTY_NOTICES\.txt|SHA256SUMS(?:\.txt)?|VERSION)$' -or
                    $file.Name -match '^AWJ.*\.(?:7z|zip|tar\.gz|tar\.xz)$' -or $file.Name -match '^awj_update_manifest_sign(?:\.exe)?$' -or
                    $file.Name -match '^update-(?:archive|keyring)\.json(?:\.sig)?$'
                if (-not $distributed) {
                    [ordered]@{ path=$relative; origin_version=$originVersion; origin_path=$originPath; size=[UInt64]$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
                }
            }
            $record.backup_files = @($backups)
            if ($IsWindows) {
                $active = @(Get-CimInstance Win32_Process | Where-Object {
                    ($_.ExecutablePath -and (Test-AwjPathUnder $directory.FullName $_.ExecutablePath)) -or
                    ($_.CommandLine -and $_.CommandLine.Contains($directory.FullName, [StringComparison]::OrdinalIgnoreCase))
                } | Select-Object -ExpandProperty ProcessId)
                if ($active.Count) { throw "目录仍被进程引用：$($active -join ',')" }
            }
        } catch { $record.status='retained'; $record.reason=$_.Exception.Message }
        $items.Add([pscustomobject]$record)
    }
    foreach ($candidate in $CacheCandidates) {
        $record = [ordered]@{path=$candidate; kind='build-cache'; status='retained'; reason='仅列清单，尚未证明失效与无引用，不执行删除。'}
        try {
            $safe = Assert-AwjBuildPath -RepositoryRoot $root -Path $candidate
            $record.path=$safe
            foreach ($active in $ActiveBuildRoots) {
                if ((Test-AwjPathUnder $safe $active) -or (Test-AwjPathUnder $active $safe)) { throw '当前构建或其依赖范围受保护。' }
            }
        } catch { $record.reason=$_.Exception.Message }
        $items.Add([pscustomobject]$record)
    }
    return [pscustomobject][ordered]@{
        schema=1; project_root=$root; version=$Version; mode='non-destructive-plan'; created_at=[DateTimeOffset]::UtcNow.ToString('o')
        acceptance_sha256=(Get-FileHash -LiteralPath (Join-Path $current 'verification/acceptance.json') -Algorithm SHA256).Hash.ToLowerInvariant()
        candidates=@($items); protected_paths=@((Join-Path $root 'bin/x64'), (Join-Path $root 'release-signing.local.json'))
    }
}

function Backup-AwjReleaseUserData {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepositoryRoot, [Parameter(Mandatory)]$Plan)
    if ($Plan.schema -ne 1 -or $Plan.mode -ne 'non-destructive-plan' -or
        [IO.Path]::GetFullPath($Plan.project_root) -ne [IO.Path]::GetFullPath($RepositoryRoot)) { throw '备份清单归属无效。' }
    $current = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $RepositoryRoot "bin/$($Plan.version)")
    $null = Test-AwjArtifactAcceptance -RepositoryRoot $RepositoryRoot -Directory $current -Version $Plan.version
    if ((Get-FileHash -LiteralPath (Join-Path $current 'verification/acceptance.json') -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Plan.acceptance_sha256) { throw '验收记录在计划后发生变化。' }
    $results = [Collections.Generic.List[object]]::new()
    $completed = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $backupRoot = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $current 'user-data-backup')
    foreach ($candidate in $Plan.candidates) {
        if ($candidate.kind -ne 'old-version' -or $candidate.status -ne 'permission-required') { continue }
        Assert-AwjVersion ([string]$candidate.version)
        $expectedSource = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $RepositoryRoot "bin/$($candidate.version)")
        $sourceRoot = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path $candidate.path
        if ($sourceRoot -ne $expectedSource -or [version]$candidate.version -ge [version]$Plan.version) { throw '备份清单不是当前项目的旧版本目录。' }
        foreach ($file in $candidate.backup_files) {
            $originVersion = [string]$candidate.version
            $originPath = [string]$file.path
            if ($file -is [Collections.IDictionary]) {
                if ($file.Contains('origin_version')) { $originVersion = $file['origin_version'] }
                if ($file.Contains('origin_path')) { $originPath = $file['origin_path'] }
            } else {
                if ($file.PSObject.Properties['origin_version']) { $originVersion = $file.origin_version }
                if ($file.PSObject.Properties['origin_path']) { $originPath = $file.origin_path }
            }
            if ($originVersion -isnot [string] -or $originPath -isnot [string] -or -not $originPath -or
                [IO.Path]::IsPathRooted($originPath) -or $originPath -match '(^|[/\\])\.\.([/\\]|$)' -or
                $file.sha256 -isnot [string] -or $file.sha256 -notmatch '^[0-9a-f]{64}$' -or -not (Test-AwjJsonUnsignedInteger $file.size)) { throw '备份记录字段或来源路径无效。' }
            Assert-AwjVersion $originVersion
            if ([version]$originVersion -gt [version]$candidate.version) { throw '备份来源版本超过原目录版本。' }
            $source = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $sourceRoot $file.path)
            if (-not (Test-AwjPathUnder $sourceRoot $source) -or (Test-AwjSensitiveName $source)) { throw '备份源路径无效。' }
            if ((Get-Item -LiteralPath $source -Force).Length -ne [UInt64]$file.size -or
                (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw "备份源在计划后变化：$source" }
            $destination = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $backupRoot "$originVersion/$originPath")
            if (-not (Test-AwjPathUnder $backupRoot $destination)) { throw '备份目标超出用户数据备份目录。' }
            if ((Test-Path -LiteralPath $destination) -and (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) {
                $destination = Resolve-AwjRepoPath -RepositoryRoot $RepositoryRoot -Path (Join-Path $backupRoot "$originVersion/.history/$($file.sha256)/$originPath")
                if (-not (Test-AwjPathUnder $backupRoot $destination)) { throw '备份历史目标越界。' }
            }
            if (Test-Path -LiteralPath $destination) {
                if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw "备份历史目标已有不同内容：$destination" }
            } else {
                [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
                Copy-Item -LiteralPath $source -Destination $destination -ErrorAction Stop
            }
            if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw "备份校验失败：$destination" }
            $backupPath = $destination.Substring($backupRoot.Length).TrimStart('\', '/').Replace('\', '/')
            if ($completed.Add($backupPath + ':' + $file.sha256)) {
                $results.Add([pscustomobject]@{version=$originVersion; path=$originPath; backup_path=$backupPath; size=[UInt64]$file.size; sha256=$file.sha256; status='backed-up'})
            }
        }
    }
    return @($results)
}

Export-ModuleMember -Function Resolve-AwjRepoPath, Assert-AwjBuildPath, Test-AwjJsonUnsignedInteger, Assert-AwjJsonFields, Initialize-AwjBuildProvenance, Complete-AwjBuildProvenance, Get-AwjSourceFingerprint, New-AwjReleaseStage, Test-AwjArtifactAcceptance, Publish-AwjVersionStage, New-AwjReleaseCleanupPlan, Backup-AwjReleaseUserData
