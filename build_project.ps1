$env:IDF_PATH = "D:\Espressif\frameworks\esp-idf-v5.1.2"
& $env:IDF_PATH\export.ps1
Set-Location D:\Emily\lvgl_demo
idf.py reconfigure
idf.py build
