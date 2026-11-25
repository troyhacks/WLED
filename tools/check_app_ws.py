#!/usr/bin/env python
"""Compile-check the application against a websocket library.

The application uses the library's lock (WsClientGuard in wled.h), so it
has to be compiled against the header that HAS the lock - the fork's, not
the copy that .pio/libdeps holds before the fork is pulled in.

This puts the library's include directory FIRST on the compile line (so
its header wins) and compiles the application's own sources with
-fsyntax-only and the build's own flags.

Usage:  python tools/check_app_ws.py [the library] [the sources ...]

  the library   the fork, or the copy in .pio/libdeps.  The default is
                the copy in .pio/libdeps.
  the sources   the application sources.  The default is wled00/ws.cpp
                and wled00/json.cpp - the two that use the lock.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.path.join(ROOT, ".pio", "build", "esp32p4_idf_hidctl",
                  "compile_commands.json")
PIOENV = "esp32p4_idf_hidctl"


def find_library(pioenv=PIOENV):
    d = os.path.join(ROOT, ".pio", "libdeps", pioenv, "ESPAsyncWebServerWLED")
    if os.path.isfile(os.path.join(d, "src", "AsyncWebSocket.cpp")):
        return d
    return None


def command_for(path):
    with open(CC, "r", encoding="utf-8") as f:
        db = json.load(f)
    for e in db:
        if os.path.normcase(e["file"]) == os.path.normcase(path):
            return e
    return None


def compile_it(e, source, extra_inc):
    cmd = e["command"]
    cmd = re.sub(r"-MD\s+-MT\s+\S+", "", cmd)
    cmd = re.sub(r"-MF\s+\S+", "", cmd)
    cmd = re.sub(r"-o\s+\S+", "", cmd)
    cmd = re.sub(r"-c\s+\S+\.cpp\b", "-fsyntax-only", cmd)
    cmd = cmd + " " + source
    # The library's include directory goes first, so its header is the one
    # that is found.  The other libraries' include directories come
    # after: the build's own codemodel does not carry them (see
    # pio-scripts/lib_include_dirs.py), so they are added here.
    libdeps = os.path.join(ROOT, ".pio", "libdeps", PIOENV)
    incs = [extra_inc]
    # Only the libraries the application actually includes.  FastLED is
    # one of them, but the IDF build defines FASTLED_NO_FASTLED, which
    # takes its ESP32 bits out - and the define is missing from the
    # codemodel this check borrows its flags from.
    for name in ("AsyncTCP", "NeoPixelBus", "LovyanGFX", "FastLED_no_fastled"):
        d = os.path.join(libdeps, name, "src")
        if os.path.isdir(d):
            incs.append(d)
    cmd = cmd.replace(" -I", " -DFASTLED_NO_FASTLED -I", 1)
    cmd = cmd.replace(" -I", " " + " ".join("-I" + d for d in incs) + " -I", 1)
    args = [a.replace('\\"', '"') for a in re.split(r"(?<!\\)\s+", cmd) if a]
    r = subprocess.run(args, cwd=e["directory"],
                       capture_output=True, text=True, errors="replace")
    return (r.stdout or "") + (r.stderr or ""), r.returncode


def main():
    args = sys.argv[1:]
    # A library given on the command line is used as it is.
    lib = None
    if args and os.path.isdir(args[0]):
        lib = args.pop(0)
    sources = args or ["wled00/ws.cpp", "wled00/json.cpp"]

    if not lib:
        lib = find_library()
        if not lib:
            print("the library was not found - give its path")
            return 1
    inc = os.path.join(lib, "src")
    print("using the library at %s" % lib)

    bad = 0
    for s in sources:
        path = os.path.join(ROOT, s.replace("/", os.sep))
        e = command_for(path)
        if not e:
            print("%s: no compile command - skipped" % s)
            continue
        out, rc = compile_it(e, path, inc)
        errors = [l for l in out.split("\n") if "error:" in l]
        if rc != 0 or errors:
            print("=== %s: FAILED ===" % s)
            print("\n".join(errors[:20]))
            if not errors:
                print(out[-3000:])
            bad += 1
        else:
            warns = len([l for l in out.split("\n") if "warning:" in l])
            print("=== %s: OK (%d warning(s))" % (s, warns))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
