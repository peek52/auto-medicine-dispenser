@echo off
echo ==============================================
echo Building Unified Cam Project for ESP32-P4
echo ==============================================

:: Path to the ESP-IDF environment (adjusted based on previous webcam_app settings)
set "IDF_PATH=C:\Users\peekz\esp\esp-idf"
call %IDF_PATH%\export.bat

cd /d "D:\project\ddddddddd\unified_cam"

:: Set target to ESP32-P4
idf.py set-target esp32p4

:: Build the project
idf.py build

echo ==============================================
echo Build finished! Run flash.bat to upload
echo ==============================================
