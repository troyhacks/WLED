"""patch_nexpixelbus_s3.py - teach NeoPixelBus about the ESP32-S3.

**The LCD method** is the S3's own: the library's NeoEsp32LcdXMethod.h is
guarded by

    #if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32S3)

so it is compiled for the S3 and for nothing else - the P4 and the S31
never see it, which is why they build.

It calls the IDF's GDMA driver the way IDF 5.x had it:

    gdma_new_ahb_channel(&dma_chan_config, &_dmaChannel);

IDF 6.x added the receive channel to the call, so the same call is now:

    gdma_new_ahb_channel(&dma_chan_config, &_dmaChannel, NULL);

The third argument is the receive channel, and NULL is what a send-only
channel wants.

The patch is applied in place, it is idempotent, and it runs on every build.

NOTE: the library is a build artefact - PlatformIO clones it into
.pio/libdeps and never updates it - so the patch has to be applied to the
clone, which is what this script does.
"""

import glob
import os

Import("env")  # noqa: F821  (injected by SCons)

LCD_METHOD = os.path.join("src", "internal", "methods", "ESP", "ESP32", "NeoEsp32LcdXMethod.h")

OLD = "gdma_new_ahb_channel(&dma_chan_config, &_dmaChannel);"
NEW = "gdma_new_ahb_channel(&dma_chan_config, &_dmaChannel, NULL);"

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
if not libdeps or not os.path.isdir(libdeps):
    print("[neopixelbus] no libdeps folder yet - nothing to do")
    raise SystemExit(0)

for path in sorted(glob.glob(os.path.join(libdeps, "*", "NeoPixelBus"))):
    if not os.path.isdir(path):
        continue
    file = os.path.join(path, LCD_METHOD)
    if not os.path.isfile(file):
        print("[neopixelbus] %s is not there - not patching" % LCD_METHOD)
        continue
    with open(file, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    if NEW in text:
        print("[neopixelbus] %s is already patched" % LCD_METHOD)
        continue
    if OLD not in text:
        print("[neopixelbus] %s has no such call - not patching" % LCD_METHOD)
        continue
    text = text.replace(OLD, NEW)
    with open(file, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("[neopixelbus] patched %s: the GDMA call has the receive channel" % LCD_METHOD)
