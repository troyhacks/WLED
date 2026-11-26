"""keep the repository's .gitignore, and stop the IDE from taking it over.

The VS Code PlatformIO extension runs `pio project init --ide vscode`,
and the core's VS Code integration writes its own .gitignore over the
project's - the repository's file is replaced by the core's
eight-entry template.

Two things are done here.

**One: the core's template is neutralised.**  The core ships two
templates for the same file:

    project/integration/tpls/vscode/.gitignore.tpl
    project/integration/tpls/vscode/.gitignore.optional.tpl

The generator writes a template unless the file name ends in
".optional" *and* the file is already there.  So the plain one always
overwrites, and the optional one never does.  Renaming the plain one
to end in ".optional" leaves the core with one template that does not
touch a file that is already there - and a project that has no
.gitignore still gets one ✓.

**Two: the repository's file is put back** if it was taken over
before this script ran (or by something else).  The content comes
from git (the tracked file) ✓.

The script runs before the build, and the extension's index build is
a build ✓, so the file is back before anyone sees it.

**No SystemExit here.**  A script that ends with `raise SystemExit(0)`
takes the whole SCons build with it: PlatformIO reports SUCCESS and
nothing is compiled.
"""

import os
import shutil
import subprocess

Import("env")  # noqa: F821  (injected by SCons)

project_dir = env.subst("$PROJECT_DIR")
path = os.path.join(project_dir, ".gitignore")


def core_templates_dir():
    """The core's VS Code template directory, or None."""
    try:
        import platformio
    except Exception:  # noqa: BLE001  (a build must not stop here)
        return None
    # platformio.__file__ is <site-packages>/platformio/__init__.py
    core = os.path.dirname(os.path.abspath(platformio.__file__))
    return os.path.join(core, "project", "integration", "tpls", "vscode")


def neutralise_the_template():
    """Stop the core from overwriting a .gitignore that is already there."""
    directory = core_templates_dir()
    if not directory:
        return
    plain = os.path.join(directory, ".gitignore.tpl")
    optional = os.path.join(directory, ".gitignore.optional.tpl")
    if not os.path.isfile(plain):
        return
    if not os.path.isfile(optional):
        # The optional one is the one that does not overwrite; keep it.
        shutil.copyfile(plain, optional)
    try:
        os.remove(plain)
        print("[gitignore] the core's .gitignore template is neutralised - "
              "it no longer overwrites a file that is already there")
    except OSError as exc:
        print("[gitignore] could not neutralise the core's template: %s" % exc)


def git_lines(args):
    """The lines of a file as git has it, or None."""
    try:
        result = subprocess.run(
            ["git"] + args,
            cwd=project_dir,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except Exception:  # noqa: BLE001  (a build must not stop here)
        return None
    if result.returncode != 0:
        return None
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}


def template_lines():
    """The lines the core's template holds, or None."""
    directory = core_templates_dir()
    if not directory:
        return None
    for name in (".gitignore.optional.tpl", ".gitignore.tpl"):
        template = os.path.join(directory, name)
        if not os.path.isfile(template):
            continue
        try:
            with open(template, "r", encoding="utf-8", errors="replace") as f:
                return {line.strip() for line in f if line.strip()}
        except OSError:
            return None
    return None


neutralise_the_template()

tracked = git_lines(["show", "HEAD:.gitignore"])
working = None
if os.path.isfile(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            working = {line.strip() for line in f if line.strip()}
    except OSError:
        working = None

template = template_lines()

if template and working == template:
    # The file is the core's template, word for word: the IDE wrote it,
    # and the repository's goes back.
    try:
        result = subprocess.run(
            ["git", "checkout", "--", ".gitignore"],
            cwd=project_dir,
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode == 0:
            print("[gitignore] the IDE had taken over the .gitignore - "
                  "the repository's is back")
        else:
            print("[gitignore] git did not give the .gitignore back - "
                  "leaving it alone (%s)"
                  % (result.stderr or "").strip().splitlines()[:1])
    except Exception as exc:  # noqa: BLE001  (a build must not stop here)
        print("[gitignore] could not ask git for the .gitignore: %s" % exc)
