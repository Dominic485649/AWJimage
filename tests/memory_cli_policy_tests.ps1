[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cli,
    [Parameter(Mandatory)][string]$WgcCli,
    [Parameter(Mandatory)][string]$InputImage,
    [Parameter(Mandatory)][string]$WorkRoot
)
$ErrorActionPreference = 'Stop'
$Cli = (Resolve-Path -LiteralPath $Cli).Path
$WgcCli = (Resolve-Path -LiteralPath $WgcCli).Path
$InputImage = (Resolve-Path -LiteralPath $InputImage).Path
$root = [IO.Path]::GetFullPath($WorkRoot)
New-Item -ItemType Directory -Force -Path $root | Out-Null
$run = Join-Path $root ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$records = [Collections.Generic.List[object]]::new()

function Invoke-PolicyCase {
    param([string]$Name, [string]$Executable, [string[]]$Arguments,
          [byte[]]$InputBytes = @(), [bool]$Success, [string]$Diagnostic = '')
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.WorkingDirectory = $run
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardInputEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw "无法启动 CLI 用例：$Name" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        try {
            if ($InputBytes.Length) { $process.StandardInput.BaseStream.Write($InputBytes, 0, $InputBytes.Length) }
            $process.StandardInput.Close()
        } catch [IO.IOException] {
            if ($Success) { throw }
            # 拒绝发生在读取前时，子进程可合法先关闭stdin；仍必须核对退出码和真实诊断。
            try { $process.StandardInput.Close() } catch [IO.IOException] {}
        }
        if (-not $process.WaitForExit(30000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "CLI 用例超时：$Name"
        }
        $code = $process.ExitCode
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    } finally { $process.Dispose() }
    [IO.File]::WriteAllText((Join-Path $run "$Name.log"), $text, [Text.UTF8Encoding]::new($false))
    $records.Add([ordered]@{name=$Name; executable=$Executable; arguments=$Arguments;
        stdin_bytes=$InputBytes.Length; exit_code=$code; expected_success=$Success})
    if (($code -eq 0) -ne $Success) { throw "CLI 用例退出状态错误：$Name，exit=$code；$text" }
    if ($Diagnostic -and $text -notmatch $Diagnostic) { throw "CLI 用例未返回预期诊断：$Name；$text" }
    return $text
}

function Assert-Png([string]$Path, [uint32]$Width, [uint32]$Height) {
    $data = [IO.File]::ReadAllBytes($Path)
    if ($data.Length -lt 33 -or [Convert]::ToHexString($data[0..7]) -cne '89504E470D0A1A0A' -or
        [Text.Encoding]::ASCII.GetString($data, 12, 4) -cne 'IHDR') { throw "无有效 PNG 输出：$Path" }
    $actualWidth = ([uint64]$data[16] -shl 24) -bor ([uint64]$data[17] -shl 16) -bor ([uint64]$data[18] -shl 8) -bor $data[19]
    $actualHeight = ([uint64]$data[20] -shl 24) -bor ([uint64]$data[21] -shl 16) -bor ([uint64]$data[22] -shl 8) -bor $data[23]
    if ($actualWidth -ne $Width -or $actualHeight -ne $Height) { throw "PNG 尺寸错误：$Path" }
}

try {
    $common = @('-f','png','-q','100','-t','1','--image-size-limit','none','--no-delete-source','--no-log')
    # 2x2 scRGB RGBA16F，0 和 1 的 half-float 表示；仅 32 字节。
    [byte[]]$frame = @(0,60,0,0,0,0,0,60, 0,0,0,60,0,0,0,60,
                      0,0,0,0,0,60,0,60, 0,60,0,60,0,60,0,60)
    $finiteProbe = @('--studio-probe-image',$InputImage,'native','--studio-input-limit-bytes','1')
    $null = Invoke-PolicyCase 'probe-finite' $Cli $finiteProbe -Success $false -Diagnostic '大小|限制|上限|保护'
    $unlockedProbe = @('--studio-probe-image',$InputImage,'native','--studio-input-limit-bytes','1',
        '--unlock-memory-limit','--no-unlock-max-input-file-bytes')
    $null = Invoke-PolicyCase 'probe-unlocked' $Cli $unlockedProbe -Success $true -Diagnostic '^\s*\d+\s+\d+\s*$'

    $finiteOutput = Join-Path $run 'wgc-finite'
    $finiteWgc = @('--stdin-wgc-rgba16f','2x2','-o',$finiteOutput) + $common +
        @('--memory-limit','64MiB','--studio-input-limit-bytes','31')
    $null = Invoke-PolicyCase 'wgc-finite' $WgcCli $finiteWgc -InputBytes $frame -Success $false -Diagnostic '帧超过输入或配置内存上限'
    if (Test-Path -LiteralPath $finiteOutput) { throw '有限 WGC 拒绝后仍生成输出。' }

    foreach ($order in 0,1) {
        $output = Join-Path $run "wgc-unlocked-$order"
        $flags = if ($order -eq 0) { @('--unlock-memory-limit','--no-unlock-max-input-file-bytes') }
                 else { @('--no-unlock-max-input-file-bytes','--unlock-memory-limit') }
        $arguments = @('--stdin-wgc-rgba16f','2x2','-o',$output) + $common +
            @('--memory-limit','1B','--studio-input-limit-bytes','31') + $flags
        $null = Invoke-PolicyCase "wgc-unlocked-$order" $WgcCli $arguments -InputBytes $frame -Success $true
        Assert-Png (Join-Path $output 'stdin-wgc-rgba16f.png') 2 2
    }
    $badOutput = Join-Path $run 'wgc-invalid'
    $unlockedWgc = @('--stdin-wgc-rgba16f','2x2','-o',$badOutput) + $common + @('--memory-limit','1B','--unlock-memory-limit')
    $null = Invoke-PolicyCase 'wgc-short' $WgcCli $unlockedWgc -InputBytes $frame[0..15] -Success $false -Diagnostic '数据短于声明'
    $null = Invoke-PolicyCase 'wgc-trailing' $WgcCli $unlockedWgc -InputBytes ([byte[]]($frame + 0)) -Success $false -Diagnostic '数据长于声明'
    $overflow = @('--stdin-wgc-rgba16f','4294967295x4294967295','-o',$badOutput) + $common + @('--unlock-memory-limit')
    $null = Invoke-PolicyCase 'wgc-representation' $WgcCli $overflow -Success $false -Diagnostic '帧超过输入或配置内存上限'
    if (Test-Path -LiteralPath $badOutput) { throw '非法 WGC 帧生成了输出。' }

    $normalOutput = Join-Path $run 'normal-unlocked'
    $normal = @('-i',$InputImage,'-o',$normalOutput) + $common +
        @('--memory-limit','1B','--studio-input-limit-bytes','1','--unlock-memory-limit','--no-unlock-max-input-file-bytes')
    $null = Invoke-PolicyCase 'normal-unlocked' $Cli $normal -Success $true
    Assert-Png (Join-Path $normalOutput 'pixel.png') 1 1
    $legacyOutput = Join-Path $run 'legacy-too-small.png'
    $legacy = @('-i',$InputImage,'-o',$legacyOutput) + $common + @('--memory-limit','1B','--unlock-max-input-file-bytes')
    $null = Invoke-PolicyCase 'legacy-retains-working-budget' $Cli $legacy -Success $false -Diagnostic 'memory-limit 不能低于 64MiB'
    if (Test-Path -LiteralPath $legacyOutput) { throw '仅输入解除错误地放开了工作集预算。' }

    [ordered]@{kind='memory-cli-boundary-validation';status='passed';cases=$records;
        cli_sha256=(Get-FileHash -LiteralPath $Cli -Algorithm SHA256).Hash.ToLowerInvariant();
        wgc_cli_sha256=(Get-FileHash -LiteralPath $WgcCli -Algorithm SHA256).Hash.ToLowerInvariant();
        input_sha256=(Get-FileHash -LiteralPath $InputImage -Algorithm SHA256).Hash.ToLowerInvariant();
        native_allocation_exhaustion_tested=$false} | ConvertTo-Json -Depth 7 |
        Set-Content -LiteralPath (Join-Path $run 'validation.json') -Encoding utf8
    Write-Output "CLI/probe/WGC 内存策略边界通过：$($records.Count) 项；$run"
} catch {
    [ordered]@{kind='memory-cli-boundary-validation';status='failed';message=$_.Exception.Message;cases=$records} |
        ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $run 'failure.json') -Encoding utf8
    throw
}
