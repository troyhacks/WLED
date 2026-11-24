"""touch_cmake.py - make the IDF builder notice a change in a component's CMakeLists.txt.

PlatformIO's espidf builder re-runs CMake only when the project's own
CMakeLists.txt files change:

    cmake_txt_files = [PROJECT_DIR/CMakeLists.txt, PROJECT_SRC_DIR/CMakeLists.txt]

A component under components/ has its own CMakeLists.txt, and that one is not
watched - so adding a component, or changing what a component is made of,
leaves the CMake codemodel stale.  The build then goes on with the old
component list: the sources that moved are still looked for where they used
to be, and the build fails with "Source ... not found, needed by target".

Touching the app component's CMakeLists.txt makes the mtime newer than the
CMake cache, which is exactly the condition the builder checks for.  So this
script does that, but only when a component's CMakeLists.txt is newer than the
cache - otherwise the build would re-run CMake every time.

It runs before the framework script, so the touch is in place when the check
happens.
"""

import glob
import os

Import("env")  # noqa: F821  (injected by SCons)

project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
cache = os.path.join(build_dir, "CMakeCache.txt")
watched = os.path.join(project_dir, "wled00", "CMakeLists.txt")

# Note: no exit() here.  A SConscript is executed, and exit() would take the
# whole build down with it - the build would report success without having
# built anything.
if os.path.isfile(cache):
    cache_mtime = os.path.getmtime(cache)
    components = glob.glob(os.path.join(project_dir, "components", "*", "CMakeLists.txt"))
    newer = [c for c in components if os.path.getmtime(c) > cache_mtime]
    if newer:
        os.utime(watched, None)
        print("Touched %s - %d component CMakeLists.txt changed" % (watched, len(newer)))
