@echo off
setlocal EnableExtensions DisableDelayedExpansion
if not defined SM86_EXE set "SM86_EXE=%LOCALAPPDATA%\Programs\SmoothMotionSM86\sm86.exe"
set "SMH_EXE=%~dp0SmoothMotionHost.exe"
if not exist "%SMH_EXE%" set "SMH_EXE=%~dp0build\Release\SmoothMotionHost.exe"
if not exist "%SM86_EXE%" (
  echo Could not find SM86: "%SM86_EXE%"
  echo Set SM86_EXE to the full path of your installed sm86.exe, then run this script again.
  pause
  exit /b 1
)
if not exist "%SMH_EXE%" (
  echo SmoothMotionHost.exe is missing. Download the GitHub Actions executable artifact first.
  pause
  exit /b 1
)
rem The final dot avoids passing a quoted directory ending in a backslash to the launcher.
for %%I in ("%SMH_EXE%") do set "SMH_DIR=%%~dpI."
"%SM86_EXE%" launch --exe "%SMH_EXE%" --game-root "%SMH_DIR%" --cwd "%SMH_DIR%"
set "SMH_RESULT=%ERRORLEVEL%"
pause
exit /b %SMH_RESULT%
