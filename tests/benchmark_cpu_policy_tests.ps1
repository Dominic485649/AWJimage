#requires -Version 7.4
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../scripts/benchmark-cpu-policy.psm1') -Force

if (-not (Test-AwjBenchmarkLegacyCpuControl -HelpText "  --no-visual-quality-gpu    固定 CPU`n")) {
    throw '未识别旧版明确提供的 CPU 控制开关。'
}
foreach ($help in @('', '  --visual-quality <1-100> 使用 CPU', '旧参数 --no-visual-quality-gpu 已删除')) {
    if (Test-AwjBenchmarkLegacyCpuControl -HelpText $help) { throw '错误识别了新版或仅在说明中的旧开关。' }
}
Assert-AwjBenchmarkCpuRows -Rows @([pscustomobject]@{status='ok'; visual_score='95'})
Assert-AwjBenchmarkCpuRows -Rows @([pscustomobject]@{
    visual_quality_gpu_requested='false'; visual_quality_gpu_used='false'; visual_quality_gpu_path='cpu-disabled'
})
Assert-AwjBenchmarkCpuRows -Rows @([pscustomobject]@{
    visual_quality_gpu_requested='0'; visual_quality_gpu_used='0'; visual_quality_gpu_path='not-requested'
})
foreach ($row in @(
    [pscustomobject]@{visual_quality_gpu_requested='true'; visual_quality_gpu_used='false'; visual_quality_gpu_path='cpu-fallback'},
    [pscustomobject]@{visual_quality_gpu_requested='false'; visual_quality_gpu_used='true'; visual_quality_gpu_path='d3d11'},
    [pscustomobject]@{visual_quality_gpu_requested='false'; visual_quality_gpu_used='false'; visual_quality_gpu_path='unknown'}
)) {
    $rejected = $false
    try { Assert-AwjBenchmarkCpuRows -Rows @($row) } catch { $rejected = $true }
    if (-not $rejected) { throw 'CPU 基准没有拒绝 GPU 请求、GPU 执行或未证明的路径。' }
}
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path $root ('build/test-benchmark-report-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path (Join-Path $fixture 'log') | Out-Null
    foreach ($phase in @('missing','legacy','ambiguous','current')) {
        if ($phase -eq 'legacy') { [IO.File]::WriteAllText((Join-Path $fixture 'summary.csv'), 'legacy') }
        if ($phase -eq 'ambiguous') { [IO.File]::WriteAllText((Join-Path $fixture 'log/summery.csv'), 'current') }
        if ($phase -eq 'current') { Remove-Item -LiteralPath (Join-Path $fixture 'summary.csv') -Confirm:$false }
        $rejected = $false
        $selected = $null
        try { $selected = Get-AwjBenchmarkSummaryPath -OutputDirectory $fixture } catch { $rejected = $true }
        if ($rejected -ne ($phase -in @('missing','ambiguous'))) { throw "报告选择策略不符：$phase" }
        if ($phase -eq 'legacy' -and $selected -ne (Join-Path $fixture 'summary.csv')) { throw '未选择旧版报告。' }
        if ($phase -eq 'current' -and $selected -ne (Join-Path $fixture 'log/summery.csv')) { throw '未选择新版报告。' }
    }
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force -Confirm:$false }
}
$blocked = $false
try {
    & (Join-Path $PSScriptRoot '../scripts/benchmark.ps1') -InputRoot $fixture `
        -AwjExecutable 'must-not-run' -ResultRoot $fixture -ExtraArguments '--delete-source' | Out-Null
} catch {
    if ($_.Exception.Message -notmatch '基准测试禁止回收源文件') { throw }
    $blocked = $true
}
if (-not $blocked) { throw '基准测试没有在访问语料前拒绝回收选项。' }
$tokens = $null
$parseErrors = $null
$null = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../scripts/benchmark.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
'基准策略测试通过：CPU 路径、新旧报告兼容、歧义拒绝及源语料保护。'
