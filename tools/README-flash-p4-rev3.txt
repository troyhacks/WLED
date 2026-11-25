================================================================================
 WLED-MM-P4 rev 3 - firmware and flasher
================================================================================

Everything needed to put the WLED-MM-P4 firmware on a blank ESP32-P4 rev 3
board, on Windows.  No build, no Python, nothing to install: unzip the
folder and run flash_p4_rev3.bat.

  Firmware   WLED-MM-P4 {{VERSION}} (build {{BUILD_DATE}}, commit {{COMMIT}})
  Chip       ESP32-P4 rev 3 - the chip reports "revision v3.2"
  Flasher    esptool {{ESPTOOL_VERSION}}, the Espressif build for Windows
  Board      WLED-P4v3

  THIS FIRMWARE IS FOR THE REV 3 CHIP ONLY.  The rev 3 silicon has a
  different memory map from the older "ES pre rev.300" chip, and ESP-IDF's
  support for the two is mutually exclusive.  This firmware will not boot on
  the older chip, and the older firmware will not boot on this one.  The
  flashing script reads the chip revision and refuses to flash if you have
  pointed it at the wrong board.


--------------------------------------------------------------------------------
 WHAT IS IN THIS FOLDER
--------------------------------------------------------------------------------

  flash_p4_rev3.bat       the flasher: double-click it, or run it from a
                          command prompt
  esptool.exe             Espressif's esptool {{ESPTOOL_VERSION}} for Windows -
                          the tool that talks to the chip
  firmware.factory.bin    the whole flash in one image - this is the file you
                          flash
  bootloader.bin          the second-stage bootloader (goes to 0x2000)
  partitions.bin          the partition table (goes to 0x8000)
  ota_data_initial.bin    the OTA data partition, empty (goes to 0xe000)
  firmware.bin            the application (goes to 0x10000)
  SHA256SUMS.txt          the checksum of every file in this folder
  esptool-LICENSE.txt     the licence of the bundled esptool

  The five .bin files are there if you want to flash the pieces one at a
  time.  If you do not, ignore them: firmware.factory.bin already holds all
  of them at the right offsets.


--------------------------------------------------------------------------------
 HOW TO FLASH
--------------------------------------------------------------------------------

  1. Plug the board in with a USB DATA cable (a charge-only cable carries
     no data) into the USB port that the USB-Serial/JTAG is on.  On Windows
     a "USB Serial Device (COMx)" appears in Device Manager.

  2. Put the board in download mode.  A blank board does not start the
     flasher on its own, so tell it to listen:
         hold BOOT
         press and release RESET
         release BOOT

  3. Run flash_p4_rev3.bat.  Double-click it, or from a command prompt:

         flash_p4_rev3.bat

     If the machine has more than one Espressif board on it, the script
     lists the ports and asks which one to use.  Run it again with that
     port:

         flash_p4_rev3.bat COM14

     The script finds the board, reads its chip revision, flashes the
     image and resets the board.  It takes about half a minute.

  4. Wait for the board.  It restarts and joins the network in about 20
     seconds.  The USB console prints the address it took:

         IP address: http://192.168.6.220 via Ethernet (Primary Route)

     and the web interface is at http://that-address/ .

     The board answers to the name WLED-P4v3, so http://wled-p4v3.local/
     works too if the machine is on the same network.

  The first start formats the settings partition, so the board comes up with
  the factory settings - the ones the web interface saves live from then on.


--------------------------------------------------------------------------------
 IF IT DOES NOT WORK
--------------------------------------------------------------------------------

  "No Espressif board found"
      No USB serial device showed up.
        - a charge-only cable cannot carry data; use a data cable
        - on the P4, use the USB port that the USB-Serial/JTAG is on; the
          other USB port is the USB host (the one for the F1 controller and
          the deck)
        - if Device Manager shows nothing at all, the USB driver is missing

  "The board on COMx did not answer"
      The chip did not talk back.
        - put it in download mode again: hold BOOT, press RESET, release BOOT
        - close every other program that has the port open (a serial
          monitor, an Arduino IDE, a PlatformIO monitor).  Two programs
          fighting over one serial port is the usual reason this happens.

  "The board on COMx is NOT a rev 3 chip"
      The script found the older P4.  Point it at the other port.  The two
      boards look identical from the outside; only the chip revision tells
      them apart.

  The write fails half way through
      The script already retries once without esptool's stub flasher, which
      is slower but does not depend on the chip's RAM map.  If that fails
      too, put the board in download mode again and run it once more.

  The board does not come up after the flash
      Watch the USB console while it resets.  The ROM's boot log says
      whether it found the bootloader and whether the application started.


--------------------------------------------------------------------------------
 NOTES
--------------------------------------------------------------------------------

  Offsets
      The P4's ROM looks for its bootloader at 0x2000, not at 0x0.  The
      merged image firmware.factory.bin has the bootloader at 0x2000, the
      partition table at 0x8000, the empty OTA data partition at 0xe000 and
      the application at 0x10000, and the first 8 KB are padding.  It is
      written at 0x0 as one blob, which is why there is only one command:

          esptool.exe --chip esp32p4 --port COM14 write-flash 0x0 firmware.factory.bin

      To flash the pieces one at a time instead:

          esptool.exe --chip esp32p4 --port COM14 write-flash 0x2000 bootloader.bin 0x8000 partitions.bin 0xe000 ota_data_initial.bin 0x10000 firmware.bin

  The stub flasher
      Esptool normally uploads a small "stub flasher" into the chip's RAM
      and lets that do the writing.  The stub is built for the OLDER P4
      memory map, and on the rev 3 silicon it starts up noisy ("Invalid
      head of packet (0x50)") and panics the chip the moment esptool asks
      it to change the baud rate:

          Changing baud rate to 921600...
          A fatal error occurred: Guru Meditation Error detected.

      esptool {{ESPTOOL_VERSION}} has a stub that fits the rev 3 chip and runs
      clean, so the bundled esptool.exe does not hit this.  If you use an
      older esptool (the one PlatformIO ships is 5.1.2), add --no-stub to
      the command above: that leaves the flashing to the ROM loader, which
      is part of the silicon and cannot be wrong about the map.  It is
      slower - the whole image takes about 20 s instead of a few - but it
      works.

  What the flash does not touch
      The settings, the presets and the palettes live on a partition of
      their own, and none of the files here write to it, so they survive a
      re-flash.  On a blank board that partition is empty, and the first
      start formats it.

  Checking the files
      SHA256SUMS.txt holds the checksum of every file in this folder.  To
      check one:

          certutil -hashfile firmware.factory.bin SHA256

      and compare it with the line for the same file in SHA256SUMS.txt.


--------------------------------------------------------------------------------
 WLED-MM-P4 - {{VERSION}} - built {{BUILD_DATE}} from {{COMMIT}}
--------------------------------------------------------------------------------
