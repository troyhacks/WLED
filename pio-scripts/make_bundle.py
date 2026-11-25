"""make_bundle.py - collect the flash files for someone else.

The build leaves four images in the build directory, at four offsets, and the
factory image (all four in one file, to be written at 0x0).  This collects
them, with the flashing script, the notes and the checksums, into one folder
that can be handed over as it is.

    pio run -e esp32p4_idf_rev3
    python pio-scripts/make_bundle.py esp32p4_idf_rev3 [folder]

It runs under SCons too (then it is the "make_bundle" target of the build),
but it does not need to: the build directory is known from the environment's
name alone.
"""

import hashlib
import os
import shutil
import sys

# The images, and where they go in the flash.
IMAGES = [
    ("bootloader.bin", 0x2000),
    ("partitions.bin", 0x8000),
    ("ota_data_initial.bin", 0xE000),
    ("firmware.bin", 0x10000),
]


def _paths():
    """Return (project_dir, env_name, build_dir, out_dir).

    Under SCons the environment knows all of it; standalone the environment's
    name is the first argument and the rest follows from the layout.
    """
    try:
        env = Import("env")  # noqa: F821  (injected by SCons)
    except Exception:
        env = None

    if env is not None:
        project_dir = env.subst("$PROJECT_DIR")
        env_name = env.subst("$PIOENV")
        build_dir = env.subst("$BUILD_DIR")
        out_dir = os.path.join(project_dir, "build_output", "bundle-%s" % env_name)
        return project_dir, env_name, build_dir, out_dir

    if len(sys.argv) < 2:
        sys.exit("usage: make_bundle.py <env> [folder]")
    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    env_name = sys.argv[1]
    build_dir = os.path.join(project_dir, ".pio", "build", env_name)
    out_dir = os.path.join(project_dir, "build_output", "bundle-%s" % env_name)
    if len(sys.argv) > 2:
        out_dir = sys.argv[2]
    return project_dir, env_name, build_dir, out_dir


def build_bundle(project_dir, build_dir, out_dir):
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
    else:
        print("missing: %s" % factory)

    for name in ("flash_p4_rev3.bat", "README-flash-p4-rev3.md"):
        src = os.path.join(project_dir, "tools", name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(out_dir, name))

    # The checksums, so whoever gets the folder can tell the files are intact.
    sums = []
    for name in sorted(os.listdir(out_dir)):
        path = os.path.join(out_dir, name)
        if not os.path.isfile(path) or name == "SHA256SUMS.txt":
            continue
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        sums.append("%s  %s" % (h.hexdigest(), name))
    with open(os.path.join(out_dir, "SHA256SUMS.txt"), "w") as f:
        f.write("\n".join(sums) + "\n")
    print("SHA256SUMS.txt written")

    return out_dir


# Under SCons: run at configure time so the target exists, and the target
# itself rebuilds the bundle once the images are there.
try:
    env = Import("env")  # noqa: F821  (injected by SCons)
except Exception:
    env = None

if env is not None:
    _project_dir, _env_name, _build_dir, _out_dir = _paths()

    def _make_bundle(source, target, env):
        print("Building the bundle in %s" % _out_dir)
        build_bundle(_project_dir, _build_dir, _out_dir)

    env.AddCustomTarget(
        "make_bundle",
        dependencies=["$BUILD_DIR/${PROGNAME}.bin"],
        actions=[_make_bundle],
        title="Bundle the flash files",
    )
else:
    _project_dir, _env_name, _build_dir, _out_dir = _paths()
    build_bundle(_project_dir, _build_dir, _out_dir)
    print("\n%s" % _out_dir)
