@echo off
REM s-panel env check: read-only, no changes
setlocal
echo [1] cl in PATH:
where cl 2>&1
echo.
echo [2] VsDevCmd:
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" echo FOUND VsDevCmd.bat
echo.
echo [3] MSVC versions:
dir /b "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" 2>&1
echo.
echo [4] Windows SDKs:
dir /b "C:\Program Files (x86)\Windows Kits\10\Include" 2>&1
echo.
echo [5] bundled cmake:
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" echo FOUND bundled cmake.exe
echo.
echo [6] git:
git --version 2>&1
echo.
echo [7] OS:
cmd /c ver 2>&1
wmic os get Caption,BuildNumber /value 2>&1 | findstr /v "^$"
endlocal
