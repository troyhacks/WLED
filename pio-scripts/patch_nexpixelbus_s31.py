"""patch_nexpixelbus_i2s.py - teach NeoPixelBus about the ESP32-S31.

Two things in NeoPixelBus have to know the chip:

**The I2S method** is written for the classic ESP32's I2S: it reads the
peripheral's registers itself (soc/i2s_struct.h) and drives them with the
Arduino core's own helpers.  Every chip whose I2S was changed since then is
excluded from it by a chip guard in the library:

    #if !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3) ...

The ESP32-S31 is not in that list - it is newer than the library's guard -
so the guard lets its I2S through, and the build breaks on the missing
register header (soc/i2s_struct.h does not exist for the S31, and it does
not exist for the P4 either - which is why the P4 is in the list).

The S31's LED bus does not need that method: the WLED-MM build drives the
strip over the Parallel IO block on the chips that have one (the P4 and the
S31 both do), and the RMT method - which is the IDF's own driver - is
compiled for it.

So the patch does three things:

  - it adds the S31 to the three guards that exclude the I2S method whole
    (the implementation and the two method classes),
  - it adds the S31 to the chip list in XMethods.h that picks the RMT
    method's "X" variants - without that, the library's own header asks
    for the I2S types that the first patch took away, and
  - it adds the S31 to the chip list in the bit-bang method, which is the
    one that writes the GPIO registers itself: the S31 has the RISC-V
    cycle counter and the newer GPIO register layout, like the P4.

The patch is applied in place, it is idempotent, and it runs on every build.

NOTE: the library is a build artefact - PlatformIO clones it into
.pio/libdeps and never updates it - so the patch has to be applied to the
clone, which is what this script does.
"""

import glob
import os

Import("env")  # noqa: F821  (injected by SCons)

CORE = os.path.join("src", "internal", "methods", "ESP", "ESP32", "Core_2_x")

# The guard the library uses for "this chip's I2S is not the classic one".
I2S_GUARD = ("#if !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3) "
              "&& !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32C5) "
              "&& !defined(CONFIG_IDF_TARGET_ESP32H2) && !defined(CONFIG_IDF_TARGET_ESP32P4)")

# The chip list that picks the RMT method's "X" variants.
X_GUARD = ("#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) "
           "|| defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32H2) "
           "|| defined(CONFIG_IDF_TARGET_ESP32P4)")

# The chip list in the bit-bang method: the RISC-V cycle counter and the
# newer GPIO register layout.
BITBANG_GUARD = ("#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) "
                 "|| defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32H2) "
                 "|| defined(CONFIG_IDF_TARGET_ESP32P4)")

S31 = "CONFIG_IDF_TARGET_ESP32S31"

# The patches: the file, the text that is there and the text that replaces it.
PATCHES = [
    (os.path.join(CORE, "Esp32_i2s.c"), I2S_GUARD, I2S_GUARD + " && !defined(%s)" % S31),
    (os.path.join(CORE, "NeoEsp32I2sMethod.h"), I2S_GUARD, I2S_GUARD + " && !defined(%s)" % S31),
    (os.path.join(CORE, "NeoEsp32I2sXMethod.h"), I2S_GUARD, I2S_GUARD + " && !defined(%s)" % S31),
    (os.path.join("src", "internal", "XMethods.h"), X_GUARD, X_GUARD + " || defined(%s)" % S31),
    (os.path.join("src", "internal", "methods", "ESP", "NeoEspBitBangMethod.cpp"),
     BITBANG_GUARD, BITBANG_GUARD + " || defined(%s)" % S31),
]

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
if not libdeps or not os.path.isdir(libdeps):
    print("[neopixelbus] no libdeps folder yet - nothing to do")
    raise SystemExit(0)

for path in sorted(glob.glob(os.path.join(libdeps, "*", "NeoPixelBus"))):
    if not os.path.isdir(path):
        continue
    for rel, old, new in PATCHES:
        file = os.path.join(path, rel)
        if not os.path.isfile(file):
            print("[neopixelbus] %s is not there - not patching" % rel)
            continue
        with open(file, "r", encoding="utf-8", newline="") as fh:
            text = fh.read()
        if new in text:
            print("[neopixelbus] %s is already patched" % rel)
            continue
        if old not in text:
            print("[neopixelbus] %s has no such guard - not patching" % rel)
            continue
        text = text.replace(old, new)
        with open(file, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        print("[neopixelbus] patched %s: the S31 is in the right place" % rel)
