@echo off
rem Builds build_m17_pc\ (m17_pc.exe with a window, m17_cli.exe for scripts, m17_test.exe) with CMake and Ninja,
rem using w64devkit in %W64DEVKIT% (default %USERPROFILE%\tools\w64devkit) and Codec 2 in %USERPROFILE%\tools\codec2.
setlocal
if not defined W64DEVKIT set "W64DEVKIT=%USERPROFILE%\tools\w64devkit"
if not exist "%W64DEVKIT%\bin\gcc.exe" (
    echo w64devkit not found in %W64DEVKIT%
    exit /b 1
)
set "PATH=%W64DEVKIT%\bin;%PATH%"
set "CC=gcc"
cd /d "%~dp0.."
cmake -S m17_pc -B build_m17_pc -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build_m17_pc || exit /b 1
echo Built build_m17_pc\m17_cli.exe, m17_test.exe and m17_pc.exe
