@echo off
rem Builds build_m17_modem_pc\m17_rf_test.exe (w64devkit gcc, Codec 2 from %USERPROFILE%\tools\codec2).
setlocal
if not defined W64DEVKIT set "W64DEVKIT=%USERPROFILE%\tools\w64devkit"
set "PATH=%W64DEVKIT%\bin;%PATH%"
set "C2=%USERPROFILE%\tools\codec2"
cd /d "%~dp0..\.."
if not exist build_m17_modem_pc mkdir build_m17_modem_pc
gcc -O2 -Wall -Wextra -Wno-unused-parameter -D_USE_MATH_DEFINES ^
    -I m17_modem\core -I m17_pc -I "%C2%\src" -I "%C2%\build_w64" ^
    m17_modem\pc\m17_rf_test.c m17_modem\core\m17_rf.c m17_modem\core\m17_modem.c m17_pc\m17.c ^
    "%C2%\build_w64\src\libcodec2.a" -lm -static -o build_m17_modem_pc\m17_rf_test.exe || exit /b 1
echo Built build_m17_modem_pc\m17_rf_test.exe
