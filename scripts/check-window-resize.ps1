#requires -Version 7.4
param([Parameter(Mandatory)][string]$Executable,[Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeResize {
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
}
'@
if(Test-Path -LiteralPath $OutputDirectory){throw 'OutputDirectory must be fresh.'}
$exe=(Resolve-Path -LiteralPath $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$copy=Join-Path $OutputDirectory 'resize-fixture.exe'
Copy-Item -LiteralPath $exe -Destination $copy
$previousBackend=$env:SLINT_BACKEND
$records=@()
try {
 foreach($backend in @('auto','software')) {
  $env:SLINT_BACKEND=if($backend -eq 'auto'){$null}else{$backend}
  foreach($theme in @(1,2)) {
   $stdout=Join-Path $OutputDirectory "$backend-$theme.frames.csv"
   $p=Start-Process -FilePath $copy -ArgumentList @('--resize-window',$theme) -WorkingDirectory $OutputDirectory -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $OutputDirectory "$backend-$theme.stderr.log")
   $failed=$false
   $handle=[IntPtr]::Zero
   try {
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    do{Start-Sleep -Milliseconds 50;$p.Refresh();$windowLine=Get-Content -LiteralPath $stdout | Where-Object {$_ -like 'resize-window,*'} | Select-Object -First 1}while(-not $p.HasExited -and -not $windowLine -and [DateTime]::UtcNow -lt $deadline)
    if(-not $windowLine){throw 'Fixture did not report its renderer window.'}
    $handle=[IntPtr]::new([Convert]::ToInt64(($windowLine -split ',')[1],16))
    Start-Sleep -Milliseconds 400
    for($i=0;$i -lt 120;$i++) {
     if($p.HasExited){throw 'Resize fixture exited during resize.'}
     # Same external SetWindowPos flags as AltSnap; no system settings changed.
     if(-not [NativeResize]::SetWindowPos($handle,[IntPtr]::Zero,80+($i%9)*3,70+($i%7)*3,(1400+[int](180*[Math]::Sin($i*.27))),(1020+[int](130*[Math]::Cos($i*.21))),0x214)){throw 'SetWindowPos failed.'}
     Start-Sleep -Milliseconds 20
    }
    Start-Sleep -Milliseconds 600
    $p.Refresh();if(-not $p.Responding){throw 'Native window is unresponsive.'}
   }catch{$failed=$true;throw}finally{
    if(-not $p.HasExited){if($handle -ne [IntPtr]::Zero){[void][NativeResize]::PostMessageW($handle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)};if(-not $p.WaitForExit(10000)){$p.Kill();$p.WaitForExit();if(-not $failed){throw 'Native close timed out.'}}}
    if(-not $failed -and $p.ExitCode){throw "Resize fixture exit $($p.ExitCode)"};$p.Dispose()
   }
   $frames=@(Get-Content -LiteralPath $stdout | Where-Object {$_ -like 'resize-frame,*'})
   $sizes=@($frames | ForEach-Object {($_ -split ',')[1..2] -join ','} | Select-Object -Unique)
   if($frames.Count -lt 10 -or $sizes.Count -lt 10 -or @($frames | Where-Object {$_ -notlike '*,pass'}).Count){throw 'Insufficient or invalid resize readback frames.'}
   $records+=@{backend=$backend;theme=$theme;native_resize_calls=120;renderer_frames=$frames.Count;distinct_sizes=$sizes.Count;invalid_frames=0;exit_code=0}
  }
 }
 $result=@{method='external SetWindowPos and native renderer readback; actual AltSnap mouse gesture and DWM screen composition not automated';fixture_sha256=(Get-FileHash -LiteralPath $copy).Hash.ToLowerInvariant();cases=$records}
 $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Encoding utf8NoBOM
 $result | ConvertTo-Json -Depth 5
}finally{$env:SLINT_BACKEND=$previousBackend}
