@echo off
echo ==============================================
echo Flashing Unified Cam Project to ESP32-P4
echo ==============================================

set "IDF_PATH=C:\Users\peekz\esp\esp-idf"
call %IDF_PATH%\export.bat

cd /d "D:\project\ddddddddd\unified_cam"

set "BAUD=921600"
set "PORT=COM6"

idf.py -p %PORT% -b %BAUD% flash
echo ==============================================
echo Flashing complete!
echo ==============================================
