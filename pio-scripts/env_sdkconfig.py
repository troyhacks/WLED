"""env_sdkconfig.py - let an environment carry its own sdkconfig settings.

The IDF's kconfig takes its values from two places: sdkconfig.defaults for
everything nothing else sets, and the environment's own sdkconfig file for
the rest.  So an environment that needs different settings - another chip
revision, say - can have them, without the shared file having to know about
it: this script points the IDF at the environment's own defaults file as
well, and that file comes last.

The files are the project root's, and the paths in SDKCONFIG_DEFAULTS are
relative to the project root.

**The chain is three files deep:**

    sdkconfig.defaults              what every chip in this tree shares
    sdkconfig.<family>.defaults     what the chip alone needs
    sdkconfig.<env>.defaults         what this environment alone needs

The family file is named by the environment's own `sdkconfig_family`
option (the P4 environments say `esp32p4`, the S31 one says `esp32s31`),
and the environment's file is named after the environment.  Every file in
the chain is optional but the first one: an environment without a family
file gets the shared settings and its own, and an environment without an
own file gets the shared settings and its family's.

The files are read in the order above, and a later file wins over an
earlier one - which is also how the IDF reads them: the defaults are
loaded in order, and a value that is in a later file overrides the one in
an earlier file.  That is what lets the rev 3 environment undo the shared
file's choice of the older memory layout.

**The defaults are only the starting point.**  Once the environment's own
sdkconfig file exists, a value that is in it wins over the defaults - so
changing a defaults file does nothing to a project that has already been
configured.  (This is the IDF's rule, and it is the opposite of what the
"defaults" name suggests.)

So when a defaults file is newer than the resolved sdkconfig, the resolved
file is **deleted** and the next configure re-seeds it from the defaults.
That is the only way a change to the defaults files takes effect, and it
is why the rev 3's settings only took hold the first time the environment
was built.
"""

import os

Import("env")  # noqa: F821  (injected by SCons)

project_dir = env.subst("$PROJECT_DIR")
pioenv = env.subst("$PIOENV")
shared = os.path.join(project_dir, "sdkconfig.defaults")

# The chip's own file.  The environment says which chip it is; without the
# option there is no family file, and the environment gets the shared
# settings and its own.
family = env.GetProjectOption("sdkconfig_family", "")
own = os.path.join(project_dir, "sdkconfig.%s.defaults" % pioenv)
extra = os.path.join(project_dir, "sdkconfig.%s.defaults" % family) if family else ""

# The chain, in the order the IDF reads it.
chain = [shared]
if extra and os.path.isfile(extra):
    chain.append(os.path.join(project_dir, os.path.basename(extra)))
if os.path.isfile(own):
    chain.append(os.path.basename(own))

if len(chain) > 1:
    os.environ["SDKCONFIG_DEFAULTS"] = ";".join(
        os.path.basename(f) for f in chain
    )

# The resolved sdkconfig holds the values the build uses, and a value in it
# beats the defaults.  So the defaults files only take effect on a resolved
# file that is regenerated from them - which means deleting the resolved one
# when a defaults file is newer than it.
#
# Every file in the chain is watched: a change to the shared one that is
# not picked up here leaves the environment on the old values silently - the
# ESP_MAIN_TASK_STACK_SIZE went that way once.
resolved = os.path.join(project_dir, "sdkconfig.%s" % pioenv)
newest = max((os.path.getmtime(f) for f in chain), default=0)
if os.path.isfile(resolved) and newest > os.path.getmtime(resolved):
    os.remove(resolved)
    print("env_sdkconfig: the defaults are newer - the resolved sdkconfig "
          "is dropped so they are applied")

if len(chain) > 1:
    print("env_sdkconfig: %s" % ";".join(os.path.basename(f) for f in chain))
