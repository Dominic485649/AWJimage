#requires -Version 7.4
param(
 [Parameter(Mandatory)][string]$Executable,
 [Parameter(Mandatory)][string]$WorkDirectory,
 [ValidateSet('software','auto')][string]$Backend='software'
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class AwjIconCheck {
 [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h,uint a,out int v,int n);
}
"@
# GUI startup reconciles installed Explorer menus. Preserve its registry roots.
function Get-AwjMenuRoots {
 $classes='HKCU:\Software\Classes'
 $items=@(Get-ChildItem -LiteralPath $classes)
 $roots=@($items | Where-Object PSChildName -Like 'AWJ*.ContextMenu*')
 $items+=@(Get-ChildItem -LiteralPath "$classes\SystemFileAssociations")
 foreach($item in ($items | Where-Object PSChildName -NotLike 'AWJ*.ContextMenu*')) {
  $shell=Join-Path $item.PSPath 'shell'
  if(Test-Path -LiteralPath $shell) {
   $roots+=@(Get-ChildItem -LiteralPath $shell | Where-Object PSChildName -Like 'AWJ*')
  }
 }
 $roots | Select-Object -ExpandProperty Name -Unique
}
$menuBackups=@()
$menuSnapshotReady=$false
$work=[IO.Path]::GetFullPath($WorkDirectory)
if(Test-Path -LiteralPath $work){throw 'WorkDirectory must be fresh.'}
$executable=(Resolve-Path -LiteralPath $Executable).Path
$previousBackend=$env:SLINT_BACKEND
$env:SLINT_BACKEND=if($Backend -eq 'auto'){$null}else{$Backend}
try {
New-Item -ItemType Directory -Path $work -Force | Out-Null
foreach($key in @(Get-AwjMenuRoots)) {
 $file=Join-Path $work ("menu-backup-{0}.reg" -f $menuBackups.Count)
 & reg.exe export $key $file /y *> $null
 if($LASTEXITCODE){throw "Cannot preserve menu registry root: $key"}
 $menuBackups+=@{key=$key;file=$file}
}
$menuSnapshotReady=$true
Copy-Item -LiteralPath $executable -Destination (Join-Path $work 'AWJ.exe')
$cli=Join-Path (Split-Path -Parent $executable) 'AWJ.com'
if(Test-Path -LiteralPath $cli){Copy-Item -LiteralPath $cli -Destination (Join-Path $work 'AWJ.com')}
$themeKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$systemAppDark=(Get-ItemPropertyValue -LiteralPath $themeKey -Name AppsUseLightTheme) -eq 0
$systemTaskbarDark=(Get-ItemPropertyValue -LiteralPath $themeKey -Name SystemUsesLightTheme) -eq 0
$records=foreach($theme in @(0,1,2)) {
 @{theme_index=$theme;show_update_changelog=$false;last_successful_update_check_at=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'AWJ.jsonc') -Encoding utf8NoBOM
 $process=Start-Process -FilePath (Join-Path $work 'AWJ.exe') -WorkingDirectory $work -WindowStyle Hidden -RedirectStandardError (Join-Path $work "theme$theme.stderr.log") -RedirectStandardOutput (Join-Path $work "theme$theme.stdout.log") -PassThru
 $caseFailed=$false; try {
  $deadline=[DateTime]::UtcNow.AddSeconds(20)
  do{Start-Sleep -Milliseconds 100;$process.Refresh()}while(-not $process.HasExited -and $process.MainWindowHandle -eq 0 -and [DateTime]::UtcNow -lt $deadline)
  if($process.HasExited -or $process.MainWindowHandle -eq 0){throw 'No native window'}
  Start-Sleep -Seconds 4; $process.Refresh()
  Write-Host "Theme $theme title=$($process.MainWindowTitle) responsive=$($process.Responding) hwnd=$($process.MainWindowHandle)"
  $hashes=foreach($kind in @(0,1)) {
   $handle=[AwjIconCheck]::SendMessageW($process.MainWindowHandle,0x7F,[IntPtr]$kind,[IntPtr]::Zero)
   if($handle -eq [IntPtr]::Zero){throw "Missing icon $kind"}
   $icon=[Drawing.Icon]::FromHandle($handle);$bitmap=$icon.ToBitmap()
   try{$path=Join-Path $work "theme$theme-icon$kind.png";$bitmap.Save($path,[Drawing.Imaging.ImageFormat]::Png);(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}finally{$bitmap.Dispose();$icon.Dispose()}
  }
  $expectedDark=$theme -eq 2 -or ($theme -eq 0 -and $systemAppDark)
  $dark=0;$hr=[AwjIconCheck]::DwmGetWindowAttribute($process.MainWindowHandle,20,[ref]$dark,4)
  if($hr -eq 0 -and ([bool]$dark) -ne $expectedDark){throw "Wrong native caption scheme: $hr/$dark"}
  $caption=0;$captionHr=[AwjIconCheck]::DwmGetWindowAttribute($process.MainWindowHandle,35,[ref]$caption,4)
  $expectedCaption=if($expectedDark){0x242424}else{0xfbf4ee}
  if($captionHr -eq 0 -and $caption -ne $expectedCaption){throw "Caption does not match navigation: $caption"}
  [pscustomobject]@{app_theme=$theme;system_app_dark=$systemAppDark;system_taskbar_dark=$systemTaskbarDark;caption_dark=[bool]$dark;caption_color=$caption;caption_color_readback_supported=($captionHr -eq 0);small_icon_sha256=$hashes[0];large_icon_sha256=$hashes[1]}
 }catch{$caseFailed=$true;throw}finally{
  if(-not $process.HasExited){[void][AwjIconCheck]::PostMessageW($process.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero);if(-not $process.WaitForExit(15000)){$process.Kill($true);$process.WaitForExit();if(-not $caseFailed){throw 'Native window close timed out'}}}
  if(-not $caseFailed -and $process.ExitCode -ne 0){throw "Native window exit $($process.ExitCode)"}
  $process.Dispose()
 }
}
if(@($records.large_icon_sha256 | Select-Object -Unique).Count -ne 1){throw 'App theme changed the taskbar icon'}
if($records[1].small_icon_sha256 -eq $records[2].small_icon_sha256){throw 'Caption icon did not follow app theme'}
if($records[0].small_icon_sha256 -ne $records[$(if($systemAppDark){2}else{1})].small_icon_sha256){throw 'Automatic caption icon does not follow the app theme'}
$records | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $work 'results.json') -Encoding utf8NoBOM
$records | Format-Table


}finally{
 $env:SLINT_BACKEND=$previousBackend
 # Each deletion is confined to AWJ menu roots enumerated above.
 if($menuSnapshotReady) {
  foreach($key in @(Get-AwjMenuRoots)) {
   & reg.exe delete $key /f *> $null
   if($LASTEXITCODE){throw "Cannot restore menu registry root: $key"}
  }
  foreach($backup in $menuBackups) {
   & reg.exe import $backup.file *> $null
   if($LASTEXITCODE){throw "Cannot restore menu registry root: $($backup.key)"}
  }
 }
}
