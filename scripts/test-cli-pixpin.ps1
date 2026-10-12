param(
    [Parameter(Mandatory)] [string]$SourceDir,
    [string]$BuildDir = 'build\x64\Release',
    [string]$Configuration = 'Release',
    [int]$SampleCount = 50
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$cli = Join-Path $repo 'bin\x64\Release\AWJ.com'
$runId = Get-Date -Format 'yyyyMMdd-HHmmss'
$workRoot = Join-Path $repo "build\cli-pixpin-test\$runId"
$sampleDir = Join-Path $workRoot 'input'
$outRoot = Join-Path $workRoot 'output'

if (-not (Test-Path $SourceDir)) {
    throw "测试集目录不存在：$SourceDir"
}

cmake --build (Join-Path $repo $BuildDir) --config $Configuration --target AWJ AWJ-com | Write-Host
if ($LASTEXITCODE -ne 0) {
    throw "AWJ 构建失败：$LASTEXITCODE"
}
if (-not (Test-Path $cli)) {
    throw "找不到 AWJ：$cli"
}

New-Item -ItemType Directory -Path $sampleDir -Force | Out-Null
New-Item -ItemType Directory -Path $outRoot -Force | Out-Null

$supported = @('.jpg', '.jpeg', '.png', '.webp', '.bmp', '.tif', '.tiff', '.gif', '.jxl', '.jp2', '.heic', '.heif', '.avif')
$images = Get-ChildItem -Path $SourceDir -File -Recurse |
    Where-Object { $supported -contains $_.Extension.ToLowerInvariant() } |
    Sort-Object FullName |
    Select-Object -First $SampleCount

if ($images.Count -eq 0) {
    throw "测试集没有找到支持的图片：$SourceDir"
}

$index = 1
foreach ($image in $images) {
    $target = Join-Path $sampleDir ('{0:D3}{1}' -f $index, $image.Extension.ToLowerInvariant())
    Copy-Item -LiteralPath $image.FullName -Destination $target -Force
    $index++
}

$cases = @(
    @{ Name = 'avif-default'; Args = @('-i', $sampleDir, '-o', (Join-Path $outRoot 'avif-default'), '-f', 'avif', '--summary', '--log', '--overwrite') },
    @{ Name = 'webp-default'; Args = @('-i', $sampleDir, '-o', (Join-Path $outRoot 'webp-default'), '-f', 'webp', '--summary', '--overwrite') },
    @{ Name = 'jxl-default'; Args = @('-i', $sampleDir, '-o', (Join-Path $outRoot 'jxl-default'), '-f', 'jxl', '--summary', '--overwrite') },
    @{ Name = 'avif-strip'; Args = @('-i', $sampleDir, '-o', (Join-Path $outRoot 'avif-strip'), '-f', 'avif', '--strip', '--chroma', '420', '--bit-depth', '8', '--template', '{index}_{name}_{params}', '--suffix-random', '--summary') },
    @{ Name = 'webp-skip-existing'; Args = @('-i', $sampleDir, '-o', (Join-Path $outRoot 'webp-default'), '-f', 'webp', '--skip-existing', '--summary') },
    @{ Name = 'avif-visual-quality-smoke'; Args = @('-i', (Get-ChildItem -Path $sampleDir -File | Select-Object -First 1).FullName, '-o', (Join-Path $outRoot 'avif-visual-quality-smoke'), '-f', 'avif', '--visual-quality', '88', '--summary') },
    @{ Name = 'jxl-visual-quality-smoke'; Args = @('-i', (Get-ChildItem -Path $sampleDir -File | Select-Object -First 1).FullName, '-o', (Join-Path $outRoot 'jxl-visual-quality-smoke'), '-f', 'jxl', '--visual-quality', '88', '--summary') }
)

$results = @()
foreach ($case in $cases) {
    $started = Get-Date
    Write-Host "`n==> $($case.Name)"
    & $cli @($case.Args)
    $exitCode = $LASTEXITCODE
    $elapsed = (Get-Date) - $started
    $results += [pscustomobject]@{
        Case = $case.Name
        ExitCode = $exitCode
        Seconds = [math]::Round($elapsed.TotalSeconds, 2)
    }
    if ($exitCode -ne 0) {
        throw "测试失败：$($case.Name)，退出码 $exitCode"
    }
}

$results | Format-Table -AutoSize
Write-Host "`n样本目录：$sampleDir"
Write-Host "输出目录：$outRoot"
