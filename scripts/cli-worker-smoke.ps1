#requires -Version 7.4

[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\bin\x64\Release\AWJ.exe'),
    [string]$InputDirectory = 'D:\图片\benchmark\test',
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 90,
    [ValidateRange(100, 10000)]
    [int]$ActionDelayMilliseconds = 750
)

$ErrorActionPreference = 'Stop'

if (-not $IsWindows) {
    throw 'The CLI worker smoke requires Windows.'
}

$exePath = [IO.Path]::GetFullPath($Executable)
$inputPath = [IO.Path]::GetFullPath($InputDirectory)
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\cli-worker-smoke'))
if (-not $workRoot.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a work directory outside the repository: $workRoot"
}
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "CLI executable not found: $exePath"
}
if (-not (Test-Path -LiteralPath $inputPath -PathType Container)) {
    throw "Benchmark input directory not found: $inputPath"
}

$supportedExtensions = @(
    '.jpg', '.jpeg', '.jpe', '.jfif', '.png', '.webp', '.bmp', '.dib',
    '.rle', '.tif', '.tiff', '.gif', '.jxl', '.avif'
)
$inputs = @(Get-ChildItem -LiteralPath $inputPath -File -Recurse |
    Where-Object { $_.Extension.ToLowerInvariant() -in $supportedExtensions } |
    Sort-Object FullName)
if ($inputs.Count -ne 613) {
    throw "The fixed worker smoke requires exactly 613 supported images; found $($inputs.Count)."
}
$validInput = $inputs | Where-Object Length -gt 0 | Sort-Object Length, FullName |
    Select-Object -First 1
$failedInput = $inputs | Where-Object Length -eq 0 | Select-Object -First 1
if ($null -eq $validInput -or $null -eq $failedInput) {
    throw 'The fixed input must contain both a valid image and the known empty failure case.'
}

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public sealed class AwjCliSmokeJob : IDisposable
{
    const uint CREATE_SUSPENDED = 0x00000004;
    const uint CREATE_NO_WINDOW = 0x08000000;
    const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    const int JobObjectExtendedLimitInformation = 9;
    const uint WAIT_OBJECT_0 = 0;
    const uint WAIT_TIMEOUT = 258;
    const uint INFINITE = 0xffffffff;

    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO
    {
        public uint cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public uint dwX;
        public uint dwY;
        public uint dwXSize;
        public uint dwYSize;
        public uint dwXCountChars;
        public uint dwYCountChars;
        public uint dwFillAttribute;
        public uint dwFlags;
        public ushort wShowWindow;
        public ushort cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public uint dwProcessId;
        public uint dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass,
        ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, uint length);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateJobObject(IntPtr job, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateProcess(IntPtr process, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint ResumeThread(IntPtr thread);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string applicationName,
        StringBuilder commandLine, IntPtr processAttributes,
        IntPtr threadAttributes, bool inheritHandles, uint creationFlags,
        IntPtr environment, string currentDirectory, ref STARTUPINFO startup,
        out PROCESS_INFORMATION processInformation);

    IntPtr job;
    IntPtr process;
    IntPtr thread;
    uint processId;

    public int ProcessId { get { return checked((int)processId); } }

    public uint ExitCode
    {
        get
        {
            uint code;
            if (process == IntPtr.Zero || !GetExitCodeProcess(process, out code))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return code;
        }
    }

    public bool Wait(int milliseconds)
    {
        if (process == IntPtr.Zero) return true;
        uint timeout = milliseconds < 0 ? INFINITE : checked((uint)milliseconds);
        uint result = WaitForSingleObject(process, timeout);
        if (result == WAIT_OBJECT_0) return true;
        if (result == WAIT_TIMEOUT) return false;
        throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    public void Terminate(uint exitCode)
    {
        if (job != IntPtr.Zero && !TerminateJobObject(job, exitCode))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    public static AwjCliSmokeJob Start(string executable, string[] arguments,
        string workingDirectory)
    {
        var result = new AwjCliSmokeJob();
        try
        {
            result.job = CreateJobObject(IntPtr.Zero, null);
            if (result.job == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error());

            var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            uint limitsSize = checked((uint)Marshal.SizeOf(limits));
            if (!SetInformationJobObject(result.job,
                    JobObjectExtendedLimitInformation, ref limits, limitsSize))
                throw new Win32Exception(Marshal.GetLastWin32Error());

            var startup = new STARTUPINFO();
            startup.cb = checked((uint)Marshal.SizeOf(startup));
            PROCESS_INFORMATION created;
            var commandLine = new StringBuilder(BuildCommandLine(executable,
                arguments));
            if (!CreateProcess(executable, commandLine, IntPtr.Zero,
                    IntPtr.Zero, false,
                    CREATE_SUSPENDED | CREATE_NO_WINDOW |
                        CREATE_UNICODE_ENVIRONMENT,
                    IntPtr.Zero, workingDirectory, ref startup, out created))
                throw new Win32Exception(Marshal.GetLastWin32Error());

            result.process = created.hProcess;
            result.thread = created.hThread;
            result.processId = created.dwProcessId;
            if (!AssignProcessToJobObject(result.job, result.process))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            if (ResumeThread(result.thread) == uint.MaxValue)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return result;
        }
        catch
        {
            result.Dispose();
            throw;
        }
    }

    static string BuildCommandLine(string executable, string[] arguments)
    {
        var command = new StringBuilder(Quote(executable));
        foreach (string argument in arguments)
            command.Append(' ').Append(Quote(argument));
        return command.ToString();
    }

    static string Quote(string value)
    {
        if (value.Length > 0 && value.IndexOfAny(new[] { ' ', '\t', '\n',
                '\v', '"' }) < 0)
            return value;
        var quoted = new StringBuilder("\"");
        int slashes = 0;
        foreach (char character in value)
        {
            if (character == '\\')
            {
                ++slashes;
                continue;
            }
            if (character == '"')
            {
                quoted.Append('\\', slashes * 2 + 1).Append('"');
                slashes = 0;
                continue;
            }
            quoted.Append('\\', slashes).Append(character);
            slashes = 0;
        }
        quoted.Append('\\', slashes * 2).Append('"');
        return quoted.ToString();
    }

    public void Dispose()
    {
        if (process != IntPtr.Zero &&
            WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
        {
            if (job != IntPtr.Zero)
                TerminateJobObject(job, 130);
            else
                TerminateProcess(process, 130);
            WaitForSingleObject(process, 5000);
        }
        if (thread != IntPtr.Zero) CloseHandle(thread);
        if (process != IntPtr.Zero) CloseHandle(process);
        if (job != IntPtr.Zero) CloseHandle(job);
        thread = process = job = IntPtr.Zero;
        processId = 0;
    }
}
'@

function Write-ManifestText([IO.BinaryWriter]$Writer, [string]$Text) {
    $bytes = [Text.UTF8Encoding]::new($false, $true).GetBytes($Text)
    $Writer.Write([uint32]$bytes.Length)
    $Writer.Write($bytes)
}

function Write-StudioManifest(
    [string]$Path,
    [IO.FileInfo[]]$Files,
    [string]$OutputDirectory
) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create,
        [IO.FileAccess]::Write, [IO.FileShare]::None)
    $writer = [IO.BinaryWriter]::new($stream,
        [Text.UTF8Encoding]::new($false), $false)
    try {
        $writer.Write([byte[]](65, 87, 74, 83, 81, 77, 70, 0))
        $writer.Write([uint32]1)
        $writer.Write([uint64]$Files.Count)
        for ($index = 0; $index -lt $Files.Count; ++$index) {
            $writer.Write([uint64]$index)
            $writer.Write([uint64]$Files[$index].Length)
            $writer.Write([uint32]2)
            $outputPath = Join-Path $OutputDirectory ('{0:D6}.avif' -f $index)
            $fields = @(
                $Files[$index].FullName, '', '', '', '', '', '', '', '', '',
                $outputPath
            )
            foreach ($field in $fields) {
                Write-ManifestText $writer $field
            }
        }
    }
    finally {
        $writer.Dispose()
    }
}

function New-WorkerArguments(
    [string]$OutputDirectory,
    [string]$ManifestPath,
    [string]$CancelEventName = ''
) {
    [string[]]$arguments = @(
        '--input', $inputPath,
        '--output', $OutputDirectory,
        '--format', 'avif',
        '--avif-encoder', 'aom',
        '--quality', '80',
        '--speed', '6',
        '--chroma', '420',
        '--bit-depth', '8',
        '--alpha', 'auto',
        '--threads', 'auto',
        '--memory-limit', 'auto',
        '--image-size-limit', 'none',
        '--template', '{name}',
        '--timeout-encode', '120',
        '--collision', 'overwrite',
        '--keep-metadata',
        '--no-wic-fallback',
        '--no-experimental-clamped-grid-padding',
        '--no-unlock-max-input-file-bytes',
        '--no-summary',
        '--no-log',
        '--studio-queue-manifest', $ManifestPath
    )
    if ($CancelEventName) {
        $arguments += @('--studio-cancel-event', $CancelEventName)
    }
    return $arguments
}

function Start-AwjCapture([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $exePath
    $start.WorkingDirectory = [IO.Path]::GetDirectoryName($exePath)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in $Arguments) {
        [void]$start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) {
        throw 'Failed to start AWJ CLI worker.'
    }
    return [pscustomobject]@{
        Process = $process
        Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync()
    }
}

function Complete-AwjCapture($Capture, [int]$Seconds = $TimeoutSeconds) {
    if (-not $Capture.Process.WaitForExit($Seconds * 1000)) {
        $Capture.Process.Kill($true)
        $Capture.Process.WaitForExit()
        throw "AWJ CLI worker timed out after $Seconds seconds."
    }
    $Capture.Process.WaitForExit()
    return [pscustomobject]@{
        ProcessId = $Capture.Process.Id
        ExitCode = $Capture.Process.ExitCode
        Stdout = $Capture.Stdout.GetAwaiter().GetResult()
        Stderr = $Capture.Stderr.GetAwaiter().GetResult()
    }
}

function Assert-StudioProtocol(
    [string]$Stdout,
    [ValidateSet('D', 'F')]
    [string]$FinalStatus
) {
    $lines = @($Stdout -split '\r?\n')
    if ($lines -notcontains "@AWJ-STUDIO/1 ITEM 0 R 0 1" -or
        $lines -notcontains "@AWJ-STUDIO/1 ITEM 0 $FinalStatus 1 1") {
        throw "Studio CLI ITEM protocol did not report R -> $FinalStatus."
    }
    $details = @($lines | Where-Object {
        $_ -match '^@AWJ-STUDIO/1 DETAIL 0 ([^ ]+) ([0-9]+) (-?[0-9]+) (-?[0-9]+) (-?[0-9]+) (-?[0-9]+)$'
    })
    if ($details.Count -ne 1) {
        throw "Studio CLI DETAIL protocol produced $($details.Count) records; expected 1."
    }
    if ($FinalStatus -eq 'D') {
        $match = [regex]::Match($details[0],
            '^@AWJ-STUDIO/1 DETAIL 0 ([^ ]+) ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)$')
        if (-not $match.Success -or $match.Groups[1].Value -eq '-' -or
            [int]$match.Groups[2].Value -lt 1) {
            throw 'Successful DETAIL record is missing its encoder, threads, or stage timings.'
        }
    }
    return $details[0]
}

function Get-DescendantProcessIds([int]$RootProcessId) {
    $processes = @(Get-CimInstance Win32_Process |
        Select-Object ProcessId, ParentProcessId)
    $pending = [Collections.Generic.Queue[uint32]]::new()
    $pending.Enqueue([uint32]$RootProcessId)
    $result = [Collections.Generic.List[int]]::new()
    while ($pending.Count -gt 0) {
        $parent = $pending.Dequeue()
        foreach ($child in $processes | Where-Object ParentProcessId -eq $parent) {
            $result.Add([int]$child.ProcessId)
            $pending.Enqueue([uint32]$child.ProcessId)
        }
    }
    return @($result)
}

function Assert-ProcessesExited([int[]]$ProcessIds, [string]$Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $alive = @($ProcessIds | Where-Object {
            $null -ne (Get-Process -Id $_ -ErrorAction SilentlyContinue)
        })
        if ($alive.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "$Description left processes running: $($alive -join ', ')"
}

$captures = [Collections.Generic.List[object]]::new()
$forcedJob = $null
$cancelEvent = $null

try {
    if (Test-Path -LiteralPath $workRoot) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $workRoot | Out-Null

    $protocolOutput = Join-Path $workRoot 'protocol-output'
    New-Item -ItemType Directory -Path $protocolOutput | Out-Null
    $protocolManifest = Join-Path $workRoot 'protocol.awjq'
    Write-StudioManifest $protocolManifest @($validInput) $protocolOutput
    $protocolCapture = Start-AwjCapture (
        New-WorkerArguments $protocolOutput $protocolManifest)
    $captures.Add($protocolCapture)
    $protocolResult = Complete-AwjCapture $protocolCapture
    if ($protocolResult.ExitCode -ne 0) {
        throw "Successful manifest probe exited $($protocolResult.ExitCode): $($protocolResult.Stderr)"
    }
    $protocolDetail = Assert-StudioProtocol $protocolResult.Stdout 'D'

    $retryOutput = Join-Path $workRoot 'retry-output'
    New-Item -ItemType Directory -Path $retryOutput | Out-Null
    $retryManifest = Join-Path $workRoot 'retry.awjq'
    Write-StudioManifest $retryManifest @($failedInput) $retryOutput
    $retryCapture = Start-AwjCapture (
        New-WorkerArguments $retryOutput $retryManifest)
    $captures.Add($retryCapture)
    $retryResult = Complete-AwjCapture $retryCapture
    if ($retryResult.ExitCode -ne 2) {
        throw "Failed-item manifest probe exited $($retryResult.ExitCode), expected 2."
    }
    $retryDetail = Assert-StudioProtocol $retryResult.Stdout 'F'
    if (@($retryResult.Stdout -split '\r?\n' | Where-Object {
            $_ -and -not $_.StartsWith('@AWJ-STUDIO/')
        }).Count -eq 0) {
        throw 'Failed-item manifest probe did not preserve the complete CLI error text.'
    }

    $ordinaryOutput = Join-Path $workRoot 'ordinary-output'
    New-Item -ItemType Directory -Path $ordinaryOutput | Out-Null
    $ordinaryManifest = Join-Path $workRoot 'ordinary.awjq'
    Write-StudioManifest $ordinaryManifest $inputs $ordinaryOutput
    $eventName = "Local\AWJ-CliSmoke-$PID-$([Guid]::NewGuid().ToString('N'))"
    $createdNew = $false
    $cancelEvent = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::ManualReset, $eventName,
        [ref]$createdNew)
    if (-not $createdNew) {
        throw "Named cancellation event already exists: $eventName"
    }
    $ordinaryCapture = Start-AwjCapture (
        New-WorkerArguments $ordinaryOutput $ordinaryManifest $eventName)
    $captures.Add($ordinaryCapture)
    if ($ordinaryCapture.Process.WaitForExit($ActionDelayMilliseconds)) {
        throw "Ordinary-cancel worker exited early with code $($ordinaryCapture.Process.ExitCode)."
    }
    [int[]]$ordinaryTree = @($ordinaryCapture.Process.Id) +
        @(Get-DescendantProcessIds $ordinaryCapture.Process.Id)
    [void]$cancelEvent.Set()
    $ordinaryResult = Complete-AwjCapture $ordinaryCapture
    if ($ordinaryResult.ExitCode -ne 130 -or
        $ordinaryResult.Stdout -notmatch '已取消') {
        throw "Named-event cancellation exited $($ordinaryResult.ExitCode) without a cancellation summary."
    }
    Assert-ProcessesExited $ordinaryTree 'Named-event cancellation'

    $forcedOutput = Join-Path $workRoot 'forced-output'
    New-Item -ItemType Directory -Path $forcedOutput | Out-Null
    $forcedManifest = Join-Path $workRoot 'forced.awjq'
    Write-StudioManifest $forcedManifest $inputs $forcedOutput
    [string[]]$forcedArguments = New-WorkerArguments $forcedOutput $forcedManifest
    $forcedJob = [AwjCliSmokeJob]::Start(
        $exePath, $forcedArguments, [IO.Path]::GetDirectoryName($exePath))
    if ($forcedJob.Wait($ActionDelayMilliseconds)) {
        throw "Forced-stop worker exited early with code $($forcedJob.ExitCode)."
    }
    [int[]]$forcedTree = @($forcedJob.ProcessId) +
        @(Get-DescendantProcessIds $forcedJob.ProcessId)
    $forcedJob.Terminate(130)
    if (-not $forcedJob.Wait($TimeoutSeconds * 1000) -or
        $forcedJob.ExitCode -ne 130) {
        throw "Job Object termination did not produce exit code 130."
    }
    Assert-ProcessesExited $forcedTree 'Job Object termination'

    [pscustomobject]@{
        UiLaunched = $false
        InputCount = $inputs.Count
        ProtocolExitCode = $protocolResult.ExitCode
        ProtocolDetail = $protocolDetail
        RetryExitCode = $retryResult.ExitCode
        RetryDetail = $retryDetail
        OrdinaryExitCode = $ordinaryResult.ExitCode
        OrdinaryTreeProcessIds = $ordinaryTree -join ','
        ForcedExitCode = $forcedJob.ExitCode
        ForcedTreeProcessIds = $forcedTree -join ','
    }
}
finally {
    if ($null -ne $cancelEvent) {
        $cancelEvent.Dispose()
    }
    if ($null -ne $forcedJob) {
        $forcedJob.Dispose()
    }
    foreach ($capture in $captures) {
        if ($null -ne $capture -and -not $capture.Process.HasExited) {
            $capture.Process.Kill($true)
            $capture.Process.WaitForExit()
        }
        if ($null -ne $capture) {
            $capture.Process.Dispose()
        }
    }
    if (Test-Path -LiteralPath $workRoot) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
}
