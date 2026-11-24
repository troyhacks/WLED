@echo off
REM ---------------------------------------------------------------------------
REM  flash_p4_rev3.bat - put the WLED-MM-P4 rev 3 firmware on an ESP32-P4
REM
REM  Usage:   flash_p4_rev3.bat [COMx]
REM           (COMx is the port the board shows; COM4 if you leave it out)
REM
REM  It needs esptool:   pip install esptool
REM  It needs the board in download mode: hold BOOT, press RESET, release BOOT.
REM
REM  The image is the whole flash: the bootloader, the partition table, the
REM  boot partition pointer and the firmware.  Your settings and your presets
REM  are on a partition of their own and are NOT touched.
REM ---------------------------------------------------------------------------

set PORT=%1
if "%PORT%"=="" set PORT=COM4

echo Flashing the WLED-MM-P4 rev 3 firmware to %PORT%
echo.

where esptool >nul 2>&1
if %ERRORLEVEL%==0 (
    esptool --chip esp32p4 --port %PORT% write_flash 0x0 firmware.factory.bin
) else (
    py -m esptool --chip esp32p4 --port %PORT% write_flash 0x0 firmware.factory.bin
)

if errorlevel 1 (
    echo.
    echo The flash did not go through.  Two things to try:
    echo   - the port: run "Device Manager" and look for the USB Serial Device
    echo   - download mode: hold BOOT, press RESET, release BOOT, and try again
    echo.
    pause
    exit /b 1
)

echo.
echo Done.  The board is restarting.
pause
