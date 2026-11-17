@echo off
rem Builds build_taikou\taikou-test1-1.exe (w64devkit gcc). Uses m17_modem\core and m17_pc\fm_path.c, m17_pc\m17.c.
setlocal
if not defined W64DEVKIT set "W64DEVKIT=%USERPROFILE%\tools\w64devkit"
set "PATH=%W64DEVKIT%\bin;%PATH%"
cd /d "%~dp0.."
if not exist build_taikou mkdir build_taikou
gcc -O2 -Wall -Wextra -Wno-unused-parameter -D_USE_MATH_DEFINES ^
    -I m17_modem\core -I m17_pc ^
    taikou-test1-1\taikou.c m17_modem\core\m17_rf.c m17_modem\core\m17_modem.c m17_pc\m17.c m17_pc\fm_path.c ^
    -lwinmm -lm -static -o build_taikou\taikou-test1-1.exe || exit /b 1
echo Built build_taikou\taikou-test1-1.exe
