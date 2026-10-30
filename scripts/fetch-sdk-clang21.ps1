$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
Get-ChildItem -LiteralPath $build -Name | Out-Null
$download = Join-Path $build 'sdk-tools/clang21-download'
$destination = Join-Path $build 'sdk-tools/clang21-extracted'
if ((Test-Path -LiteralPath $download) -or (Test-Path -LiteralPath $destination)) { throw '工具链目录已存在，拒绝覆盖。' }
New-Item -ItemType Directory -Path $download | Out-Null
$archive = Join-Path $download 'LLVM-21.1.8-win64.exe'
& curl.exe --fail --location --output $archive 'https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/LLVM-21.1.8-win64.exe'
if ($LASTEXITCODE) { throw 'Clang 21 下载失败。' }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($hash -cne '7a5386c26497db1691f320121e5b113364dd0274b98e55f15f4dbc00c0450113') { throw '官方资产 SHA-256 不匹配，拒绝使用。' }
New-Item -ItemType Directory -Path $destination | Out-Null
& 7z.exe x $archive "-o$destination" -y | Select-Object -Last 8
if ($LASTEXITCODE) { throw 'Clang 21 隔离解包失败。' }
& (Join-Path $destination 'bin/clang-cl.exe') --version
if ($LASTEXITCODE) { throw 'Clang 21 自检失败。' }
[pscustomobject]@{ archive_sha256=$hash; compiler=(Join-Path $destination 'bin/clang-cl.exe'); installed_globally=$false }
