"""Flash the ESP32-C5 remote.

The ESP32-C5's USB Serial/JTAG does not like the stub flasher: esptool
starts it, and then the first write times out.  The ROM's own flasher does
the job - it is slower, and it works.

The bootloader goes to 0x2000 on this chip (not 0x0, and not 0x1000), the
partition table to 0x8000 and the application to 0x10000.

Usage:  python tools/c5_flash.py [port] [env]
"""

import os
import subprocess
import sys

# esptool's progress bar uses characters that the Windows console's default
# code page cannot show.
os.environ.setdefault("PYTHONIOENCODING", "utf-8")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM10"
ENV = sys.argv[2] if len(sys.argv) > 2 else "esp32c5_8MB_troyhacks_lcd"
BUILD = os.path.join(".pio", "build", ENV)

BOOTLOADER = os.path.join(BUILD, "bootloader.bin")
PARTITIONS = os.path.join(BUILD, "partitions.bin")
FIRMWARE = os.path.join(BUILD, "firmware.bin")

for f in (BOOTLOADER, PARTITIONS, FIRMWARE):
    if not os.path.exists(f):
        print("missing:", f)
        sys.exit(1)

cmd = [
    sys.executable, "-m", "esptool",
    "--chip", "esp32c5",
    "--port", PORT,
    "--baud", "115200",
    "--no-stub",
    "--before", "default-reset",
    "--after", "hard-reset",
    "write_flash",
    "--flash-mode", "dio",
    "--flash-size", "detect",
    "--flash-freq", "80m",
    "0x2000", BOOTLOADER,
    "0x8000", PARTITIONS,
    "0x10000", FIRMWARE,
]

print(" ".join(cmd))
sys.exit(subprocess.call(cmd))
