Set-StrictMode -Version Latest

function Test-AwjBenchmarkLegacyCpuControl {
    param([AllowEmptyString()][string]$HelpText)
    # 只兼容被比较的旧程序；新版应用不重新接受已删除的 GPU 参数。
    return [regex]::IsMatch($HelpText, '(?m)^\s*--no-visual-quality-gpu(?:\s|$)')
}

function Get-AwjBenchmarkLegacyCpuControl {
    param([Parameter(Mandatory)][string]$Executable)
    $help = (& $Executable --help 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "无法核对基准程序的 CPU 控制能力：$Executable" }
    return Test-AwjBenchmarkLegacyCpuControl -HelpText $help
}

function Assert-AwjBenchmarkCpuRows {
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows)
    foreach ($row in $Rows) {
        foreach ($name in @('visual_quality_gpu_requested', 'visual_quality_gpu_used')) {
            $property = $row.PSObject.Properties[$name]
            if ($null -ne $property -and [string]$property.Value -notin @('false', '0')) {
                throw "cpu-search 的旧版摘要仍请求或使用 GPU：$name=$($property.Value)"
            }
        }
        $path = $row.PSObject.Properties['visual_quality_gpu_path']
        if ($null -ne $path -and [string]$path.Value -notin @('cpu-disabled', 'not-requested')) {
            throw "cpu-search 的旧版指标路径不是固定 CPU：$($path.Value)"
        }
    }
}

function Get-AwjBenchmarkSummaryPath {
    param([Parameter(Mandatory)][string]$OutputDirectory)
    # 兼容旧版及 FFmpeg 对照的 summary.csv；拒绝两个报告并存时猜测来源。
    $candidates = @(
        (Join-Path $OutputDirectory 'log/summery.csv'),
        (Join-Path $OutputDirectory 'summary.csv')
    )
    $found = @($candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    if ($found.Count -ne 1) {
        throw "基准输出必须恰有一份 CSV 报告，实际为 $($found.Count)：$OutputDirectory"
    }
    return $found[0]
}

Export-ModuleMember -Function Test-AwjBenchmarkLegacyCpuControl, Get-AwjBenchmarkLegacyCpuControl, Assert-AwjBenchmarkCpuRows, Get-AwjBenchmarkSummaryPath
