#!/usr/bin/env python
"""Put one file into the index with somebody else's changes taken out.

The tree is shared with another session and they are staging their own
work at the same time, so `git add` and `git apply --cached` keep
picking up their hunks.  This takes the working tree's copy of a file,
removes the blocks that are not ours, and puts the result into the index
directly - the working tree is not touched, so their work stays where it
is.

    python tools/git_stage_ours.py <file> <block-file> [<block-file> ...]

Each block file holds the exact lines to remove (they must match the
working tree byte for byte).
"""

import subprocess
import sys

path = sys.argv[1]
blocks = []
for bf in sys.argv[2:]:
    with open(bf, "rb") as f:
        blocks.append(f.read())

with open(path, "rb") as f:
    data = f.read()

for b in blocks:
    if b not in data:
        sys.exit("block not found in %s:\n%r" % (path, b[:200]))
    data = data.replace(b, b"")

tmp = ".git/stage_ours_tmp"
with open(tmp, "wb") as f:
    f.write(data)
sha = subprocess.run(["git", "hash-object", "-w", tmp],
                     capture_output=True, text=True).stdout.strip()
subprocess.run(["git", "update-index", "--cacheinfo", "100644," + sha + "," + path],
               check=True)
print("staged %s as %s" % (path, sha[:10]))
