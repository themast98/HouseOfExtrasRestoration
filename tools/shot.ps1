# Capture the Yakuza 4 window to a PNG.
#
# Uses PrintWindow with PW_RENDERFULLCONTENT, which captures DirectX swap-chain
# content that a plain screen grab of an occluded or background window misses.
# Falls back to a screen-region copy if PrintWindow returns an empty bitmap.
#
# Usage: powershell -File shot.ps1 -Out C:\path\shot.png

param([string]$Out = "$env:TEMP\yakuza_shot.png")

Add-Type -AssemblyName System.Drawing

$src = @'
using System;
using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  // Without these the process sees VIRTUALISED coordinates: on a 150% display
  // GetWindowRect reports 1280x720 for a 1920x1080 window, and CopyFromScreen
  // then copies physical pixels into a too-small bitmap - a cropped capture.
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
Add-Type -TypeDefinition $src -ErrorAction SilentlyContinue

# -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. Falls back to the older
# process-wide call on Windows versions that lack it.
try { [W]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null } catch { }
try { [W]::SetProcessDPIAware() | Out-Null } catch { }

$p = @(Get-Process -Name Yakuza4 -ErrorAction SilentlyContinue) | Where-Object { $_.MainWindowHandle -ne 0 }
if ($p.Count -eq 0) { Write-Output "ERR no Yakuza4 window"; exit 1 }
$h = $p[0].MainWindowHandle

if ([W]::IsIconic($h)) { [W]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 400 }

# Client rect mapped to screen coordinates: this is the rendered game image
# with no title bar or borders, and in physical pixels now that we are DPI-aware.
$cr = New-Object W+RECT
if (-not [W]::GetClientRect($h, [ref]$cr)) { Write-Output "ERR GetClientRect failed"; exit 1 }
$origin = New-Object W+POINT
$origin.X = 0; $origin.Y = 0
[W]::ClientToScreen($h, [ref]$origin) | Out-Null

$r = New-Object W+RECT
$r.L = $origin.X; $r.T = $origin.Y
$w = $cr.R - $cr.L; $ht = $cr.B - $cr.T
if ($w -le 0 -or $ht -le 0) {
  # Some windows report an empty client rect; fall back to the window rect.
  if (-not [W]::GetWindowRect($h, [ref]$r)) { Write-Output "ERR GetWindowRect failed"; exit 1 }
  $w = $r.R - $r.L; $ht = $r.B - $r.T
}
if ($w -le 0 -or $ht -le 0) { Write-Output "ERR bad window size"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
# 2 = PW_RENDERFULLCONTENT: required for DirectX-rendered client areas.
$ok = [W]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()

# PrintWindow can succeed but hand back an all-black bitmap for some swap
# chains; detect that and fall back to grabbing the screen region instead.
$blank = $true
for ($y = 0; $y -lt $ht -and $blank; $y += 40) {
  for ($x = 0; $x -lt $w; $x += 40) {
    $c = $bmp.GetPixel($x, $y)
    if ($c.R -ne 0 -or $c.G -ne 0 -or $c.B -ne 0) { $blank = $false; break }
  }
}

if (-not $ok -or $blank) {
  [W]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 500
  $bmp.Dispose()
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
  $g.Dispose()
  $method = "screen-copy"
} else {
  $method = "PrintWindow"
}

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "OK $Out ($w x $ht, $method)"
