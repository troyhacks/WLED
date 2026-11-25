#!/usr/bin/env python
"""Stage a file into the index with some of its hunks REMOVED.

Some files carry both my changes and changes that were already in the tree
before I started (the pure-IDF port's TCM_IRAM_ATTR -> SPM_IRAM_ATTR rename,
the deck usermod's removal, the USER_PRINT -> DEBUG_PRINT quieting).  Those
belong to other work and must not end up in these commits.

This builds the index's content directly: the working copy, with the hunks
whose patterns are given taken back out.  The worktree is not touched.

Usage:  stage_hunks.py <file> <remove-pattern> [<remove-pattern> ...]
"""
import subprocess, sys, re, os

path = sys.argv[1]
patterns = sys.argv[2:]

diff = subprocess.run(["git", "diff", "--", path],
                      capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
if not diff:
    print("%s: no diff" % path)
    sys.exit(0)

lines = diff.split("\n")
i = 0
while i < len(lines) and not lines[i].startswith("@@"):
    i += 1

hunks = []
cur = None
for line in lines[i:]:
    if line.startswith("@@"):
        if cur:
            hunks.append(cur)
        cur = [line]
    elif cur is not None:
        cur.append(line)
if cur:
    hunks.append(cur)

# Universal newlines: the worktree has CRLF (core.autocrlf) and the diff
# does not, so the comparison has to be on the normalised text.  The blob is
# written with LF, which is what the repository stores.
with open(path, "r", encoding="utf-8") as f:
    content = f.read().split("\n")

removed = 0
for h in hunks:
    body = h[1:]
    if not any(re.search(p, "\n".join(body)) for p in patterns):
        continue
    # "\ No newline at end of file" is not a line of the file.
    body = [l for l in body if not l.startswith("\\")]
    new_lines = [l[1:] for l in body if l.startswith("+") or l.startswith(" ")]
    old_lines = [l[1:] for l in body if l.startswith("-") or l.startswith(" ")]
    # Find the hunk by its content, not by its line number: the numbers are
    # the worktree's, and the worktree's line endings need not match the
    # diff's.
    found = -1
    for at in range(len(content) - len(new_lines) + 1):
        if content[at:at + len(new_lines)] == new_lines:
            found = at
            break
    if found < 0:
        print("%s: could not locate a hunk in the worktree - skipping" % path)
        sys.exit(1)
    content[found:found + len(new_lines)] = old_lines
    removed += 1

# Write it to a file and hash that: subprocess' text mode would
# translate the newlines to CRLF on the way out, and the repository stores
# LF.
import tempfile
with tempfile.NamedTemporaryFile("w", suffix=".tmp", delete=False,
                                 encoding="utf-8", newline="") as tmp:
    tmp.write("\n".join(content))
    tmpname = tmp.name
blob = subprocess.run(["git", "hash-object", "-w", tmpname],
                      capture_output=True, text=True).stdout.strip()
os.unlink(tmpname)
if not blob:
    print("%s: hash-object failed" % path)
    sys.exit(1)
mode = subprocess.run(["git", "ls-files", "-s", path],
                      capture_output=True, text=True).stdout.split()[0]
subprocess.run(["git", "update-index", "--cacheinfo", "%s,%s,%s" % (mode, blob, path)], check=True)
print("%s: staged with %d hunk(s) removed" % (path, removed))
