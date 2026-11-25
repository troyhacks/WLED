"""env_sdkconfig.py - let an environment carry its own sdkconfig settings.

The IDF's kconfig takes its values from two places: sdkconfig.defaults for
everything nothing else sets, and the environment's own sdkconfig file for
the rest.  So an environment that needs different settings - another chip
revision, say - can have them, without the shared file having to know about
it: this script points the IDF at the environment's own defaults file as
well, and that file comes last.

The file is sdkconfig.<env>.defaults, next to sdkconfig.defaults.  An
environment without one gets the shared settings only, and nothing here
happens.

The IDF reads SDKCONFIG_DEFAULTS from the environment, and the paths in it
are relative to the project root.

**The defaults are only the starting point.**  Once the environment's own
sdkconfig file exists, a value that is in it wins over the defaults - so
changing a defaults file does nothing to a project that has already been
configured.  (This is the IDF's rule, and it is the opposite of what the
"defaults" name suggests.)

So when the defaults file is newer than the resolved sdkconfig, the resolved
file is **deleted** and the next configure re-seeds it from the defaults.
That is the only way a change to the defaults file takes effect, and it is
why the rev 3's settings only took hold the first time the environment was
built.
"""

import os

Import("env")  # noqa: F821  (injected by SCons)

project_dir = env.subst("$PROJECT_DIR")
pioenv = env.subst("$PIOENV")
extra_name = "sdkconfig.%s.defaults" % pioenv
extra = os.path.join(project_dir, extra_name)

if os.path.isfile(extra):
    os.environ["SDKCONFIG_DEFAULTS"] = "sdkconfig.defaults;%s" % extra_name

    # The resolved sdkconfig holds the values the build uses, and a value in
    # it beats the defaults.  So the defaults file only takes effect on a
    # resolved file that is regenerated from them - which means deleting the
    # resolved one when the defaults are newer than it.
    own = os.path.join(project_dir, "sdkconfig.%s" % pioenv)
    if os.path.isfile(own) and os.path.getmtime(extra) > os.path.getmtime(own):
        os.remove(own)
        print("env_sdkconfig: %s is newer - the resolved sdkconfig is dropped "
              "so the defaults are applied" % extra_name)

    print("env_sdkconfig: %s" % extra_name)
