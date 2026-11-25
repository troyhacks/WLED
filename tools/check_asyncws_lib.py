#!/usr/bin/env python
"""Compile-check the websocket library, with the build's own flags.

The library is compiled by PlatformIO, not by CMake, so it is not in
compile_commands.json.  This takes the flags from a wled00 source, adds
the library's own include paths, and compiles the library source
with -fsyntax-only: no link, no object, just the compiler's opinion of
the C++.

Usage:  python tools/check_asyncws_lib.py [the path to the library]
"""
import json
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, ".pio", "build", "esp32p4_idf_hidctl")
CC = os.path.join(ROOT, ".pio", "build", "esp32p4_idf_hidctl", "compile_commands.json")


def find_library():
    for env in sorted(os.listdir(os.path.join(ROOT, ".pio", "libdeps"))):
        d = os.path.join(ROOT, ".pio", "libdeps", env, "ESPAsyncWebServerWLED")
        if os.path.isfile(os.path.join(d, "src", "AsyncWebSocket.cpp")):
            return d
    return None


def sample_command():
    with open(CC, "r", encoding="utf-8") as f:
        db = json.load(f)
    for e in db:
        if "/wled00/" in e["file"].replace("\\", "/"):
            return e
    return None


def main():
    lib = sys.argv[1] if len(sys.argv) > 1 else find_library()
    if not lib:
        print("the library was not found")
        return 1
    src = os.path.join(lib, "src", "AsyncWebSocket.cpp")
    print("checking %s" % src)

    e = sample_command()
    if not e:
        print("no sample command in compile_commands.json")
        return 1

    cmd = e["command"]
    # Everything that is only about the output, and then the source.
    cmd = re.sub(r"-MD\s+-MT\s+\S+", "", cmd)
    cmd = re.sub(r"-MF\s+\S+", "", cmd)
    cmd = re.sub(r"-o\s+\S+", "", cmd)
    cmd = re.sub(r"-c\s+\S+\.cpp\b", "-fsyntax-only", cmd)
    cmd = cmd + " " + src

    # The library's own include paths.
    extra = [
        "-I" + os.path.join(lib, "src"),
        "-I" + os.path.join(ROOT, ".pio", "libdeps", "esp32p4_idf_hidctl", "AsyncTCP", "src"),
        "-I" + os.path.join(ROOT, "components", "wled_compat", "include"),
    ]
    cmd = cmd + " " + " ".join(extra)

    # The command is split on the spaces that are not escaped: the defines
    # carry \" inside them, and shlex would eat the quotes.  The shell that
    # runs the real build strips those backslashes, so we do too.
    args = [a.replace('\\"', '"') for a in re.split(r"(?<!\\)\s+", cmd) if a]

    r = subprocess.run(args, cwd=e["directory"],
                       capture_output=True, text=True, errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    if "error" in out.lower() or r.returncode != 0:
        print(out[-8000:])
        print("FAILED (%d)" % r.returncode)
        return 1
    # The warnings are worth seeing too.
    warn = [l for l in out.split("\n") if "warning" in l]
    for l in warn[:40]:
        print(l)
    print("OK - %d warning(s)" % len(warn))
    return 0


if __name__ == "__main__":
    sys.exit(main())
