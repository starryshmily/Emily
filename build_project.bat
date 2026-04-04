@echo off
call D:\Espressif\frameworks\esp-idf-v5.1.2\export.bat
echo IDF_PATH=%IDF_PATH%
echo PATH=%PATH%
where idf.py 2>&1
cd /d D:\Emily\lvgl_demo
idf.py reconfigure
