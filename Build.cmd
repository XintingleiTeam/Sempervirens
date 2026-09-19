@echo off
setlocal
cd /d "%~dp0"

set "ZIG=%~dp0.tools\zig-x86_64-windows-0.16.0\zig.exe"
if not exist "%ZIG%" for %%Z in (zig.exe) do set "ZIG=%%~$PATH:Z"
if not exist "%ZIG%" (
  echo Zig 0.16 was not found. Place it in .tools or add zig.exe to PATH.
  exit /b 1
)

if not exist build mkdir build
if not exist build\test mkdir build\test
if not exist build\release mkdir build\release
if not exist build\release\versions mkdir build\release\versions
if not exist build\release\versions\0.1.0.0 mkdir build\release\versions\0.1.0.0
set "ZIG_GLOBAL_CACHE_DIR=%~dp0.zig-cache\global"
set "ZIG_LOCAL_CACHE_DIR=%~dp0.zig-cache\local"
set "COMMON=src\text.cpp src\path_safety.cpp src\profile.cpp src\instance_discovery.cpp src\option_text.cpp src\scan.cpp src\nbt.cpp src\atomic_file.cpp src\execution.cpp src\presentation.cpp src\config_difference.cpp src\preferences.cpp build\miniz.o build\miniz_tdef.o build\miniz_tinfl.o"
set "UPDATE_INCLUDES=-Ithird_party\miniz"
for %%F in (miniz miniz_tdef miniz_tinfl) do (
  "%ZIG%" cc -w -std=c99 -O2 -target x86_64-windows-gnu -Ithird_party\miniz -c third_party\miniz\%%F.c -o build\%%F.o > build\compiler.log 2>&1
  if errorlevel 1 (
    type build\compiler.log
    exit /b 1
  )
)
"%ZIG%" c++ -w -std=c++20 -O2 -target x86_64-windows-gnu -DSEMPERVIRENS_TESTING -Ithird_party -Isrc tests\core_tests.cpp %COMMON% -o build\core-tests.exe > build\compiler.log 2>&1
if errorlevel 1 (
  type build\compiler.log
  exit /b 1
)
build\core-tests.exe
if errorlevel 1 exit /b 1

"%ZIG%" c++ -w -std=c++20 -O2 -target x86_64-windows-gnu -Ithird_party -Isrc %UPDATE_INCLUDES% tests\update_tests.cpp src\update.cpp src\text.cpp src\atomic_file.cpp build\miniz.o build\miniz_tdef.o build\miniz_tinfl.o -lbcrypt -lwinhttp -lshell32 -lole32 -o build\update-tests.exe > build\compiler.log 2>&1
if errorlevel 1 (type build\compiler.log & exit /b 1)
build\update-tests.exe
if errorlevel 1 exit /b 1

"%ZIG%" rc /c 65001 /:output-format coff /fo build\app-res.o /i . app.rc > build\compiler.log 2>&1
if errorlevel 1 (
  type build\compiler.log
  exit /b 1
)
"%ZIG%" c++ -w -std=c++20 -O2 -target x86_64-windows-gnu -municode -Ithird_party -Isrc src\main.cpp %COMMON% build\app-res.o -o build\Sempervirens-inspect.exe > build\compiler.log 2>&1
if errorlevel 1 (
  type build\compiler.log
  exit /b 1
)
"%ZIG%" c++ -w -std=c++20 -O2 -target x86_64-windows-gnu -municode -Ithird_party -Isrc tests\parity_migrate.cpp %COMMON% build\app-res.o -o build\parity-migrate.exe > build\compiler.log 2>&1
if errorlevel 1 (
  type build\compiler.log
  exit /b 1
)
pushd build
Sempervirens-inspect.exe --inspect .. > profile-fallback-test.log
if errorlevel 1 (
  type profile-fallback-test.log
  popd
  exit /b 1
)
popd
"%ZIG%" c++ -w -std=c++20 -O2 -target x86_64-windows-gnu -municode -mwindows -Wl,--subsystem,windows -Ithird_party -Isrc %UPDATE_INCLUDES% src\ui_app.cpp src\splash.cpp src\win_compat.cpp src\update.cpp %COMMON% build\app-res.o -ld2d1 -ldwrite -ldwmapi -lgdi32 -limm32 -lole32 -loleaut32 -loleacc -lshell32 -lwindowscodecs -luuid -lbcrypt -lwinhttp -o build\test\Sempervirens.exe > build\compiler.log 2>&1
if errorlevel 1 (
  type build\compiler.log
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File ".\Test-Native-Binary.ps1" > build\native-binary-test.log 2>&1
if errorlevel 1 (
  type build\native-binary-test.log
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File ".\Test-Ui-Smoke.ps1"
if errorlevel 1 exit /b 1

copy /y build\test\Sempervirens.exe build\release\versions\0.1.0.0\SempervirensApp.exe >nul
"%ZIG%" rc /c 65001 /:output-format coff /fo build\launcher-res.o /i . launcher.rc > build\compiler.log 2>&1
if errorlevel 1 (type build\compiler.log & exit /b 1)
"%ZIG%" rc /c 65001 /:output-format coff /fo build\updater-res.o /i . updater.rc > build\compiler.log 2>&1
if errorlevel 1 (type build\compiler.log & exit /b 1)
"%ZIG%" c++ -w -std=c++20 -O2 -target x86_64-windows-gnu -municode -mwindows -Wl,--subsystem,windows -Ithird_party -Isrc %UPDATE_INCLUDES% src\launcher.cpp src\update.cpp src\text.cpp src\atomic_file.cpp build\miniz.o build\miniz_tdef.o build\miniz_tinfl.o build\launcher-res.o -lbcrypt -lwinhttp -lshell32 -lole32 -o build\release\Sempervirens.exe > build\compiler.log 2>&1
if errorlevel 1 (type build\compiler.log & exit /b 1)
"%ZIG%" c++ -w -std=c++20 -O2 -target x86_64-windows-gnu -municode -mwindows -Wl,--subsystem,windows -Ithird_party -Isrc %UPDATE_INCLUDES% src\updater.cpp src\update.cpp src\text.cpp src\atomic_file.cpp build\miniz.o build\miniz_tdef.o build\miniz_tinfl.o build\updater-res.o -lbcrypt -lwinhttp -lshell32 -lole32 -o build\release\SempervirensUpdater.exe > build\compiler.log 2>&1
if errorlevel 1 (type build\compiler.log & exit /b 1)
copy /y installer\current.json build\release\current.json >nul
echo Sempervirens 0.1.0.0 release payload created under build\release\.
exit /b 0
