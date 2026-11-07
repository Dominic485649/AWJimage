param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath($OutputDirectory)
$allowed = [IO.Path]::GetFullPath((Join-Path $repo 'build/release-1.3.3')) + [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)) { throw '夹具目录必须位于本项目 1.3.3 构建根且全新。' }
New-Item -ItemType Directory -Path $output | Out-Null
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'scripts/package-release.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw '发布脚本语法错误。' }
$function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-AdditionalReleaseAssets' }, $true)
$guards = @($function.Body.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text.StartsWith('$HmosSdkPreview -and') })
if ($guards.Count -ne 1) { throw '预览版本边界必须唯一。' }
# 只执行真实白名单边界，不运行打包主流程、签名或任何目标程序。
$guard = [scriptblock]::Create($guards[0].Extent.Text)
$records = @()
$cases = @(@{version='1.3.1';allow=$true}, @{version='1.3.3';allow=$true}, @{version='1.3.2';allow=$false}, @{version='1.3.4';allow=$false})
foreach ($case in $cases) {
  $Version = $case.version; $LocalCandidate = $false; $HmosSdkPreview = $true
  $accepted = $false; $errorText = ''
  try { . $guard; $accepted = $true } catch { $errorText = $_.Exception.Message }
  if ($accepted -ne $case.allow -or (-not $accepted -and $errorText -notmatch '预览范围')) { throw "PowerShell 版本范围错误：$Version" }
  $records += @{engine='PowerShell';version=$Version;accepted=$accepted;expected=$case.allow}
  $probe = @'
cmake_minimum_required(VERSION 3.30)
include("@INCLUDE@")
awj_sdk_select_profile("@VERSION@" hmos-arm64-api26-clang23 Linux x86_64 p)
awj_sdk_require_hmos_preview_scope("@VERSION@" p)
'@
  $probe = $probe.Replace('@INCLUDE@', (Join-Path $repo 'cmake/sdk-preview-profile.cmake').Replace('\','/')).Replace('@VERSION@',$Version)
  $probePath = Join-Path $output "scope-$Version.cmake"
  [IO.File]::WriteAllText($probePath, $probe, [Text.UTF8Encoding]::new($false))
  $text = & cmake -P $probePath 2>&1
  $accepted = $LASTEXITCODE -eq 0
  $text | Set-Content -LiteralPath (Join-Path $output "cmake-$Version.log") -Encoding utf8
  if ($accepted -ne $case.allow -or (-not $accepted -and "$text" -notmatch '预览范围')) { throw "CMake 版本范围错误：$Version" }
  $records += @{engine='CMake';version=$Version;accepted=$accepted;expected=$case.allow}
}
$Version = '1.3.3'; $LocalCandidate = $true; $HmosSdkPreview = $true
$accepted = $false; $errorText = ''
try { . $guard; $accepted = $true } catch { $errorText = $_.Exception.Message }
if ($accepted -or $errorText -notmatch '预览范围') { throw '预览开关不能用于本地候选模式。' }
$records += @{engine='PowerShell';version=$Version;local_candidate=$true;accepted=$false;expected=$false}
[ordered]@{fixture_only=$true;scope='explicit-hmos-preview-version-guards';passed=$records.Count;device_execution='not-run';checks=$records} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'summary.json') -Encoding utf8
"$($records.Count) 个版本白名单检查通过；仅元数据夹具，不是 SDK 或设备运行验收。"
