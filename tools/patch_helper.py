"""Patch helper: apply edits to a file that has MIXED line endings.

The tree has CRLF in the parts that came from upstream and LF in the
parts that were added later, so a plain string match finds only half of
what is there.  This tries both.
"""
import io
import sys


def patch(path, edits):
    s = io.open(path, encoding="utf-8", newline="").read()
    for old, new in edits:
        if s.count(old) == 1:
            s = s.replace(old, new)
            continue
        alt = old.replace("\n", "\r\n")
        if s.count(alt) == 1:
            s = s.replace(alt, new.replace("\n", "\r\n"))
            continue
        # Mixed: try matching the first line and rebuilding.
        lines = s.split("\n")
        hits = [i for i, l in enumerate(lines) if l.strip() == old.split("\n")[0].strip()]
        raise SystemExit("NO MATCH (%d) for %r" % (s.count(old), old.split("\n")[0][:70]))
    io.open(path, "w", encoding="utf-8", newline="").write(s)
    print("patched %s (%d edits)" % (path, len(edits)))
