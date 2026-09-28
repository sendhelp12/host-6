@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "SMH_EXE=%~dp0SmoothMotionHost.exe"
if not exist "%SMH_EXE%" set "SMH_EXE=%~dp0build\Release\SmoothMotionHost.exe"
if not exist "%SMH_EXE%" (
  echo SmoothMotionHost.exe is missing. Download the GitHub Actions executable artifact first.
  pause
  exit /b 1
)
"%SMH_EXE%" %*
set "SMH_RESULT=%ERRORLEVEL%"
pause
exit /b %SMH_RESULT%
