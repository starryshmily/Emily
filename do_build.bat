@echo off
call D:\Espressif\frameworks\esp-idf-v5.1.2\export.bat >nul 2>&1
cd /d D:\Emily\lvgl_demo
idf.py build > build_log.txt 2>&1
echo BUILD_EXIT_CODE=%ERRORLEVEL% >> build_log.txt
