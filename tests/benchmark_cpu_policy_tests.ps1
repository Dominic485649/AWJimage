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
$tokens = $null
$parseErrors = $null
$null = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../scripts/benchmark.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
'CPU 基准策略测试通过：旧版能力识别、新版无旧开关、CPU 路径与拒绝 GPU/回退/未知状态。'
