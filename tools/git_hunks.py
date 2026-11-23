#!/usr/bin/env python
"""Keep only the named hunks of a unified diff.

The tree is shared with another session, and some files carry both our
changes.  This takes a diff, drops the hunks that are not ours, and prints
the rest - for `git apply --cached`.

    python tools/git_hunks.py <file> <hunk-number> [<hunk-number> ...]
"""

import subprocess
import sys

path = sys.argv[1]
keep = set(int(n) for n in sys.argv[2:])

diff = subprocess.run(["git", "diff", "--unified=3", "--", path],
                      capture_output=True).stdout
if not diff:
    sys.exit("no diff for " + path)

# The bytes are kept as they are: the repository has autocrlf on, and a
# newline translation here would make the patch not apply.
lines = diff.decode("utf-8", "surrogateescape").splitlines(keepends=True)
head = []
i = 0
while i < len(lines) and not lines[i].startswith("@@"):
    head.append(lines[i])
    i += 1

hunks = []
cur = None
for line in lines[i:]:
    if line.startswith("@@"):
        if cur is not None:
            hunks.append(cur)
        cur = [line]
    elif cur is not None:
        cur.append(line)
if cur is not None:
    hunks.append(cur)

out = list(head)
for n, h in enumerate(hunks, 1):
    if n in keep:
        out += h
    else:
        sys.stderr.write("dropping hunk %d of %s\n" % (n, path))

sys.stdout.buffer.write("".join(out).encode("utf-8", "surrogateescape"))
