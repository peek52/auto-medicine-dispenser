@echo off
REM One-shot recovery: apply IDF NULL-guard patch + rebuild + flash
REM Run from File Explorer (double-click) or any cmd window.
REM Requires: ESP-IDF v5.3.x at C:\Users\peekz\esp\esp-idf, COM6 free.

setlocal enabledelayedexpansion
set IDF_PATH=C:\Users\peekz\esp\esp-idf
set PROJECT=D:\project\ddddddddd\unified_cam
set PATCH=%PROJECT%\i2c_master_null_guard.patch

echo.
echo ================================================
echo  Step 1/4 : Apply NULL-guard patch to ESP-IDF
echo ================================================
pushd "%IDF_PATH%"
git apply --check "%PATCH%" 2>nul
if errorlevel 1 (
    echo.
    echo Patch already applied or conflict — checking...
    git diff --stat components/esp_driver_i2c/i2c_master.c
    echo.
) else (
    git apply "%PATCH%"
    if errorlevel 1 (
        echo ERROR: failed to apply patch
        popd
        pause
        exit /b 1
    )
    echo Patch applied OK.
)
popd

echo.
echo ================================================
echo  Step 2/4 : Activate ESP-IDF environment
echo ================================================
call "%IDF_PATH%\export.bat"
if errorlevel 1 (
    echo ERROR: idf export failed
    pause
    exit /b 1
)

echo.
echo ================================================
echo  Step 3/4 : Build firmware (clean rebuild)
echo ================================================
cd /d "%PROJECT%"
call idf.py fullclean
call idf.py build
if errorlevel 1 (
    echo ERROR: build failed
    pause
    exit /b 1
)

echo.
echo ================================================
echo  Step 4/4 : Flash to COM6
echo ================================================
call idf.py -p COM6 flash
if errorlevel 1 (
    echo ERROR: flash failed — make sure COM6 is free (no monitor open)
    pause
    exit /b 1
)

echo.
echo ================================================
echo  DONE — board should boot in full mode now.
echo  WiFi/camera/Telegram/VL53 should all work.
echo ================================================
echo.
pause
