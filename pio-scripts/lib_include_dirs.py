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

# The platform layer's include directories.  The libraries need them too -
# ESPAsyncWebServer includes <FS.h>, <LittleFS.h>, <IPAddress.h>, <Print.h>
# and <libb64/cencode.h> - and they are compiled by PlatformIO, not by CMake.
# The IDF builder puts the application's own include directories on the
# libraries' compile line as well, but that set comes out of CMake's codemodel,
# which is only regenerated when CMakeLists.txt changes.  Listing them here
# makes the libraries' include path independent of that timing.
for sub in ("", "include", "vendor", os.path.join("vendor", "fs")):
    path = os.path.join(project_dir, "components", "wled_platform", sub) if sub else \
        os.path.join(project_dir, "components", "wled_platform")
    if os.path.isdir(path):
        include_dirs.append(path)

# esp_hosted 3.x force-includes one of its headers into every source file
# that links its targets:
#
#   target_compile_options(eh_host_config INTERFACE
#       "SHELL:-include eh_host_port_master_config.h")
#
# The builder hands the application's compile flags to every component it
# compiles, so that "-include" reaches components that have nothing to do
# with esp_hosted - and they cannot find the header.  The header is a
# macro translator with every definition guarded, so it is safe to have
# it on the include path everywhere.
#
# The assembler gets the flags too (ASPPFLAGS), so the directory has to
# be on that path as well - the assembly sources are preprocessed, and
# they fail the same way.
esp_hosted_dirs = []
for path in sorted(glob.glob(os.path.join(
        project_dir, "managed_components", "espressif__esp_hosted",
        "host", "*", "include"))):
    if os.path.isdir(path):
        esp_hosted_dirs.append(path)
        include_dirs.append(path)

if include_dirs:
    env.AppendUnique(CCFLAGS=[("-I", d) for d in include_dirs])
    print("Added %d library include directories" % len(include_dirs))

if esp_hosted_dirs:
    env.AppendUnique(ASPPFLAGS=[("-I", d) for d in esp_hosted_dirs])
    print("Added %d esp_hosted include directories to the assembler"
          % len(esp_hosted_dirs))
