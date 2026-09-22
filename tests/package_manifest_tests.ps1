$ErrorActionPreference = 'Stop'
# Load only pure selection functions; never run packaging or access signing keys.
$Path = Join-Path $PSScriptRoot '../scripts/package-release.ps1'
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw "Packaging script has parse errors: $Errors" }
foreach ($Name in @('Get-SortedEntries', 'Get-CurrentArchiveEntries')) {
    $Node = $Ast.Find({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq $Name }, $true)
    if (-not $Node) { throw "Missing function: $Name" }
    . ([scriptblock]::Create($Node.Extent.Text))
}
$History = @(
    @{version='1.0.9'; channel='stable'; revoked=$false},
    @{version='1.0.15'; channel='prerelease'; revoked=$false},
    @{version='1.0.10'; channel='stable'; revoked=$true})
$New = @{version='1.1.0'; channel='stable'; revoked=$false; assets=@{hash='unchanged'}}
$Selected = [object[]]@(Get-CurrentArchiveEntries $History $New)
if ($Selected.Count -ne 1 -or -not [object]::ReferenceEquals($Selected[0], $New)) {
    throw 'Stable release retained history or changed the target'
}
$RoundTrip = @{schema=2; sequence=42; entries=$Selected} | ConvertTo-Json -Depth 8 | ConvertFrom-Json
if ($RoundTrip.entries -isnot [array] -or $RoundTrip.entries.Count -ne 1) {
    throw 'Singleton entries lost JSON array shape'
}
$New.channel = 'prerelease'
$Selected = @(Get-CurrentArchiveEntries $History $New)
if ($Selected.Count -ne 2 -or $Selected[0].version -ne '1.0.10' -or -not $Selected[0].revoked) {
    throw 'Prerelease lost stable declaration or resurrected revoked target'
}
if (@(Get-CurrentArchiveEntries @() $New).Count -ne 1) { throw 'First release selection failed' }
$Rejected = $false
try { Get-CurrentArchiveEntries $History $History[0] | Out-Null } catch { $Rejected = $true }
if (-not $Rejected) { throw 'Duplicate version accepted' }
Write-Output 'package manifest selection passed'
