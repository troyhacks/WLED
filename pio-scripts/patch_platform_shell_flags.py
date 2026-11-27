"""patch the platform's IDF builder so it understands CMake's SHELL: prefix.

CMake writes a compile option that must not be split into a list with the
prefix "SHELL:" in front of it:

    "SHELL:-include eh_host_port_master_config.h"

esp_hosted 3.x uses that for a header it wants force-included into its
own sources.  PlatformIO's IDF builder does not know the prefix: it
strips the quotes and splits the fragment on whitespace, so the option
arrives at the compiler as

    SHELL:-include  eh_host_port_master_config.h

- the "-include" is lost, and the header ends up as a bare argument on
the compile line.  GCC then sees two input files and says:

    fatal error: cannot specify '-o' with '-c', '-S' or '-E' with
    multiple files

The fix is to take the prefix off before the fragment is split, so the
option and its argument stay together.

There is a second half to it.  The builder sorts the flags it took
from the code model ("CMake randomly populates build flags in code
model"), and sorting pulls "-include" away from its file: the option
sorts among the other "-" options, the file among the plain names.  So
the file lands on the compile line on its own again, and GCC says the
same thing.  The sorting has to keep an option and its argument
together.

The patch is applied to the platform package in place and it is
idempotent: the script runs on every build and does nothing when the
patch is already there.
"""

import os

Import("env")  # noqa: F821  (injected by SCons)

# The two places the builder takes a fragment apart.  Both strip the
# quotes first and then split what is left.
PATCHES = [
    (
        '                fragment = raw_fragment.strip("\\" ")',
        '                fragment = raw_fragment.strip("\\" ")'
        '.removeprefix("SHELL:")',
    ),
    (
        '            build_flags = raw_fragment.strip("\\" ")',
        '            build_flags = raw_fragment.strip("\\" ")'
        '.removeprefix("SHELL:")',
    ),
]

# The sorting, and the helper that keeps an option with its argument.
HELPER = '''def _sorted_flags(flags):
    """Sort the flags, keeping an option and its argument together.

    "-include <file>" is two entries, and sorting them apart puts the
    file on the compile line as a source of its own - GCC then refuses
    the command ("cannot specify '-o' with '-c' and multiple files").
    """
    flags = list(flags)
    pairs = []
    index = 0
    while index < len(flags):
        if flags[index] == "-include" and index + 1 < len(flags):
            pairs.append([flags[index], flags[index + 1]])
            index += 2
        else:
            pairs.append([flags[index]])
            index += 1
    pairs.sort(key=lambda pair: pair[0])
    return [flag for pair in pairs for flag in pair]


'''

SORT_PATCHES = [
    (
        '        "ASPPFLAGS": sorted(app_flags.get("ASM", default_flags.get("ASM"))),',
        '        "ASPPFLAGS": _sorted_flags(app_flags.get("ASM", default_flags.get("ASM"))),',
    ),
    (
        '        "CFLAGS": sorted(app_flags.get("C", default_flags.get("C"))),',
        '        "CFLAGS": _sorted_flags(app_flags.get("C", default_flags.get("C"))),',
    ),
    (
        '        "CXXFLAGS": sorted(app_flags.get("CXX", default_flags.get("CXX"))),',
        '        "CXXFLAGS": _sorted_flags(app_flags.get("CXX", default_flags.get("CXX"))),',
    ),
    (
        "def get_app_flags(app_config, default_config):",
        HELPER + "def get_app_flags(app_config, default_config):",
    ),
]

# The bootloader's build.  The application's flags are merged into the
# global environment before the components are built, and they carry
# esp_hosted's forced include - but the bootloader has its own
# configuration, which does not know the hosted coprocessor, so the
# header stops the build with "No Slave Target or more than one Slave
# Target was defined".  The bootloader is not a hosted TU: it does not
# need the include.
#
# The block ends with a marker, so a later version of this patch can
# take the old one out before it puts its own in.
BOOTLOADER_MARKER = "  # esp_hosted include patch"

BOOTLOADER_PATCH = (
    "    bootloader_env = env.Clone()",
    '''    bootloader_env = env.Clone()
    # The application's compile flags are merged into the global environment
    # before the components are built, and they carry esp_hosted's forced
    # include ("-include eh_host_port_master_config.h").  The bootloader is
    # built with its own configuration, which does not know the hosted
    # coprocessor, so the header would stop the build with "No Slave Target
    # or more than one Slave Target was defined".  The bootloader is not a
    # hosted TU and does not need the include.
    for _flags in ("CCFLAGS", "CFLAGS", "CXXFLAGS", "ASPPFLAGS"):
        _kept = []
        _skip_next = False
        for _flag in bootloader_env.get(_flags, []):
            if _skip_next:
                _skip_next = False
                continue
            if _flag == "-include":
                _skip_next = True
                continue
            _kept.append(_flag)
        bootloader_env[_flags] = _kept''' + BOOTLOADER_MARKER,
)

# The same leak, one level up.
#
# The application's flags are merged into the global environment
# (env.MergeFlags(project_flags)) and every IDF component is built from
# it.  So a forced include that belongs to one component - esp_hosted's
# "eh_host_port_master_config.h", which esp_hosted puts on the
# application through its interface - lands on every component's compile
# line.  The components that do not have the header in their include path
# then stop the build:
#
#   <command-line>: fatal error: eh_host_port_master_config.h: No such
#       file or directory
#
# The fix is per component, and it is where the information is: a component
# keeps the forced include when it can find the header in its own include
# path, and loses it when it cannot.  The components that want the header
# (esp_hosted's own) have it in their path; the others do not.
#
# The header itself is written for this: it says "auto-included into every
# host TU" and it is a pure Kconfig-to-macro translator.
DROP_INCLUDES_PATCH = (
    "        build_env.AppendUnique(CPPDEFINES=defines, CPPPATH=includes)",
    '''        build_env.AppendUnique(CPPDEFINES=defines, CPPPATH=includes)
        # A forced include ("-include <file>") that came in with the
        # application's flags belongs to one component (esp_hosted's
        # "eh_host_port_master_config.h"), and the application's flags are
        # merged into the global environment - so it ends up on every
        # component's compile line.  The components that do not have the
        # header in their include path then fail to compile:
        #
        #   <command-line>: fatal error: eh_host_port_master_config.h:
        #       No such file or directory
        #
        # The component that has the header in its include path keeps the
        # include; the others lose it.
        _forced_include_dirs = list(build_env.get("CPPPATH", []))
        for _flags in ("CCFLAGS", "CFLAGS", "CXXFLAGS", "ASPPFLAGS"):
            _kept = []
            _index = 0
            _list = list(build_env.get(_flags, []))
            while _index < len(_list):
                _flag = _list[_index]
                if _flag == "-include" and _index + 1 < len(_list):
                    _header = _list[_index + 1]
                    if os.path.isabs(_header):
                        _found = os.path.isfile(_header)
                    else:
                        _found = any(
                            os.path.isfile(os.path.join(_dir, _header))
                            for _dir in _forced_include_dirs
                        )
                    if _found:
                        _kept.append(_flag)
                        _kept.append(_header)
                    _index += 2
                    continue
                _kept.append(_flag)
                _index += 1
            build_env[_flags] = _kept''',
)


def drop_old_bootloader_block(src):
    """Take out a bootloader block an earlier version of this patch left.

    The "bootloader_env = env.Clone()" line stays: it is the anchor the
    patch below looks for, and without it the block would not come back.
    """
    start = src.find("    bootloader_env = env.Clone()")
    if start < 0:
        return src
    end = src.find(BOOTLOADER_MARKER, start)
    if end < 0:
        return src
    line_end = src.find("\n", end)
    if line_end < 0:
        line_end = len(src)
    return (src[:start] + "    bootloader_env = env.Clone()\n"
            + src[line_end + 1:])


def builder_path():
    try:
        platform_dir = env.PioPlatform().get_dir()
    except Exception:  # noqa: BLE001  (a build must not stop here)
        return None
    if not platform_dir:
        return None
    path = os.path.join(platform_dir, "builder", "frameworks", "espidf.py")
    return path if os.path.isfile(path) else None


path = builder_path()
if not path:
    print("[shellflags] the platform's IDF builder was not found - not patching")
else:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()
    src = drop_old_bootloader_block(src)
    n = 0
    for old, new in PATCHES + SORT_PATCHES + [DROP_INCLUDES_PATCH, BOOTLOADER_PATCH]:
        if new in src:
            continue
        if old in src:
            src = src.replace(old, new)
            n += 1
    if n:
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        print("[shellflags] patched %d place(s) in %s: CMake's SHELL: prefix "
              "has to come off before the fragment is split, and the flags "
              "have to be sorted without an option losing its argument"
              % (n, path))
    else:
        print("[shellflags] %s is already patched" % path)
