# auto_test.ps1 — живой автотест: меняет ширину ячеек иконок трея и проверяет
# результат по скриншоту (расстояние между иконками должно вырасти).
#
# Требует загруженной DLL в explorer (loader.exe --inject) и работающего
# рабочего стола. Конфиг по умолчанию восстанавливается в конце.
#
# Использование:
#   powershell -ExecutionPolicy Bypass -File tools\auto_test.ps1
#   powershell ... -Config C:\path\config.ini -BaseWidth 28 -TestWidth 40
param(
  [string]$Config = "",
  [string]$BinDir = "",
  [int]$BaseWidth = 28,
  [int]$TestWidth = 40,
  [int]$SettleMs = 2500,
  [int]$MinDelta = 12
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$repo = Split-Path -Parent $PSScriptRoot
if (-not $BinDir) { $BinDir = Join-Path $repo 'build\bin' }
if (-not $Config) { $Config = Join-Path $BinDir 'config.ini' }
$loader = Join-Path $BinDir 'loader.exe'

function Fail([string]$msg) { throw $msg }

# --- геометрия панели задач ---------------------------------------------
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@

$hwnd = [TB]::FindWindowW('Shell_TrayWnd', $null)
if ($hwnd -eq [IntPtr]::Zero) { Fail 'Shell_TrayWnd not found' }
$rect = New-Object TB+RECT
[void][TB]::GetWindowRect($hwnd, [ref]$rect)
$taskbarH = $rect.B - $rect.T
if ($taskbarH -le 0) { Fail "bad taskbar rect $($rect.L),$($rect.T),$($rect.R),$($rect.B)" }

# Область уведомлений: правая часть панели, без часов (последние ~90 px).
$captureX = $rect.R - 430
$captureW = 340
if ($captureX -lt $rect.L) { $captureX = $rect.L; $captureW = $rect.R - $rect.L }

function Save-TrayShot([string]$path) {
  $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $full = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
  $g = [System.Drawing.Graphics]::FromImage($full)
  $g.CopyFromScreen($vs.Left, $vs.Top, 0, 0, $full.Size)
  $g.Dispose()
  $crop = $full.Clone((New-Object System.Drawing.Rectangle($captureX, $rect.T, $captureW, $taskbarH)), $full.PixelFormat)
  $crop.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $crop.Dispose()
  $full.Dispose()
}

# Расстояние между первой и последней иконкой в области уведомлений (px).
function Get-IconSpan([string]$path) {
  $bmp = New-Object System.Drawing.Bitmap($path)
  $bg = $bmp.GetPixel(1, 1)
  $cols = @()
  $y0 = [int]($bmp.Height * 0.25)
  $y1 = [int]($bmp.Height * 0.85)
  for ($x = 0; $x -lt $bmp.Width; $x++) {
    $hit = $false
    for ($y = $y0; $y -lt $y1; $y += 2) {
      $p = $bmp.GetPixel($x, $y)
      $d = [Math]::Abs($p.R - $bg.R) + [Math]::Abs($p.G - $bg.G) + [Math]::Abs($p.B - $bg.B)
      if ($d -gt 60) { $hit = $true; break }
    }
    $cols += $hit
  }
  $bmp.Dispose()

  # Склейка колонок в группы (иконки), допуск 2 px.
  $groups = @()
  $start = -1; $gap = 0
  for ($x = 0; $x -lt $cols.Count; $x++) {
    if ($cols[$x]) {
      if ($start -lt 0) { $start = $x }
      $gap = 0
    } elseif ($start -ge 0) {
      $gap++
      if ($gap -gt 2) {
        $groups += ,@($start, ($x - $gap))
        $start = -1; $gap = 0
      }
    }
  }
  if ($start -ge 0) { $groups += ,@($start, ($cols.Count - 1)) }
  # Отбрасываем группы уже 3 px — это шум/границы.
  $groups = @($groups | Where-Object { ($_[1] - $_[0]) -ge 3 })
  if ($groups.Count -lt 2) { return -1 }
  $first = ($groups[0][0] + $groups[0][1]) / 2
  $last = ($groups[-1][0] + $groups[-1][1]) / 2
  return [int]($last - $first)
}

function Set-TrayWidth([int]$value) {
  $text = Get-Content -LiteralPath $Config -Raw
  if ($text -match '(?m)^TrayIconSize=\d+') {
    $text = [regex]::Replace($text, '(?m)^TrayIconSize=\d+', "TrayIconSize=$value")
  } else {
    $text = $text.TrimEnd() + "`r`nTrayIconSize=$value`r`n"
  }
  [System.IO.File]::WriteAllText($Config, $text, (New-Object System.Text.ASCIIEncoding))
}

# --- проверки ------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Config)) { Fail "config not found: $Config" }
if (-not (Test-Path -LiteralPath $loader)) { Fail "loader not found: $loader" }

$check = & $loader --check 2>&1 | Out-String
if ($check -notmatch 'loaded in explorer:\s*yes') {
  Fail "DLL is not loaded in explorer (run loader.exe --inject first):`n$check"
}

$backup = Get-Content -LiteralPath $Config -Raw
$tmpDir = Join-Path $env:TEMP "s-panel-autotest"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$shotA = Join-Path $tmpDir 'tray_base.png'
$shotB = Join-Path $tmpDir 'tray_test.png'

try {
  Write-Host "taskbar rect: $($rect.L),$($rect.T) .. $($rect.R),$($rect.B)  capture x=$captureX w=$captureW"

  Set-TrayWidth $BaseWidth
  Start-Sleep -Milliseconds $SettleMs
  Save-TrayShot $shotA
  $spanA = Get-IconSpan $shotA
  Write-Host "base width=$BaseWidth span=${spanA}px -> $shotA"

  Set-TrayWidth $TestWidth
  Start-Sleep -Milliseconds $SettleMs
  Save-TrayShot $shotB
  $spanB = Get-IconSpan $shotB
  Write-Host "test width=$TestWidth span=${spanB}px -> $shotB"

  if ($spanA -lt 0 -or $spanB -lt 0) {
    Fail "could not detect tray icons (span base=$spanA test=$spanB) — is the notification area visible?"
  }
  $delta = $spanB - $spanA
  if ($delta -lt $MinDelta) {
    Fail "tray icons did not spread: span $spanA -> $spanB (delta=$delta, expected >= $MinDelta)"
  }
  Write-Host "PASS: tray icons spread by ${delta}px ($spanA -> $spanB)"
} finally {
  [System.IO.File]::WriteAllText($Config, $backup, (New-Object System.Text.ASCIIEncoding))
  Write-Host "config restored"
}
