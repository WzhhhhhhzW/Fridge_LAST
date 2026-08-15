@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul
title RA6M5 One-Click USB Flash

set "SCRIPT_DIR=%~dp0"
set "ELF_FILE=%SCRIPT_DIR%project_firdge\Debug\project_firdge.elf"
set "PACKAGER=%SCRIPT_DIR%tools\prepare_local_flash.py"
set "SIGNED_FILE=%SCRIPT_DIR%project_firdge\Debug\project_firdge.local.confirmed.bin"
set "PYOCD_CONFIG=%SCRIPT_DIR%project_firdge\Debug\100ASK_PYOCD\pyocd.yaml"
set "PYOCD_TARGET=R7FA6M5BF"
set "APP_BASE_ADDRESS=0x00020000"
set "PYTHON_EXE="
set "USE_PY_LAUNCHER="

echo ============================================================
echo   RA6M5 One-Click USB Flash
echo ============================================================
echo.

if not exist "%ELF_FILE%" (
    echo [ERROR] Firmware ELF was not found:
    echo         %ELF_FILE%
    echo.
    echo Build project_firdge in e2 studio first, then run this file again.
    goto :failed
)

if not exist "%PYOCD_CONFIG%" (
    echo [ERROR] pyOCD configuration was not found:
    echo         %PYOCD_CONFIG%
    goto :failed
)

for /f "delims=" %%P in ('where python 2^>nul') do (
    if not defined PYTHON_EXE (
        "%%P" -c "import pyocd" >nul 2>&1
        if !errorlevel! equ 0 set "PYTHON_EXE=%%P"
    )
)

if not defined PYTHON_EXE (
    where py >nul 2>&1
    if not errorlevel 1 (
        py -3 -c "import pyocd" >nul 2>&1
        if not errorlevel 1 set "USE_PY_LAUNCHER=1"
    )
)

if not defined PYTHON_EXE if not defined USE_PY_LAUNCHER (
    echo [ERROR] Python with pyOCD was not found.
    echo.
    echo Install pyOCD with:
    echo     python -m pip install pyocd
    goto :failed
)

if not exist "%PACKAGER%" (
    echo [ERROR] Local MCUboot packager was not found:
    echo         %PACKAGER%
    goto :failed
)

echo [STEP 1/2] Create and verify the signed MCUboot confirmed image...
echo.
if defined PYTHON_EXE (
    "!PYTHON_EXE!" "%PACKAGER%"
) else (
    py -3 "%PACKAGER%"
)
if errorlevel 1 (
    echo.
    echo [ERROR] Packaging stopped. Nothing was written to the RA6M5.
    echo Build project_firdge in e2 studio and fix the reported item first.
    goto :failed
)

if not exist "%SIGNED_FILE%" (
    echo [ERROR] The verified local flash image was not created:
    echo         %SIGNED_FILE%
    goto :failed
)

if defined PYTHON_EXE (
    "!PYTHON_EXE!" -c "import pathlib,sys; p=pathlib.Path(sys.argv[1]); d=p.read_bytes(); sys.exit(0 if len(d) == 0x60000 and d[:4] == bytes.fromhex('3db8f396') else 1)" "%SIGNED_FILE%"
) else (
    py -3 -c "import pathlib,sys; p=pathlib.Path(sys.argv[1]); d=p.read_bytes(); sys.exit(0 if len(d) == 0x60000 and d[:4] == bytes.fromhex('3db8f396') else 1)" "%SIGNED_FILE%"
)
if errorlevel 1 (
    echo [ERROR] Firmware is not a valid signed MCUboot application image.
    echo Expected: 0x60000 bytes with MCUboot magic 3D B8 F3 96.
    echo Refusing to flash an unsigned or incorrectly packaged image.
    goto :failed
)

for %%F in ("%SIGNED_FILE%") do (
    echo [INFO] Signed image : %%~fF
    echo [INFO] Size     : %%~zF bytes
    echo [INFO] Modified : %%~tF
)
echo [INFO] App base     : %APP_BASE_ADDRESS%
echo [INFO] Target       : %PYOCD_TARGET%
echo.

if /I "%~1"=="--check" (
    echo [OK] Self-check passed. The signed MCUboot image and flash settings are ready.
    exit /b 0
)

echo Connect the RA6M5 board by USB and close any active debug session.
echo [STEP 2/2] Flash the signed application into the MCUboot lower slot.
echo Flashing now...
echo.

if defined PYTHON_EXE (
    "!PYTHON_EXE!" -m pyocd flash "%SIGNED_FILE%" -a %APP_BASE_ADDRESS% --config "%PYOCD_CONFIG%" --target %PYOCD_TARGET%
) else (
    py -3 -m pyocd flash "%SIGNED_FILE%" -a %APP_BASE_ADDRESS% --config "%PYOCD_CONFIG%" --target %PYOCD_TARGET%
)
set "FLASH_RESULT=!ERRORLEVEL!"

echo.
if not "%FLASH_RESULT%"=="0" (
    echo [ERROR] Flash failed. pyOCD exit code: %FLASH_RESULT%
    echo Check the USB cable, board power, driver, and whether another program is using the probe.
    goto :failed
)

echo ============================================================
echo [OK] Firmware flashed successfully. The board should restart.
echo ============================================================
echo.
pause
exit /b 0

:failed
echo.
echo No FSP configuration or generated ra_gen file was changed.
echo.
if /I "%~1"=="--check" exit /b 1
pause
exit /b 1
