@echo off
setlocal EnableDelayedExpansion
REM ============================================================================
REM  flash_p4_rev3.bat - put the WLED-MM-P4 rev 3 firmware on an ESP32-P4
REM
REM  Usage:   flash_p4_rev3.bat [COMx]
REM
REM  Everything this script needs is in this folder: esptool.exe and the
REM  firmware images.  There is nothing to install.
REM
REM  Before the first run, the board has to be in DOWNLOAD MODE:
REM     hold BOOT, press RESET, release BOOT
REM  The board says "WLED-P4v3" on the network once it is up.
REM
REM  This firmware is for the rev 3 chip ONLY (the chip reports "revision
REM  v3.2").  It will not run on the older P4, and the older firmware will
REM  not run on it.
REM ============================================================================

cd /d "%~dp0"

set "PORT=%~1"
set "FACTORY=firmware.factory.bin"
set "CMD="

if not exist "%FACTORY%" (
    echo.
    echo %FACTORY% is missing from this folder.
    pause
    exit /b 1
)

REM ---- the flasher ----------------------------------------------------------
REM  The bundled esptool.exe is the Espressif build for Windows; it needs
REM  nothing else.  If it is gone, fall back to an esptool on this machine.
if exist "%~dp0esptool.exe" (
    set "CMD=""%~dp0esptool.exe"""
) else (
    where esptool >nul 2>&1
    if not errorlevel 1 set "CMD=esptool"
    if not defined CMD (
        where python >nul 2>&1
        if not errorlevel 1 set "CMD=python -m esptool"
    )
)
if not defined CMD (
    echo.
    echo There is no esptool: esptool.exe is missing from this folder, and
    echo this machine has no esptool either.
    echo Put esptool.exe back next to this script and run it again.
    pause
    exit /b 1
)

REM ---- which port? ----------------------------------------------------------
if not "%PORT%"=="" goto haveport

set "PORTFILE=%TEMP%\wled_p4_ports.txt"
powershell -NoProfile -Command "Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -match 'VID_303A' } | ForEach-Object { $_.DeviceID }" > "%PORTFILE%" 2>nul
set "N=0"
set "PORTLIST="
for /f "usebackq delims=" %%P in ("%PORTFILE%") do (
    set /a N+=1
    set "PORTLIST=!PORTLIST!%%P "
    if !N! == 1 set "PORT=%%P"
)
if !N! == 0 (
    echo.
    echo No Espressif board found on this machine.
    echo.
    echo   - the USB cable has to be a data cable, and it has to be in the
    echo     USB port that the USB-Serial/JTAG is on
    echo   - check "Device Manager" for a "USB Serial Device (COMx)"
    echo   - if the board is not listed at all, install the USB driver
    echo.
    pause
    exit /b 1
)
if !N! GTR 1 (
    echo.
    echo There are !N! Espressif boards on this machine.  Run this script again
    echo with the port of the rev 3 board:
    echo.
    for %%P in (!PORTLIST!) do echo     flash_p4_rev3.bat %%P
    echo.
    echo Pointing it at the wrong board is safe: the script reads the chip
    echo revision and stops if it is not a rev 3.
    echo.
    pause
    exit /b 1
)
echo Using port %PORT%
goto haveport

:haveport
REM ---- what is on that port? -------------------------------------------------
REM  The rev 3 image does not boot on the older chip, so check first.
set "IDOUT=%TEMP%\wled_p4_chip.txt"
%CMD% --chip esp32p4 --port %PORT% chip-id > "%IDOUT%" 2>&1
if errorlevel 1 %CMD% --chip esp32p4 --port %PORT% chip_id > "%IDOUT%" 2>&1

findstr /C:"revision v1" /C:"revision v2" "%IDOUT%" >nul
if not errorlevel 1 (
    echo.
    echo The board on %PORT% is NOT a rev 3 chip:
    findstr /C:"Chip type" "%IDOUT%"
    echo.
    echo The rev 3 firmware will not boot on it, so this script stops here.
    echo Find the port of the rev 3 board and run this again with that port.
    pause
    exit /b 1
)

findstr /C:"revision" "%IDOUT%" >nul
if errorlevel 1 (
    echo.
    echo The board on %PORT% did not answer.
    echo.
    echo   - put it in download mode: hold BOOT, press RESET, release BOOT
    echo   - check the cable: it has to be a data cable
    echo   - check "Device Manager" for a "USB Serial Device (COMx)"
    echo.
    echo If the board is already running WLED, it can also be flashed from
    echo the web interface: "Settings" - "Update" - choose firmware.factory.bin
    echo.
    pause
    exit /b 1
)
findstr /C:"Chip type" "%IDOUT%"

REM ---- the flash ------------------------------------------------------------
echo.
echo Flashing the WLED-MM-P4 rev 3 firmware to %PORT% ...
echo.

%CMD% --chip esp32p4 --port %PORT% write-flash 0x0 "%FACTORY%"

if errorlevel 1 (
    echo.
    echo The write did not go through.  Trying once more without esptool's
    echo stub flasher - it is slower, but it does not depend on the chip's
    echo RAM map ...
    echo.
    %CMD% --no-stub --chip esp32p4 --port %PORT% write-flash 0x0 "%FACTORY%"
)

if errorlevel 1 (
    echo.
    echo The flash did not go through.  Three things to try:
    echo   - the port: run "Device Manager" and look for "USB Serial Device"
    echo   - download mode: hold BOOT, press RESET, release BOOT, and try again
    echo   - the cable: a charge-only cable cannot carry data
    echo.
    pause
    exit /b 1
)

echo.
echo Done.  The board restarts and is on the network in about 20 seconds.
echo.
echo   - watch the USB console to see the address it takes
echo   - the web interface is at http://that-address/
echo   - the first start formats the settings partition, so the board comes
echo     up with the factory settings
echo.
pause
