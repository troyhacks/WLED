"""env_sdkconfig.py - let an environment carry its own sdkconfig settings.

The IDF's kconfig takes its values from two places: sdkconfig.defaults for
everything nothing else sets, and the environment's own sdkconfig file for
the rest.  So an environment that needs different settings - another chip
revision, say - can have them, without the shared file having to know about
it: this script points the IDF at the environment's own defaults file as
well, and that file comes last, so what is in it wins.

The file is sdkconfig.<env>.defaults, next to sdkconfig.defaults.  An
environment without one gets the shared settings only, and nothing here
happens.

The IDF reads SDKCONFIG_DEFAULTS from the environment, and the paths in it
are relative to the project root.
"""

import os

Import("env")  # noqa: F821  (injected by SCons)

project_dir = env.subst("$PROJECT_DIR")
pioenv = env.subst("$PIOENV")
extra_name = "sdkconfig.%s.defaults" % pioenv
extra = os.path.join(project_dir, extra_name)

if os.path.isfile(extra):
    os.environ["SDKCONFIG_DEFAULTS"] = "sdkconfig.defaults;%s" % extra_name

    # The builder only re-runs CMake when the shared defaults or the
    # environment's own sdkconfig file change, and this file is neither.
    # Touching the environment's sdkconfig makes it re-resolve, which is what
    # picks the settings up.
    own = os.path.join(project_dir, "sdkconfig.%s" % pioenv)
    if os.path.isfile(own) and os.path.getmtime(extra) > os.path.getmtime(own):
        os.utime(own, None)

    print("env_sdkconfig: %s" % extra_name)
