@echo off
rem ESP-IDF 5.1.6 environment for this project (works without changing the PowerShell execution policy).
rem Usage:  idf_env.bat build
rem         idf_env.bat -p COM10 flash
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.1_py3.11_env
call C:\Users\user\esp516\v5.1.6\esp-idf\export.bat >nul || exit /b 1
cd /d "%~dp0"
idf.py %*
