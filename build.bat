@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "CONFIG=Release"
set "RUN_TESTS=OFF"
:args
if "%~1"=="" goto build
if /I "%~1"=="Debug" (
  set "CONFIG=Debug"
) else if /I "%~1"=="Release" (
  set "CONFIG=Release"
) else if /I "%~1"=="--tests" (
  set "RUN_TESTS=ON"
) else if /I not "%~1"=="--frozen" (
  echo Usage: build.bat [Release^|Debug] [--tests]
  exit /b 1
)
shift
goto args
:build
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: Install CMake 4.2+ and Visual Studio C++ tools with a Windows SDK.
  exit /b 1
)
cmake -S . -B build -A Win32 -DBUILD_TESTING=%RUN_TESTS%
if errorlevel 1 exit /b 1
if "%RUN_TESTS%"=="ON" (
  cmake --build build --config %CONFIG% --parallel
  if errorlevel 1 exit /b 1
  ctest --test-dir build -C %CONFIG% --output-on-failure
  if errorlevel 1 exit /b 1
) else (
  cmake --build build --config %CONFIG% --target SanFrameOptimizer --parallel
  if errorlevel 1 exit /b 1
)
echo SUCCESS: build\%CONFIG%\SanFrameOptimizer.asi and SanFrameOptimizer.ini
exit /b 0
