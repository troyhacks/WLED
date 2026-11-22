"""Wait for a line matching a pattern in the board's console log.

    python tools/wait_console_event.py <logfile> <pattern> [count]

Prints the matching lines as they arrive and exits.  Used to watch for a
key press on the deck without spending any of the board's web server.
"""
import re
import sys
import time

log = sys.argv[1] if len(sys.argv) > 1 else "build_output/hidctl_console.log"
pattern = re.compile(sys.argv[2] if len(sys.argv) > 2 else r"HIDCTL\] (key|zone|enc)")
want = int(sys.argv[3]) if len(sys.argv) > 3 else 1


def matches():
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            return [ln.rstrip() for ln in f if pattern.search(ln)]
    except OSError:
        return []


seen = 0
deadline = time.time() + 900
while time.time() < deadline:
    lines = matches()
    if len(lines) > seen:
        for ln in lines[seen:]:
            print(ln, flush=True)
        seen = len(lines)
        if seen >= want:
            sys.exit(0)
    time.sleep(1.5)
print("timeout: no more matching lines", flush=True)
