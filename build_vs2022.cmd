@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Use the GitHub Actions workflow, or install VS2022 C++ build tools.
  pause
  exit /b 1
)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0"
if errorlevel 1 goto failed
cmake --build build --config Release --parallel
if errorlevel 1 goto failed
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 goto failed
echo Built: "%~dp0build\Release\SmoothMotionHost.exe"
pause
exit /b 0
:failed
echo Build or test failed. The full output above contains the diagnostic.
pause
exit /b 1
