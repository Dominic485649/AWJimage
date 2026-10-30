param(
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$IncludeDirectory,
    [string]$ReadObject = 'llvm-readobj'
)
$ErrorActionPreference = 'Stop'
# 从实际安装的公共头生成精确白名单，不能仅检查 awj_ 前缀。
$expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($header in Get-ChildItem -LiteralPath (Join-Path $IncludeDirectory 'awj') -Filter '*.h' -File) {
    $text = [IO.File]::ReadAllText($header.FullName)
    foreach ($match in [regex]::Matches($text, 'AWJ_API\s+[^;]+?AWJ_CALL\s+(awj_[A-Za-z0-9_]+)\s*\(', [Text.RegularExpressions.RegexOptions]::Singleline)) {
        if (-not $expected.Add($match.Groups[1].Value)) { throw "重复的公共符号：$($match.Groups[1].Value)" }
    }
}
$output = & $ReadObject --coff-exports $Library
if ($LASTEXITCODE -ne 0) { throw '读取 DLL 导出失败。' }
$actual = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($line in $output) { if ($line -match '^\s*Name:\s+(.+)$') { [void]$actual.Add($Matches[1].Trim()) } }
if (-not $expected.Count -or -not $actual.SetEquals($expected)) {
    $extra = @($actual | Where-Object { -not $expected.Contains($_) })
    $missing = @($expected | Where-Object { -not $actual.Contains($_) })
    throw "DLL 导出不匹配；额外：$($extra -join ', ')；缺少：$($missing -join ', ')"
}
"DLL 精确导出白名单通过：$($actual.Count) 个公共 C 符号，无 C++ 或第三方接口。"
