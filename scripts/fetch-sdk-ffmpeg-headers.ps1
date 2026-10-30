$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Get-ChildItem -LiteralPath (Join-Path $root 'build') -Name | Out-Null
$download = Join-Path $root 'build/sdk-tools/ffmpeg-download'
$destination = Join-Path $root 'build/sdk-tools/ffmpeg-headers'
if ((Test-Path -LiteralPath $download) -or (Test-Path -LiteralPath $destination)) { throw 'FFmpeg 头文件目录已存在，拒绝覆盖。' }
New-Item -ItemType Directory -Path $download | Out-Null
New-Item -ItemType Directory -Path $destination | Out-Null
$archive = Join-Path $download 'ffmpeg-d8990137eec96e8ead292f5de2fc2c6de450e676.tar.gz'
& curl.exe --fail --location --output $archive 'https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/d8990137eec96e8ead292f5de2fc2c6de450e676'
if ($LASTEXITCODE) { throw '固定版本 FFmpeg 头文件下载失败。' }
Get-FileHash -LiteralPath $archive -Algorithm SHA256 | Format-List Path,Hash
& tar.exe -xzf $archive -C $destination --strip-components=1
if ($LASTEXITCODE) { throw 'FFmpeg 头文件归档解包失败。' }
'仅使用固定提交的 LGPL 公共头，不构建或分发现有 nonfree FFmpeg 运行库。'
