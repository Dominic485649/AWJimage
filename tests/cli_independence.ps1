#requires -Version 7.4
param(
    [Parameter(Mandatory)] [string]$Cli,
    [Parameter(Mandatory)] [string]$Gui,
    [Parameter(Mandatory)] [string]$InputImage,
    [Parameter(Mandatory)] [string]$WorkDirectory
)
$ErrorActionPreference = 'Stop'
$work = [IO.Path]::GetFullPath($WorkDirectory)
$alone = Join-Path $work 'com-only'
New-Item -ItemType Directory -Path $alone -Force | Out-Null
Copy-Item -LiteralPath $Cli -Destination (Join-Path $alone 'AWJ.com') -Force
Copy-Item -LiteralPath $InputImage -Destination (Join-Path $alone 'input.jpg') -Force
if (Test-Path -LiteralPath (Join-Path $alone 'AWJ.exe')) { throw 'Isolation fixture contains a GUI.' }

function Invoke-Awj([string]$Program, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($Program)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = $alone
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill($true)
            throw 'AWJ test timed out.'
        }
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode) { throw "Exit $($process.ExitCode): $text" }
        return $text
    } finally { $process.Dispose() }
}

$program = Join-Path $alone 'AWJ.com'
$help = Invoke-Awj $program @('--help')
if ($help -notmatch '--format.*jpg' -or $help -notmatch '--jpeg-progressive-level') { throw 'Canonical JPG help is missing.' }
if ($help -notmatch '--cpu-affinity') { throw 'CPU affinity help is missing.' }
$parentMask = [Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity.ToInt64()
$cpuId = 0
while (($parentMask -band (1L -shl $cpuId)) -eq 0) { $cpuId++ }
$affinityOutput = Join-Path $alone 'affinity'
$affinityLog = Invoke-Awj $program @('--threads', '3', '--cpu-affinity', "$cpuId", '-i',
    (Join-Path $alone 'input.jpg'), '-o', $affinityOutput, '--format', 'jpg')
if (-not (Test-Path -LiteralPath (Join-Path $affinityOutput 'input.jpg'))) { throw 'CLI affinity conversion failed.' }
if ($affinityLog -notmatch 'CPU affinity: 1 logical CPUs; thread budget 3') { throw 'Explicit CLI budget was reserved after affinity.' }
$invalidOutput = Join-Path $alone 'invalid-affinity'
$invalidRejected = $false
try { $null = Invoke-Awj $program @('--cpu-affinity', '64', '-i', (Join-Path $alone 'input.jpg'), '-o', $invalidOutput) }
catch { $invalidRejected = $_.Exception.Message -match 'CPU|cpu-affinity' }
if (-not $invalidRejected -or (Test-Path -LiteralPath $invalidOutput)) { throw 'Invalid affinity did not fail before conversion.' }
$probe = Invoke-Awj $program @('--studio-probe-image', (Join-Path $alone 'input.jpg'), 'native')
if ($probe.Trim() -notmatch '^\d+ \d+$') { throw 'Standalone image probe failed.' }
foreach ($format in @('jpg', 'jpeg', 'jpgli', 'jpegli')) {
    $output = Join-Path $alone $format
    $null = Invoke-Awj $program @('-i', (Join-Path $alone 'input.jpg'), '-o', $output,
        '--format', $format, '--jpeg-progressive-level', '0')
    if (-not (Test-Path -LiteralPath (Join-Path $output 'input.jpg'))) { throw "Standalone $format conversion failed." }
}
$eventName = "Local\AWJIsolation-$PID"
$event = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $eventName)
try {
    $shellOutput = Join-Path $alone 'shell'
    $text = Invoke-Awj $program @('--shell-convert', '-i', (Join-Path $alone 'input.jpg'),
        '-o', $shellOutput, '--format', 'jpg', '--studio-cancel-event', $eventName)
    if ($text -notmatch '@AWJ-STUDIO/1 ITEM 0 D 1 1') { throw 'Shell worker progress was lost.' }
} finally { $event.Dispose() }

$legacyHelp = Invoke-Awj $Gui @('--help')
if ($legacyHelp -notmatch '--format.*jpg') { throw 'Legacy EXE forwarding failed.' }
$guiOutput = Join-Path $alone 'shell-gui'
$null = Invoke-Awj $Gui @('--shell-window', '--shell-convert', '-i', (Join-Path $alone 'input.jpg'),
    '-o', $guiOutput, '--format', 'jpg')
if (-not (Test-Path -LiteralPath (Join-Path $guiOutput 'input.jpg'))) { throw 'Shell GUI did not complete via the CLI.' }
Write-Output 'COM-only conversion/probe, JPG aliases, worker progress, EXE forwarding and shell GUI passed.'
