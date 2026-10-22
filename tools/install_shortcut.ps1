# install_shortcut.ps1 — ярлык «s-panel» на рабочем столе для запуска двойным кликом.
#
#   powershell -ExecutionPolicy Bypass -File tools\install_shortcut.ps1
#
# Ярлык запускает установленную копию (loader.exe --inject-and-tray --show-popup)
# без окна консоли: поднимает иконку в трее и сразу открывает панель настроек.
param(
  [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 's-panel'),
  [switch]$NoInstall
)

$ErrorActionPreference = 'Stop'

if (-not $NoInstall) {
  # Разложить loader.exe/taskbar.dll в %LOCALAPPDATA%\s-panel.
  $repo = Split-Path -Parent $PSScriptRoot
  $src = Join-Path $repo 'build\bin'
  if (-not (Test-Path (Join-Path $src 'loader.exe'))) {
    throw "loader.exe not found in $src - build first (build.bat)"
  }
  New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
  Copy-Item (Join-Path $src 'loader.exe') (Join-Path $InstallDir 'loader.exe') -Force
  Copy-Item (Join-Path $src 'taskbar.dll') (Join-Path $InstallDir 'taskbar.dll') -Force
  if (-not (Test-Path (Join-Path $InstallDir 'config.ini'))) {
    Copy-Item (Join-Path $src 'config.ini') (Join-Path $InstallDir 'config.ini') -Force
  }
}

$vbs = Join-Path $InstallDir 'start-s-panel.vbs'
if (-not (Test-Path $vbs)) {
  @'
' start-s-panel.vbs - запуск s-panel по двойному клику (без окна консоли).
' Внедряет taskbar.dll в explorer.exe, поднимает иконку с настройками в трее
' и сразу открывает панель настроек. Если s-panel уже запущен, повторный запуск
' только покажет панель (второй иконки не будет).
Option Explicit

Dim fso, shell, dir, exe, cmd

Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

dir = fso.GetParentFolderName(WScript.ScriptFullName)
exe = dir & "\loader.exe"

If Not fso.FileExists(exe) Then
  MsgBox "loader.exe not found next to this script:" & vbCrLf & dir, 16, "s-panel"
  WScript.Quit 1
End If

cmd = """" & exe & """ --inject-and-tray --show-popup"
' 0 = скрыто, False = не ждать завершения
shell.Run cmd, 0, False
'@ | Set-Content -LiteralPath $vbs -Encoding ASCII
}

$desktop = [Environment]::GetFolderPath('Desktop')
$lnk = Join-Path $desktop 's-panel.lnk'
$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = Join-Path $env:SystemRoot 'System32\wscript.exe'
$s.Arguments = "`"$vbs`""
$s.WorkingDirectory = $InstallDir
$s.IconLocation = (Join-Path $InstallDir 'loader.exe') + ',0'
$s.Description = 's-panel: панель настроек панели задач'
$s.WindowStyle = 7
$s.Save()

Write-Host "shortcut: $lnk"
Write-Host "launcher:  $vbs"
Write-Host "install:   $InstallDir"
