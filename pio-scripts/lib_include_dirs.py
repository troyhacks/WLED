"""lib_include_dirs.py - put the PlatformIO libraries on the compile line.

Under the Arduino framework, the libraries in lib_deps (AsyncTCP,
ESPAsyncWebServer, NeoPixelBus, LovyanGFX) are found by PlatformIO's library
dependency finder, and their include directories are handed to the compiler.

Under ESP-IDF the picture is different: CMake resolves the include paths from
the component graph, and PlatformIO's libraries are not part of it.  So the
WLED component, which includes <AsyncTCP.h> and <ESPAsyncWebServer.h>, would
not find them.

This script puts those include directories on the compile line of everything
the builder compiles, which is what the Arduino build does by default.  It is
the one place where the two builds have to agree about where the libraries
live.

The libraries themselves are still built by PlatformIO, exactly as in the
Arduino build - only the include path is added here.
"""

import glob
import os

Import("env")  # noqa: F821  (injected by SCons)

project_dir = env.subst("$PROJECT_DIR")
libdeps_dir = os.path.join(project_dir, ".pio", "libdeps", env.subst("$PIOENV"))

include_dirs = []
for path in sorted(glob.glob(os.path.join(libdeps_dir, "*", "src"))):
    if os.path.isdir(path):
        include_dirs.append(path)

if include_dirs:
    env.AppendUnique(CCFLAGS=[("-I", d) for d in include_dirs])
    print("Added %d library include directories" % len(include_dirs))
