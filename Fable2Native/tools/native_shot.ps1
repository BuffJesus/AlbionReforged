# native_shot.ps1 — launch f2native_frontend (D3D12) and PrintWindow-capture the flip-model window.
param(
    [string]$Mode = "--skip-intro --start-menu",
    [string]$Tag  = "baseline",
    [int]$WaitSeconds = 8,
    [int]$WindowWaitSeconds = 30
)
$ErrorActionPreference = 'Continue'
$root   = "D:\Documents\Fable2RE"
$exe    = "$root\Fable2Native\build\RelWithDebInfo\f2native_frontend.exe"
$uiRoot = "$root\ghidra_out\title_ui_re"
$gameDir= "$root\Fable2Recomp\assets\game"
$shotDir= "C:\Users\Cornelio\AppData\Local\Temp\claude\D--Documents-Fable2RE\b2a27413-0e9c-4ef8-8f5a-336216f89ad4\scratchpad\shots"
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Cap {
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@ -ReferencedAssemblies System.Drawing

$full = "$Mode --ui-root `"$uiRoot`" --game-dir `"$gameDir`""
Write-Host "launch: $exe $full"
$p = Start-Process -FilePath $exe -ArgumentList $full -PassThru
Start-Sleep -Seconds $WaitSeconds

$proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if (-not $proc) { Write-Host "process exited early"; return }
$h = $proc.MainWindowHandle
$tries = 0
while ($h -eq 0 -and $tries -lt $WindowWaitSeconds) {
    Start-Sleep -Seconds 1
    $proc.Refresh()
    $h = $proc.MainWindowHandle
    $tries++
}
if ($h -eq 0) { Write-Host "no window handle"; Stop-Process -Id $p.Id -Force; return }

$r = New-Object Cap+R
[Cap]::GetClientRect($h, [ref]$r) | Out-Null
$w = $r.Rr - $r.L; $ht = $r.B - $r.T
Write-Host "window ${w}x${ht} handle=$h"
if ($w -le 0 -or $ht -le 0) { Stop-Process -Id $p.Id -Force; return }
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Cap]::PrintWindow($h, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$f = Join-Path $shotDir "native_${Tag}.png"
$bmp.Save($f, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "shot -> $f"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
