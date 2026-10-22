@echo off
REM Dry tests: no explorer contact. loader --check is read-only.
REM Live test (--live) additionally needs taskbar.dll loaded in explorer.
setlocal
set "BIN=%~dp0..\build\bin"

echo --- test_config ---
"%BIN%\test_config.exe"
if errorlevel 1 (
  echo FAILED: test_config ^(errorlevel %errorlevel%^)
  goto :fail
)

echo --- test_host ---
"%BIN%\test_host.exe" "%BIN%\taskbar.dll"
if errorlevel 1 (
  echo FAILED: test_host ^(errorlevel %errorlevel%^)
  goto :fail
)

echo --- test_resolve ---
"%BIN%\test_resolve.exe"
if errorlevel 1 (
  echo FAILED: test_resolve ^(errorlevel %errorlevel%^)
  goto :fail
)

echo --- test_hooktargets ---
"%BIN%\test_hooktargets.exe"
if errorlevel 1 (
  echo FAILED: test_hooktargets ^(errorlevel %errorlevel%^)
  goto :fail
)

echo --- loader --check ---
"%BIN%\loader.exe" --check
if errorlevel 1 (
  echo FAILED: loader --check ^(errorlevel %errorlevel%^)
  goto :fail
)

echo ALL DRY TESTS PASS

REM Live test: change tray cell width, measure icon spacing on screenshots.
REM Requires taskbar.dll loaded in explorer (loader.exe --inject).
if /I "%~1"=="--live" (
  echo --- auto_test ^(live^) ---
  powershell -ExecutionPolicy Bypass -File "%~dp0..\tools\auto_test.ps1"
  if errorlevel 1 (
    echo FAILED: auto_test ^(errorlevel %errorlevel%^)
    goto :fail
  )
)

endlocal & exit /b 0

:fail
echo ONE OR MORE TESTS FAILED
endlocal & exit /b 1
