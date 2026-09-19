@echo off
setlocal
cd /d "%~dp0"

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo Inno Setup 6 was not found.
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File ".\tools\Generate-Icon.ps1"
if errorlevel 1 exit /b 1

call Build.cmd
if errorlevel 1 exit /b 1

"%ISCC%" "installer\Sempervirens.iss"
if errorlevel 1 exit /b 1

if not exist "artifacts\release\Sempervirens-0.1.0.0-Setup.exe" (
  echo The release installer was not created.
  exit /b 1
)
echo Sempervirens 0.1.0.0 installer created under artifacts\release\.
exit /b 0
