@echo off
REM Dry tests: no explorer contact. loader --check is read-only.
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

echo --- loader --check ---
"%BIN%\loader.exe" --check
if errorlevel 1 (
  echo FAILED: loader --check ^(errorlevel %errorlevel%^)
  goto :fail
)

echo ALL DRY TESTS PASS
endlocal & exit /b 0

:fail
echo ONE OR MORE TESTS FAILED
endlocal & exit /b 1
