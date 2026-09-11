param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Inputs,
    [Parameter(Mandatory)][string]$Outputs,
    [ValidateSet('avif', 'png')][string]$Format = 'avif',
    [ValidateRange(1, 100)][int]$Quality = 50,
    [string[]]$Groups = @('large', 'mixed', 'grid', 'stress')
)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$inputRoot = (Resolve-Path -LiteralPath $Inputs).Path
$outputRoot = [IO.Path]::GetFullPath($Outputs)
if (Test-Path -LiteralPath $outputRoot) { throw 'Output directory must be fresh' }
New-Item -ItemType Directory -Path $outputRoot | Out-Null
$manifest = Get-Content -LiteralPath (Join-Path $inputRoot 'samples.json') -Raw | ConvertFrom-Json -AsHashtable
$records = @()
Get-FileHash -LiteralPath $exe | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'executable.json')
foreach ($group in $Groups) {
    $budgets = if ($Format -eq 'png') { @(256, 1024, 2048) }
        elseif ($group -eq 'grid') { @(128, 1024, 2048, 4096) } else { @(512, 3072, 6144, 12288) }
    foreach ($memoryMiB in $budgets) {
        $folder = Join-Path $outputRoot "$group-$memoryMiB"
        New-Item -ItemType Directory -Path $folder | Out-Null
        $info = [Diagnostics.ProcessStartInfo]::new($exe)
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.WindowStyle = 'Hidden'
        $info.WorkingDirectory = $folder
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        foreach ($arg in @('-i', (Join-Path $inputRoot $group), '-o', $folder, '--format', $Format,
            '--quality', "$Quality", '--threads', '8', '--memory-limit', "${memoryMiB}MiB",
            '--summary', '--log')) { $info.ArgumentList.Add($arg) }
        if ($Format -eq 'avif') {
            foreach ($arg in @('--speed', '8', '--chroma', '444')) { $info.ArgumentList.Add($arg) }
        }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $info
        $watch = [Diagnostics.Stopwatch]::StartNew()
        if (-not $process.Start()) { throw 'Could not start benchmark' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $peakWorking = 0L
        $peakCommit = 0L
        $cpuSeconds = 0.0
        do {
            $process.Refresh()
            if ($process.HasExited) { break }
            $peakWorking = [Math]::Max($peakWorking, $process.PeakWorkingSet64)
            $peakCommit = [Math]::Max($peakCommit, $process.PeakPagedMemorySize64)
            $cpuSeconds = $process.TotalProcessorTime.TotalSeconds
        } while (-not $process.WaitForExit(20))
        $process.WaitForExit()
        $watch.Stop()
        try { $cpuSeconds = $process.TotalProcessorTime.TotalSeconds } catch { }
        $stdout.Result | Set-Content -LiteralPath (Join-Path $folder 'stdout.log')
        $stderr.Result | Set-Content -LiteralPath (Join-Path $folder 'stderr.log')
        $summary = @(Import-Csv -LiteralPath (Join-Path $folder 'summary.csv'))
        $failed = @($summary | Where-Object status -eq 'failed')
        $succeeded = @($summary | Where-Object status -eq 'ok')
        $expectedRejection = $memoryMiB -le 512 -and $failed.Count -gt 0 -and
            @($failed | Where-Object message -NotMatch '超过内存(预算|限制)').Count -eq 0
        $files = @($manifest.Keys | Where-Object { $_ -match "^$group[/\\]" })
        $pixels = 0L
        foreach ($file in $files) {
            if ([IO.Path]::GetFileName($file) -in $succeeded.input) {
                $pixels += [long]$manifest[$file].dimensions[0] * $manifest[$file].dimensions[1]
            }
        }
        $records += [pscustomobject]@{
            Group=$group; Format=$Format; Quality=$Quality; MemoryMiB=$memoryMiB; Files=$files.Count; Pixels=$pixels
            Succeeded=$succeeded.Count; Failed=$failed.Count; ExpectedRejection=$expectedRejection
            EncoderThreads=($succeeded.encoder_threads | Sort-Object -Unique) -join '/'
            Seconds=$watch.Elapsed.TotalSeconds; CpuSeconds=$cpuSeconds
            AverageBusyCores=$cpuSeconds / $watch.Elapsed.TotalSeconds
            MegapixelsPerSecond=$pixels / 1e6 / $watch.Elapsed.TotalSeconds
            PeakWorkingSetMiB=$peakWorking / 1MB; PeakCommitMiB=$peakCommit / 1MB
            ExitCode=$process.ExitCode
        }
        $records | Export-Csv -LiteralPath (Join-Path $outputRoot 'metrics.csv') -NoTypeInformation
        $records[-1] | ConvertTo-Json -Compress | Write-Output
        if ($summary.Count -ne $files.Count -or
            ($process.ExitCode -ne 0 -and -not $expectedRejection) -or
            ($failed.Count -gt 0 -and -not $expectedRejection) -or
            $peakCommit -gt $memoryMiB * 1MB) { throw "Benchmark failed or exceeded budget: $folder" }
        $process.Dispose()
    }
}
