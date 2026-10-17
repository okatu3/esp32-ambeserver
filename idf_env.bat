@echo off
rem ESP-IDF 5.1.6 environment for this project (works without changing the PowerShell execution policy).
rem Usage:  idf_env.bat build
rem         idf_env.bat -p COM10 flash
rem Set IDF_516 (ESP-IDF folder) and IDF_PY_ENV (its Python environment) to use other locations.
if not defined IDF_516 set "IDF_516=C:\Users\user\esp516\v5.1.6\esp-idf"
if not defined IDF_PY_ENV set "IDF_PY_ENV=C:\Espressif\python_env\idf5.1_py3.11_env"
set "IDF_PYTHON_ENV_PATH=%IDF_PY_ENV%"
call "%IDF_516%\export.bat" >nul || exit /b 1
cd /d "%~dp0"
idf.py %*
