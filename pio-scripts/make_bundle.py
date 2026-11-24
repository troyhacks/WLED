"""make_bundle.py - collect the flash files for someone else.

The build leaves four images in the build directory, at four offsets, and
the factory image (all four in one file, to be written at 0x0).  This
collects them, with the flashing script and the notes, into one folder
that can be handed over as it is.

    pio run -e esp32p4_idf_rev3
    python pio-scripts/make_bundle.py esp32p4_idf_rev3 [folder]
"""

import os
import shutil
import sys

Import("env")  # noqa: F821  (injected by SCons)

project_dir = env.subst("$PROJECT_DIR")
pioenv = env.subst("$PIOENV")
build_dir = env.subst("$BUILD_DIR")

out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    project_dir, "build_output", "bundle-%s" % pioenv
)

# The images, and where they go in the flash.
IMAGES = [
    ("bootloader.bin", 0x2000),
    ("partitions.bin", 0x8000),
    ("ota_data_initial.bin", 0xE000),
    ("firmware.bin", 0x10000),
]

os.makedirs(out_dir, exist_ok=True)

for name, offset in IMAGES:
    src = os.path.join(build_dir, name)
    if not os.path.isfile(src):
        print("missing: %s" % src)
        continue
    shutil.copyfile(src, os.path.join(out_dir, name))
    print("%-24s -> 0x%06x  (%d bytes)" % (name, offset, os.path.getsize(src)))

factory = os.path.join(build_dir, "firmware.factory.bin")
if os.path.isfile(factory):
    shutil.copyfile(factory, os.path.join(out_dir, "firmware.factory.bin"))
    print("%-24s -> 0x%06x  (%d bytes)" % ("firmware.factory.bin", 0, os.path.getsize(factory)))

for name in ("flash_p4_rev3.bat", "README-flash-p4-rev3.md"):
    src = os.path.join(project_dir, "tools", name)
    if os.path.isfile(src):
        shutil.copyfile(src, os.path.join(out_dir, name))

print("\n%s" % out_dir)
