@echo off
setlocal
cd /d "%~dp0tools"
python ota_packager_gui.py
if errorlevel 1 (
    echo.
    echo OTA packager failed to start. Install Python 3 with Tkinter, then try again.
    pause
)
