# WLED-MM-P4 firmware for the ESP32-P4 rev 3

This is the WLED-MM-P4 firmware built for the **ESP32-P4 rev 3** (and later).

> **This image is for the rev 3 chip only.**  The rev 3 silicon has a
> different memory map from the older "ES pre rev.300" chip, and ESP-IDF's
> support for the two is mutually exclusive.  This firmware will not boot on
> the older chip, and the older firmware will not boot on this one.

## What you need

* An ESP32-P4 **rev 3** board (the chip reports `revision v3.2`).
* [esptool](https://pypi.org/project/esptool/): `pip install esptool`
* The board in **download mode**: hold **BOOT**, press **RESET**, release
  **BOOT**.  On the P4 the USB port on its own is not enough - the chip has
  to be told to listen for the flasher.

## Flashing

Put all the files in one folder and run:

```
flash_p4_rev3.bat COM4
```

(`COM4` is the port the board shows in Device Manager.  If you leave it out,
the script uses COM4.)

Or, by hand, the one command that does it:

```
esptool --chip esp32p4 --port COM4 write_flash 0x0 firmware.factory.bin
```

If esptool is not on your PATH: `python -m esptool --chip esp32p4 --port COM4 write_flash 0x0 firmware.factory.bin`.

(Esptool 5 renamed the command to `write-flash`; the old `write_flash` still
works there, and `write-flash` does not work on esptool 4.  The batch script
asks esptool which name it has and uses the right one.)

Before you flash, you can check that the files arrived intact - `SHA256SUMS.txt`
holds the checksum of every file in the folder:

```
certutil -hashfile firmware.factory.bin SHA256
```

### The files in the folder

| file | what it is | where it goes |
| --- | --- | --- |
| `firmware.factory.bin` | the whole flash in one image | **write it at 0x0** |
| `bootloader.bin` | the second-stage bootloader | 0x2000 |
| `partitions.bin` | the partition table | 0x8000 |
| `ota_data_initial.bin` | the OTA data partition (empty, so the factory app is the one that runs) | 0xe000 |
| `firmware.bin` | the application | 0x10000 |

The P4's ROM looks for its bootloader at **0x2000**, so if you flash the
pieces one at a time, use those offsets - in particular, do **not** put
`bootloader.bin` at 0x0:

```
esptool --chip esp32p4 --port COM4 write_flash 0x2000 bootloader.bin 0x8000 partitions.bin 0xe000 ota_data_initial.bin 0x10000 firmware.bin
```

(The same command with `write-flash` on esptool 5.)

## What is not touched

The settings, the presets and the palettes live on a partition of their own
(the filesystem partition), and none of the files above write to it.  So your
own settings survive the flash.  If you want to start from scratch anyway,
format the filesystem from the web interface afterwards.

## Afterwards

The board restarts and comes up on the network; the console prints the
address it took.  The web interface is at `http://<that address>/`, and
`/json/info` says which build is running.

If the board does not come up, watch the USB console while it resets: the
ROM's boot log says whether it found the bootloader, and whether the
application started.
