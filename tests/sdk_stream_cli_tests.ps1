param(
    [Parameter(Mandatory)][string]$StreamCli,
    [Parameter(Mandatory)][string]$Ffmpeg,
    [Parameter(Mandatory)][string]$WgcCli,
    [Parameter(Mandatory)][string]$WorkDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$work = [IO.Path]::GetFullPath($WorkDirectory)
if (-not $work.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw '测试目录必须在本工作树内。' }
Get-ChildItem -LiteralPath (Split-Path $work -Parent) -Name | Out-Null
if (Test-Path -LiteralPath $work) { throw '此集成测试需要全新空目录，不能覆盖旧证据。' }
New-Item -ItemType Directory -Path $work | Out-Null
function Start-BinaryProcess([string]$File, [string[]]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($File)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.WorkingDirectory = $work
    foreach ($arg in $Arguments) { $info.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info
    if (-not $process.Start()) { throw '不能启动测试子进程。' }
    return $process
}
function Invoke-Bytes([string]$File, [string[]]$Arguments, [byte[]]$InputBytes) {
    $process = Start-BinaryProcess $File $Arguments
    $memory = [IO.MemoryStream]::new()
    try {
        $errorTask = $process.StandardError.ReadToEndAsync()
        $outputTask = $process.StandardOutput.BaseStream.CopyToAsync($memory)
        $process.StandardInput.BaseStream.Write($InputBytes)
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) { $process.Kill($true); throw '测试子进程超时。' }
        $null = $outputTask.GetAwaiter().GetResult()
        return @{Code=$process.ExitCode; Bytes=$memory.ToArray(); Error=$errorTask.GetAwaiter().GetResult()}
    } finally { $memory.Dispose(); $process.Dispose() }
}
function Assert-Png([byte[]]$Bytes) {
    if ($Bytes.Length -lt 24 -or [Convert]::ToHexString($Bytes[0..7]) -ne '89504E470D0A1A0A') { throw '输出不是原生 PNG 码流。' }
}
# 真正 FFmpeg rawvideo -> 内存管道 -> C ABI SDK，像素和输出均不生成文件。
$producer = Start-BinaryProcess $Ffmpeg @('-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc=size=8x6:rate=1','-frames:v','2','-pix_fmt','rgba','-f','rawvideo','pipe:1')
$consumer = Start-BinaryProcess $StreamCli @('--raw','rgba8','8','6','--color','1','13','0','1','--codec','png','--lossless')
$memory = [IO.MemoryStream]::new()
try {
    $producer.StandardInput.Close()
    $producerError = $producer.StandardError.ReadToEndAsync(); $consumerError = $consumer.StandardError.ReadToEndAsync()
    $outputTask = $consumer.StandardOutput.BaseStream.CopyToAsync($memory)
    $transfer = $producer.StandardOutput.BaseStream.CopyToAsync($consumer.StandardInput.BaseStream)
    $null = $transfer.GetAwaiter().GetResult(); $consumer.StandardInput.Close()
    if (-not $producer.WaitForExit(30000) -or -not $consumer.WaitForExit(30000)) { $producer.Kill($true); $consumer.Kill($true); throw 'FFmpeg 流测试超时。' }
    $null = $outputTask.GetAwaiter().GetResult()
    if ($producer.ExitCode -ne 0 -or $consumer.ExitCode -ne 0) { throw ($producerError.GetAwaiter().GetResult() + $consumerError.GetAwaiter().GetResult()) }
    $bytes = $memory.ToArray(); $offset = 0
    for ($frame = 0; $frame -lt 2; ++$frame) {
        if ($offset + 40 -gt $bytes.Length -or [Text.Encoding]::ASCII.GetString($bytes,$offset,7) -ne 'AWJRES1') { throw '结果分帧头无效。' }
        if ([BitConverter]::ToUInt32($bytes,$offset+8) -ne 1 -or [BitConverter]::ToUInt32($bytes,$offset+12) -ne 40 -or [BitConverter]::ToUInt64($bytes,$offset+16) -ne $frame) { throw '结果帧号/版本无效。' }
        $size = [BitConverter]::ToUInt64($bytes,$offset+32); $offset += 40
        if ($size -gt $bytes.Length - $offset) { throw '结果 payload 越界。' }
        Assert-Png $bytes[$offset..($offset + $size - 1)]; $offset += $size
    }
    if ($offset -ne $bytes.Length) { throw 'stdout 混入日志或结果分帧不完整。' }
} finally { $memory.Dispose(); $producer.Dispose(); $consumer.Dispose() }
# 生成协议帧，只在 MemoryStream 中构造；截断帧必须失败。
$wire = [byte[]]::new(176)
[Text.Encoding]::ASCII.GetBytes("AWJFRM1`0").CopyTo($wire,0)
function Put32([int]$Offset, [uint32]$Value) { [BitConverter]::GetBytes($Value).CopyTo($wire,$Offset) }
Put32 8 1; Put32 12 160; Put32 16 2; Put32 20 2; Put32 24 1; Put32 28 8; Put32 32 1
Put32 56 1; Put32 60 1000; Put32 64 1; Put32 68 13; Put32 72 0; Put32 76 1; Put32 80 ([uint32]::MaxValue)
[BitConverter]::GetBytes([uint64]16).CopyTo($wire,96)
[BitConverter]::GetBytes([uint64]8).CopyTo($wire,104); Put32 136 2
for ($i=160; $i -lt 176; ++$i) { $wire[$i] = 255 }
$single = Invoke-Bytes $StreamCli @('--wire','--codec','png','--single') $wire
if ($single.Code -ne 0) { throw $single.Error }; Assert-Png $single.Bytes
$broken = Invoke-Bytes $StreamCli @('--wire','--codec','png') $wire[0..170]
if ($broken.Code -eq 0 -or $broken.Bytes.Length) { throw '截断输入未被拒绝，或出现非完整结果。' }
if ((Get-ChildItem -LiteralPath $work -Force).Count) { throw '内存流模式创建了工作目录文件。' }
# 兼容旧 WGC 语法，仅最终文件 sink 写出；输入保持内存，不创建 raw 临时文件。
$half = [byte[]]::new(32)
for ($i=0; $i -lt 16; ++$i) { [BitConverter]::GetBytes([uint16]0x3c00).CopyTo($half,$i*2) }
$wgc = Invoke-Bytes $WgcCli @('--stdin-wgc-rgba16f','2x2','-o',$work,'--format','png','--no-log','--no-summary') $half
if ($wgc.Code -ne 0) { throw ([Text.Encoding]::UTF8.GetString($wgc.Bytes) + $wgc.Error) }
$files = @(Get-ChildItem -LiteralPath $work -File -Recurse)
if ($files.Count -ne 1 -or $files[0].Extension -ne '.png') { throw '旧 WGC CLI 未仅生成最终 PNG。' }
Assert-Png ([IO.File]::ReadAllBytes($files[0].FullName))
'真实 FFmpeg 多帧、AWJFRM1/AWJRES1、单帧码流、截断和旧 WGC 无 raw 临时输入集成验证通过。'
'验证边界：检查了测试工作目录和已移除的 spooling 路径，未进行整个进程的系统级文件 I/O 跟踪。'
