#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$InputRoot,
    [string]$BaselineExecutable,
    [string]$FfmpegExecutable,
    [ValidateSet('matched-sdr','defaults')] [string]$FfmpegProfile = 'matched-sdr',
    [Parameter(Mandatory)] [string]$AwjExecutable,
    [Parameter(Mandatory)] [string]$ResultRoot,
    [string]$BaselineLabel = '1.2.0',
    [string]$CandidateLabel = '1.2.1',
    [switch]$BaselineDisableGpu,
    [switch]$BaselineOnly,
    [ValidateSet('default','cpu-search','gpu-search')] [string]$Scenario = 'default',
    [ValidateRange(0,300)] [int]$CooldownSeconds = 30,
    [string[]]$ExtraArguments = @(),
    [ValidateRange(1,64)] [int]$FileConcurrency = 12,
    [switch]$Smoke
)
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'Process-tree measurements require Windows Job Objects.' }
function Get-Median([double[]]$Values) {
    if (-not $Values.Count) { throw 'No measurements.' }
    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if ($ordered.Count % 2) { return $ordered[$middle] }
    return ($ordered[$middle-1] + $ordered[$middle]) / 2
}

function Add-BenchmarkJobType {
    if ('AwjBenchmarkJob' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public sealed class AwjBenchmarkChild : IDisposable
{
    const uint WAIT_OBJECT_0 = 0;
    const uint WAIT_TIMEOUT = 258;
    const uint INFINITE = 0xffffffff;
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    IntPtr process;
    IntPtr thread;
    readonly int id;
    internal AwjBenchmarkChild(IntPtr p, IntPtr t, uint processId) { process = p; thread = t; id = checked((int)processId); }
    public bool HasExited {
        get {
            if (process == IntPtr.Zero) return true;
            uint value = WaitForSingleObject(process, 0);
            if (value == WAIT_OBJECT_0) return true;
            if (value == WAIT_TIMEOUT) return false;
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
    }
    public int ProcessId { get { return id; } }
    public int ExitCode {
        get {
            uint code;
            if (process == IntPtr.Zero || !GetExitCodeProcess(process, out code)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return unchecked((int)code);
        }
    }
    public void Wait() {
        if (process == IntPtr.Zero) return;
        if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public void Dispose() {
        if (thread != IntPtr.Zero) CloseHandle(thread);
        if (process != IntPtr.Zero) CloseHandle(process);
        thread = IntPtr.Zero;
        process = IntPtr.Zero;
    }
}

public sealed class AwjBenchmarkJob : IDisposable
{
    const uint CREATE_SUSPENDED = 0x00000004;
    const uint CREATE_NO_WINDOW = 0x08000000;
    const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    const int JobObjectBasicAccountingInformation = 1;
    const int JobObjectExtendedLimitInformation = 9;
    [StructLayout(LayoutKind.Sequential)] struct IO_COUNTERS { public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount; }
    [StructLayout(LayoutKind.Sequential)] struct BASIC_LIMIT { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass; }
    [StructLayout(LayoutKind.Sequential)] struct EXTENDED_LIMIT { public BASIC_LIMIT BasicLimitInformation; public IO_COUNTERS IoInfo; public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }
    [StructLayout(LayoutKind.Sequential)] struct ACCOUNTING { public long TotalUserTime, TotalKernelTime, ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime; public uint TotalPageFaultCount, TotalProcesses, ActiveProcesses, TotalTerminatedProcesses; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct STARTUPINFO { public uint cb; public string lpReserved, lpDesktop, lpTitle; public uint dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public ushort wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public uint dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateJobObject(IntPtr a, string n);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr h, int c, ref EXTENDED_LIMIT i, uint s);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr h, int c, out EXTENDED_LIMIT i, uint s, IntPtr r);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool QueryInformationJobObject(IntPtr h, int c, out ACCOUNTING i, uint s, IntPtr r);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr h, IntPtr p);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr h, uint c);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr h, uint c);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcess(string app, StringBuilder command, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFO startup, out PROCESS_INFORMATION created);
    IntPtr job;
    public AwjBenchmarkJob() {
        job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        var info = new EXTENDED_LIMIT();
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info, checked((uint)Marshal.SizeOf(info)))) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    [StructLayout(LayoutKind.Sequential)] struct SECURITY_ATTRIBUTES { public int length; public IntPtr descriptor; public int inherit; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateFile(string path, uint access, uint sharing, ref SECURITY_ATTRIBUTES security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int handle);
    public AwjBenchmarkChild Start(string executable, string[] arguments, string cwd, string logPath) {
        var startup = new STARTUPINFO();
        startup.cb = checked((uint)Marshal.SizeOf(startup));
        PROCESS_INFORMATION created;
        var command = new StringBuilder(BuildCommandLine(executable, arguments));
        var security = new SECURITY_ATTRIBUTES { length = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES)), inherit = 1 };
        var log = CreateFile(logPath, 0x40000000, 1, ref security, 2, 0x80, IntPtr.Zero);
        if (log == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        startup.dwFlags = 0x100;
        startup.hStdInput = GetStdHandle(-10);
        startup.hStdOutput = startup.hStdError = log;
        try {
            if (!CreateProcess(executable, command, IntPtr.Zero, IntPtr.Zero, true, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, IntPtr.Zero, cwd, ref startup, out created)) throw new Win32Exception(Marshal.GetLastWin32Error());
        } finally { CloseHandle(log); }
        try {
            if (!AssignProcessToJobObject(job, created.hProcess)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (ResumeThread(created.hThread) == UInt32.MaxValue) throw new Win32Exception(Marshal.GetLastWin32Error());
            return new AwjBenchmarkChild(created.hProcess, created.hThread, created.dwProcessId);
        } catch {
            TerminateProcess(created.hProcess, 1);
            CloseHandle(created.hThread);
            CloseHandle(created.hProcess);
            throw;
        }
    }
    public double CpuSeconds {
        get {
            ACCOUNTING info;
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, out info, checked((uint)Marshal.SizeOf(typeof(ACCOUNTING))), IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return (info.TotalUserTime + info.TotalKernelTime) / 10000000.0;
        }
    }
    public long PeakMemoryBytes {
        get {
            EXTENDED_LIMIT info;
            if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, out info, checked((uint)Marshal.SizeOf(typeof(EXTENDED_LIMIT))), IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
            ulong bytes = info.PeakJobMemoryUsed.ToUInt64();
            return bytes > Int64.MaxValue ? Int64.MaxValue : checked((long)bytes);
        }
    }
    static string BuildCommandLine(string executable, string[] arguments) {
        var result = new StringBuilder(Quote(executable));
        foreach (string argument in arguments) result.Append(' ').Append(Quote(argument));
        return result.ToString();
    }
    static string Quote(string value) {
        if (value.Length > 0 && value.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0) return value;
        var result = new StringBuilder("\"");
        int slashCount = 0;
        foreach (char c in value) {
            if (c == '\\') { ++slashCount; continue; }
            if (c == '"') { result.Append('\\', slashCount * 2 + 1).Append('"'); slashCount = 0; continue; }
            result.Append('\\', slashCount).Append(c);
            slashCount = 0;
        }
        result.Append('\\', slashCount * 2).Append('"');
        return result.ToString();
    }
    public void Dispose() {
        if (job != IntPtr.Zero) CloseHandle(job);
        job = IntPtr.Zero;
    }
}
'@
}

function Invoke-FfmpegBatch($Executable, $Inputs, $Output, $Job) {
    $pending = [Collections.Generic.List[object]]::new()
    $summary = [Collections.Generic.List[object]]::new()
    $next = 0
    try {
        while ($next -lt $Inputs.Count -or $pending.Count) {
            while ($next -lt $Inputs.Count -and $pending.Count -lt $FileConcurrency) {
                $input = $Inputs[$next++]
                $target = Join-Path $Output ($input.BaseName + '.avif')
                $arguments = @('-nostdin','-hide_banner','-loglevel','error','-y',
                    '-noautorotate','-threads','1','-i',$input.FullName,'-frames:v','1','-an','-sn','-dn',
                    '-filter_threads','1',
                    '-c:v','libaom-av1','-usage','allintra','-still-picture','1',
                    '-cpu-used','5','-crf','23','-b:v','0','-lag-in-frames','0',
                    '-threads','1','-row-mt','1','-aom-params','tune=iq',
                    '-f','avif',$target)
                if ($FfmpegProfile -eq 'matched-sdr') {
                    $arguments = $arguments[0..($arguments.Count-4)] + @(
                        '-map_metadata','-1',
                        '-vf','scale=out_range=pc:out_color_matrix=bt709,setparams=range=full:color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709',
                        '-pix_fmt','yuv444p10le','-color_range','pc','-color_primaries','bt709',
                        '-color_trc','iec61966-2-1','-colorspace','bt709','-f','avif',$target)
                }
                $child = $Job.Start($Executable,$arguments,[IO.Path]::GetDirectoryName($Executable),
                    (Join-Path $Output ($input.BaseName + '.log')))
                $pending.Add([pscustomobject]@{ child=$child; input=$input; target=$target })
            }
            for ($index=$pending.Count-1; $index -ge 0; --$index) {
                $item = $pending[$index]
                if (-not $item.child.HasExited) { continue }
                $exitCode = $item.child.ExitCode
                $ok = $exitCode -eq 0 -and (Test-Path -LiteralPath $item.target)
                $summary.Add([pscustomobject]@{
                    input=$item.input.Name; output=[IO.Path]::GetFileName($item.target)
                    status=$(if($ok){'ok'}else{'failed'}); exit_code=$exitCode
                    output_bytes=$(if($ok){(Get-Item -LiteralPath $item.target).Length}else{0})
                    output_sha256=$(if($ok){(Get-FileHash -LiteralPath $item.target -Algorithm SHA256).Hash.ToLowerInvariant()}else{''})
                })
                $item.child.Dispose()
                $pending.RemoveAt($index)
            }
            if ($pending.Count) { Start-Sleep -Milliseconds 20 }
        }
    } finally { foreach ($item in $pending) { $item.child.Dispose() } }
    $summary | Sort-Object input | Export-Csv -LiteralPath (Join-Path $Output 'summary.csv') -NoTypeInformation -Encoding utf8NoBOM
    return $(if(@($summary | Where-Object status -NE 'ok').Count){2}else{0})
}

$InputRoot = (Resolve-Path -LiteralPath $InputRoot).Path
$ffmpegMode = -not [string]::IsNullOrWhiteSpace($FfmpegExecutable)
if ($ffmpegMode) {
    if ($BaselineExecutable -or $Scenario -ne 'default') { throw 'FFmpeg comparison requires default scenario and no AWJ baseline.' }
    $BaselineExecutable = $FfmpegExecutable
    $BaselineLabel = 'ffmpeg'
    $ExtraArguments += @('--threads',"$FileConcurrency",'--memory-limit','8GiB')
    if ($FfmpegProfile -eq 'matched-sdr') { $ExtraArguments += @(
        '--chroma','444','--bit-depth','10','--alpha','off','--strip',
        '--color-primaries','1','--transfer-characteristics','13','--matrix-coefficients','1','--color-range','1') }
}
if (-not $BaselineExecutable) { throw 'Provide FfmpegExecutable or BaselineExecutable.' }
$BaselineExecutable = (Resolve-Path -LiteralPath $BaselineExecutable).Path
$AwjExecutable = (Resolve-Path -LiteralPath $AwjExecutable).Path
$ResultRoot = [IO.Path]::GetFullPath($ResultRoot)
$workRoot = [IO.Path]::GetDirectoryName($InputRoot.TrimEnd('\')) + '\'
if (-not $ResultRoot.StartsWith($workRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $ResultRoot.StartsWith($InputRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Results must remain inside the corpus workspace, outside inputs.'
}
New-Item -ItemType Directory -Path $ResultRoot -Force | Out-Null
$inputs = @(Get-ChildItem -LiteralPath $InputRoot -File)
if (-not $inputs.Count) { throw 'Empty corpus.' }
$corpusIdentity = ($inputs | Sort-Object Name | ForEach-Object {
    $_.Name + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
}) -join "`n"
$corpusHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($corpusIdentity))).ToLowerInvariant()
$labels = @($BaselineLabel,$CandidateLabel)
if ($labels[0] -eq $labels[1] -or @($labels | Where-Object { $_ -notmatch '^[A-Za-z0-9._-]+$' }).Count) { throw 'Distinct path-safe labels required.' }
$executables = @($BaselineExecutable,$AwjExecutable)
$hashes = @($executables | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() })
$metadata = [ordered]@{
    baseline_label=$labels[0]; candidate_label=$labels[1]; baseline_disable_gpu=[bool]$BaselineDisableGpu
    scenario=$Scenario; inputs=$inputs.Count; corpus_sha256=$corpusHash; baseline=$hashes[0]; candidate=$hashes[1]
    extra_arguments=@($ExtraArguments)
    ffmpeg_comparison=$ffmpegMode; file_concurrency=$FileConcurrency
    ffmpeg_profile=$FfmpegProfile
    baseline_only=[bool]$BaselineOnly
    script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    cpu=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
    logical_processors=[Environment]::ProcessorCount
    memory_bytes=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    os=[Environment]::OSVersion.VersionString; started_utc=[DateTimeOffset]::UtcNow.ToString('o')
    warmups=$(if($Smoke){0}else{1}); measurements=$(if($Smoke){1}else{5}); order='alternating'; peak_memory='whole-job committed bytes'
}
$metaPath = Join-Path $ResultRoot 'conditions.json'
if (Test-Path -LiteralPath $metaPath) {
    $previous = Get-Content -LiteralPath $metaPath -Raw | ConvertFrom-Json
    if ($previous.baseline -ne $hashes[0] -or $previous.candidate -ne $hashes[1] -or
        $previous.inputs -ne $inputs.Count -or $previous.scenario -ne $Scenario -or
        $previous.corpus_sha256 -ne $corpusHash -or $previous.baseline_label -ne $labels[0] -or
        $previous.candidate_label -ne $labels[1] -or $previous.baseline_disable_gpu -ne [bool]$BaselineDisableGpu -or
        [string]::Join([char]31, [string[]]@($previous.extra_arguments)) -ne [string]::Join([char]31, $ExtraArguments) -or
        ($previous.script_sha256 -and $previous.script_sha256 -ne $metadata.script_sha256) -or
        [bool]$previous.ffmpeg_comparison -ne $ffmpegMode -or
        ($previous.ffmpeg_profile -and $previous.ffmpeg_profile -ne $FfmpegProfile) -or
        [bool]$previous.baseline_only -ne [bool]$BaselineOnly -or
        $previous.measurements -ne $metadata.measurements) {
        throw 'Existing results belong to different binaries or inputs.'
    }
} else { $metadata | ConvertTo-Json | Set-Content -LiteralPath $metaPath -Encoding utf8NoBOM }
Add-BenchmarkJobType
$runsPath = Join-Path $ResultRoot 'runs.csv'
$runs = [Collections.Generic.List[object]]::new()
if (Test-Path -LiteralPath $runsPath) {
    foreach($row in Import-Csv -LiteralPath $runsPath) {
        if ([int]$row.failed -ne 0 -or [int]$row.exit_code -ne 0) { throw 'Existing results contain conversion errors; use a new result directory.' }
        $runs.Add($row)
    }
}
$lastIteration = if ($Smoke) { 0 } else { 5 }
for ($iteration=0; $iteration -le $lastIteration; ++$iteration) {
    $order = if ($BaselineOnly) { @(0) } elseif ($iteration % 2) { @(1,0) } else { @(0,1) }
    foreach ($versionIndex in $order) {
        $version = $labels[$versionIndex]
        if (@($runs | Where-Object { [int]$_.iteration -eq $iteration -and $_.version -eq $version }).Count) { continue }
        $output = Join-Path $ResultRoot "$iteration-$version"
        if (Test-Path -LiteralPath $output) { throw "Unrecorded output exists: $output; inspect it before restarting." }
        New-Item -ItemType Directory -Path $output | Out-Null
        $arguments = @('--input',$InputRoot,'--output',$output,'--summary','--no-log','--format','avif','--quality','70','--speed','5')
        if ($Scenario -ne 'default') { $arguments += @('--visual-quality','50') }
        $arguments += $ExtraArguments
        if ($Scenario -eq 'cpu-search' -or ($versionIndex -eq 0 -and $BaselineDisableGpu)) { $arguments += '--no-visual-quality-gpu' }
        Write-Host "$(Get-Date -Format HH:mm:ss) $Scenario iteration $iteration, $version, $($inputs.Count) images"
        $job = [AwjBenchmarkJob]::new()
        $child = $null
        $watch = [Diagnostics.Stopwatch]::StartNew()
        try {
            if ($ffmpegMode -and $versionIndex -eq 0) {
                $exitCode = Invoke-FfmpegBatch $executables[0] $inputs $output $job
            } else {
                $child = $job.Start($executables[$versionIndex],$arguments,[IO.Path]::GetDirectoryName($executables[$versionIndex]),(Join-Path $output 'stdout.log'))
                while (-not $child.HasExited) { Start-Sleep -Milliseconds 200 }
                $child.Wait()
                $exitCode = $child.ExitCode
            }
            $watch.Stop()
            $cpuSeconds = $job.CpuSeconds
            $peakMemory = $job.PeakMemoryBytes
        } finally {
            if ($child) { $child.Dispose() }
            $job.Dispose()
        }
        if ($exitCode -notin @(0,2)) { throw "Conversion exited with $exitCode." }
        $summary = @(Import-Csv -LiteralPath (Join-Path $output 'summary.csv'))
        if ($summary.Count -ne $inputs.Count) { throw "Expected $($inputs.Count) summary rows, got $($summary.Count)." }
        $ok = @($summary | Where-Object status -EQ 'ok')
        $row = [pscustomobject]@{
            scenario=$Scenario; iteration=$iteration; warmup=($iteration -eq 0 -and -not $Smoke)
            version=$version; wall_seconds=$watch.Elapsed.TotalSeconds; tree_cpu_seconds=$cpuSeconds
            peak_committed_bytes=$peakMemory; success=$ok.Count; failed=($summary.Count-$ok.Count)
            output_bytes=($summary | Measure-Object -Property output_bytes -Sum).Sum
            exit_code=$exitCode
        }
        $runs.Add($row)
        $runs | Export-Csv -LiteralPath $runsPath -NoTypeInformation -Encoding utf8NoBOM
        Write-Host ("Done: {0:F2}s, CPU {1:F2}s, peak {2:F1} MiB, {3} succeeded" -f $row.wall_seconds,$row.tree_cpu_seconds,($peakMemory/1MB),$ok.Count)
        if ($row.failed -or $exitCode -ne 0) { throw 'Conversion errors recorded; this run cannot be used for a complete-corpus benchmark.' }
        if ($CooldownSeconds) { Start-Sleep -Seconds $CooldownSeconds }
    }
}
$aggregateLabels = if ($BaselineOnly) { @($labels[0]) } else { $labels }
$aggregates = foreach($version in $aggregateLabels) {
    $rows = @($runs | Where-Object { $_.version -eq $version -and ($Smoke -or [int]$_.iteration -gt 0) })
    [pscustomobject]@{
        version=$version; runs=$rows.Count; success=$rows[0].success; failed=$rows[0].failed
        wall_seconds=Get-Median @($rows | ForEach-Object { [double]$_.wall_seconds })
        tree_cpu_seconds=Get-Median @($rows | ForEach-Object { [double]$_.tree_cpu_seconds })
        peak_committed_mib=(Get-Median @($rows | ForEach-Object { [double]$_.peak_committed_bytes }))/1MB
        output_mib=(Get-Median @($rows | ForEach-Object { [double]$_.output_bytes }))/1MB
    }
}
$aggregates | Export-Csv -LiteralPath (Join-Path $ResultRoot 'medians.csv') -NoTypeInformation -Encoding utf8NoBOM
$aggregates | Format-Table -AutoSize
