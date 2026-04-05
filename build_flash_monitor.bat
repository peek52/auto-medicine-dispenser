@echo off
echo ==============================================
echo Build, Flash, and Monitor - Unified Cam
echo ==============================================

set "IDF_PATH=C:\Users\peekz\esp\esp-idf"
call %IDF_PATH%\export.bat

cd /d "D:\project\ddddddddd\unified_cam"

idf.py -p COM6 build flash monitor
