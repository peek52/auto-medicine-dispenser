@echo off
echo ==============================================
echo Monitoring Unified Cam Project ESP32-P4 (COM6)
echo ==============================================

set "IDF_PATH=C:\Users\peekz\esp\esp-idf"
call %IDF_PATH%\export.bat

cd /d "D:\project\ddddddddd\unified_cam"

idf.py -p COM6 monitor
