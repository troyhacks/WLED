"""make_bundle.py - collect the flash files for someone else.

The build leaves four images in the build directory, at four offsets, and the
factory image (all four in one file, to be written at 0x0).  This collects
them, with the flashing script, the notes and the checksums, into one folder
that can be handed over as it is.

    pio run -e esp32p4_idf_rev3
    python pio-scripts/make_bundle.py esp32p4_idf_rev3 [folder]

The folder is self-sufficient on Windows: it carries Espressif's esptool.exe
(the official build for Windows), so whoever gets it needs nothing else - no
Python, no installation, no network.  The exe is downloaded once into
tools/.cache/ and kept there; if it cannot be fetched, the bundle is still
made and the flashing script falls back to an esptool on the machine.

It runs under SCons too (then it is the "make_bundle" target of the build),
but it does not need to: the build directory is known from the environment's
name alone.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

# The images, and where they go in the flash.
IMAGES = [
    ("bootloader.bin", 0x2000),
    ("partitions.bin", 0x8000),
    ("ota_data_initial.bin", 0xE000),
    ("firmware.bin", 0x10000),
]

# The notes and the flashing script that go into the bundle.
SCRIPT = "flash_p4_rev3.bat"
NOTES = "README-flash-p4-rev3.txt"

# The official Espressif build of esptool for Windows.  It needs nothing
# else on the machine - that is the whole point of shipping it.
ESPTOOL_VERSION = "5.7.0"
ESPTOOL_URL = (
    "https://github.com/espressif/esptool/releases/download/"
    f"v{ESPTOOL_VERSION}/esptool-v{ESPTOOL_VERSION}-windows-amd64.zip"
)
ESPTOOL_SHA256 = "c97911246149141eab535b3a6dd5367cd3c16ba17098bce27ac6af780708c9e9"


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


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _git(project_dir, *args):
    try:
        return subprocess.check_output(
            ["git", "-C", project_dir] + list(args), text=True
        ).strip()
    except Exception:
        return "unknown"


def _fetch_esptool(project_dir, out_dir):
    """Put Espressif's esptool.exe into the bundle.

    It is downloaded once into tools/.cache/ and kept there.  If it cannot be
    fetched, the bundle is still made - the flashing script then looks for an
    esptool on the machine instead.
    """
    cache = os.path.join(project_dir, "tools", ".cache")
    os.makedirs(cache, exist_ok=True)
    zip_path = os.path.join(cache, f"esptool-v{ESPTOOL_VERSION}-windows-amd64.zip")
    unpacked = os.path.join(cache, "esptool")

    try:
        if not os.path.isfile(zip_path):
            print("Downloading esptool %s ..." % ESPTOOL_VERSION)
            urllib.request.urlretrieve(ESPTOOL_URL, zip_path)
        if _sha256(zip_path) != ESPTOOL_SHA256:
            print("The esptool zip is not the one we pinned - skipping it")
            return
        exe = os.path.join(unpacked, "esptool.exe")
        if not os.path.isfile(exe):
            os.makedirs(unpacked, exist_ok=True)
            with zipfile.ZipFile(zip_path) as z:
                for member in z.namelist():
                    name = os.path.basename(member)
                    if name in ("esptool.exe", "LICENSE"):
                        with z.open(member) as src, open(
                            os.path.join(unpacked, name), "wb"
                        ) as dst:
                            shutil.copyfileobj(src, dst)
        shutil.copyfile(exe, os.path.join(out_dir, "esptool.exe"))
        shutil.copyfile(
            os.path.join(unpacked, "LICENSE"),
            os.path.join(out_dir, "esptool-LICENSE.txt"),
        )
        print("esptool.exe (%d bytes)" % os.path.getsize(exe))
    except Exception as exc:
        print("No esptool.exe in the bundle: %s" % exc)
        print("The flashing script will use an esptool on the machine instead.")


def _write_notes(project_dir, out_dir):
    """The notes that go with the bundle, as plain text.

    Windows does not know what to do with a .md file, so the notes that
    travel with the firmware are plain text.
    """
    src = os.path.join(project_dir, "tools", NOTES)
    if not os.path.isfile(src):
        return
    with open(src, encoding="utf-8") as f:
        notes = f.read()

    version = "unknown"
    try:
        with open(os.path.join(project_dir, "package.json"), encoding="utf-8") as f:
            version = json.load(f)["version"]
    except Exception:
        pass
    notes = notes.replace("{{VERSION}}", version)
    notes = notes.replace(
        "{{BUILD_DATE}}", _git(project_dir, "log", "-1", "--format=%cd", "--date=short")
    )
    notes = notes.replace("{{COMMIT}}", _git(project_dir, "rev-parse", "--short", "HEAD"))
    notes = notes.replace("{{ESPTOOL_VERSION}}", ESPTOOL_VERSION)
    if "{{" in notes:
        left = [line for line in notes.splitlines() if "{{" in line]
        sys.exit("Unfilled placeholders in %s:\n  %s" % (NOTES, "\n  ".join(left)))

    with open(os.path.join(out_dir, "README.txt"), "w", encoding="utf-8") as f:
        f.write(notes)
    print("README.txt written")


def _crlf(path):
    """cmd.exe is happiest with CRLF line endings, and Notepad shows a plain
    text file the way it should with them too."""
    with open(path, "rb") as f:
        data = f.read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    with open(path, "wb") as f:
        f.write(data)


def build_bundle(project_dir, build_dir, out_dir):
    # Start from an empty folder: a file left over from an earlier bundle
    # would otherwise ship again, and the checksums would cover it too.
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
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

    for name in (SCRIPT,):
        src = os.path.join(project_dir, "tools", name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(out_dir, name))

    _write_notes(project_dir, out_dir)
    _fetch_esptool(project_dir, out_dir)

    for name in (SCRIPT, "README.txt"):
        path = os.path.join(out_dir, name)
        if os.path.isfile(path):
            _crlf(path)

    # The checksums, so whoever gets the folder can tell the files are intact.
    sums = []
    for name in sorted(os.listdir(out_dir)):
        path = os.path.join(out_dir, name)
        if not os.path.isfile(path) or name == "SHA256SUMS.txt":
            continue
        sums.append("%s *%s" % (_sha256(path), name))
    with open(os.path.join(out_dir, "SHA256SUMS.txt"), "w") as f:
        f.write("\n".join(sums) + "\n")
    print("SHA256SUMS.txt written")

    # And a zip of it, because one file is easier to hand over than a folder.
    zip_path = os.path.join(os.path.dirname(out_dir), os.path.basename(out_dir) + ".zip")
    if os.path.isfile(zip_path):
        os.remove(zip_path)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(out_dir)):
            path = os.path.join(out_dir, name)
            if os.path.isfile(path):
                z.write(path, os.path.join(os.path.basename(out_dir), name))
    print("%s (%d bytes)" % (zip_path, os.path.getsize(zip_path)))

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
