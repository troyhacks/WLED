# WLED-MM-P4 firmware for the ESP32-P4 rev 3

This is the WLED-MM-P4 firmware built for the **ESP32-P4 rev 3** (and later) -
the chip whose SRAM map differs from the older revision, and which ESP-IDF's
support for the two does not allow to be mixed.

## What you need

* An ESP32-P4 rev 3 board (the one with the "ES pre rev.300" chip is a
  different build - it will not run this firmware, and this firmware will
  not run on it).
* [esptool](https://pypi.org/project/esptool/): `pip install esptool`
* The board in **download mode**: hold **BOOT**, press **RESET**, release
  **BOOT**.  (The board's own USB port is the one to use.)

## Flashing

Put the files in one folder and run:

```
flash_p4_rev3.bat COM4
```

(`COM4` is the port the board shows in Device Manager - the number will
differ from machine to machine.)

Or, by hand, the one command that does it:

```
esptool --chip esp32p4 --port COM4 write_flash 0x0 firmware.factory.bin
```

(`py -m esptool ...` if esptool is not on the PATH.)

## What goes where

`firmware.factory.bin` is the whole flash in one file.  It holds:

| offset | file |
| --- | --- |
| 0x2000 | `bootloader.bin` |
| 0x8000 | `partitions.bin` |
| 0xe000 | `ota_data_initial.bin` |
| 0x10000 | `firmware.bin` (the application) |

The four files are in the bundle as well, if you would rather flash them
one at a time - the P4's ROM looks for its bootloader at **0x2000**, so do
not flash `bootloader.bin` at 0x0:

```
esptool --chip esp32p4 --port COM4 write_flash 0x2000 bootloader.bin 0x8000 partitions.bin 0xe000 ota_data_initial.bin 0x10000 firmware.bin
```

## What is not touched

The settings, the presets and the palettes live on a partition of their own
(the filesystem partition), and none of the files above write to it.  So
your own settings survive the flash.  If you want to start from scratch
anyway, format the filesystem from the web interface afterwards.

## Afterwards

The board restarts and comes up on the network - the console prints the
address it took.  The web interface is at `http://<that address>/`, and
`/json/info` says which build is running.

If the board does not come up, watch the USB console while it resets: the
ROM's boot log says whether it found the bootloader, and whether the
application started.
