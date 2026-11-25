@echo off
REM ---------------------------------------------------------------------------
REM  flash_p4_rev3.bat - put the WLED-MM-P4 rev 3 firmware on an ESP32-P4
REM
REM  Usage:   flash_p4_rev3.bat [COMx]
REM           (COMx is the port the board shows; COM4 if you leave it out)
REM
REM  Needs esptool:   pip install esptool
REM
REM  Needs the board in download mode: hold BOOT, press RESET, release BOOT.
REM  (On the P4 the USB port alone will not do it - the chip has to be told to
REM  listen for the flasher.)
REM
REM  The image is the whole flash: the bootloader, the partition table, the
REM  OTA data partition and the application.  Your settings and your presets
REM  live on a partition of their own and are NOT touched.
REM
REM  This image is for the rev 3 chip ONLY.  It will not run on the older
REM  "ES pre rev.300" silicon, and that older firmware will not run on this
REM  one - the two need different images.
REM ---------------------------------------------------------------------------

set PORT=%1
if "%PORT%"=="" set PORT=COM4

REM Work in the folder the script is in, so the image is found whether the
REM script is run from here or double-clicked in Explorer.
cd /d "%~dp0"

if not exist firmware.factory.bin (
    echo firmware.factory.bin is not here.
    echo Put this script and the image files in the same folder and run it again.
    pause
    exit /b 1
)

REM Find esptool: as a command, then as a module of the Python launcher.
set "ESPCMD="
where esptool >nul 2>&1
if not errorlevel 1 set "ESPCMD=esptool"
if not defined ESPCMD (
    where py >nul 2>&1
    if not errorlevel 1 set "ESPCMD=py -m esptool"
)
if not defined ESPCMD (
    where python >nul 2>&1
    if not errorlevel 1 set "ESPCMD=python -m esptool"
)
if not defined ESPCMD (
    echo esptool was not found.
    echo Install it with:   pip install esptool
    echo and run this script again.
    pause
    exit /b 1
)

REM esptool 5 renamed "write_flash" to "write-flash"; esptool 4 only knows
REM the old name.  Ask the tool which one it has.
set "WRITE=write_flash"
%ESPCMD% --help 2>nul | find "write-flash" >nul
if not errorlevel 1 set "WRITE=write-flash"

echo Flashing the WLED-MM-P4 rev 3 firmware to %PORT%
echo.

%ESPCMD% --chip esp32p4 --port %PORT% %WRITE% 0x0 firmware.factory.bin

if errorlevel 1 (
    echo.
    echo The flash did not go through.  Three things to try:
    echo   - the port: run "Device Manager" and look for the USB Serial Device
    echo   - download mode: hold BOOT, press RESET, release BOOT, and try again
    echo   - esptool: run "pip install esptool"
    echo.
    pause
    exit /b 1
)

echo.
echo Done.  The board is restarting.
pause
