@echo off
REM s-panel unified build entry point. Usage: build.bat [--clean]
setlocal
set "VSDEV=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEV%" (
  echo ERROR: VsDevCmd.bat not found. Install VS BuildTools 2022 + Desktop C++ + Win11 SDK.
  exit /b 1
)
call "%VSDEV%" -arch=x64 >nul
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
  set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
if "%1"=="--clean" rmdir /s /q build 2>nul
if not exist build mkdir build
"%CMAKE_EXE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CMAKE_EXE%" --build build
endlocal
