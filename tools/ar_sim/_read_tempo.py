#!/usr/bin/env python3
"""Read the board's tempo state once it answers again after a flash.

Keeps the request count low: the board's async_tcp task wedges after about
sixty or seventy cumulative GETs.
"""

import json
import sys
import time
import urllib.request

URL = "http://192.168.6.249/json/ar"
KEYS = ("bpm", "valid", "conf", "quant", "locked", "beat_ms",
        "ref_offset", "ref_locked", "ref_slips", "cand_step")


def get():
    with urllib.request.urlopen(URL, timeout=10) as r:
        return json.loads(r.read().decode())


def main():
    tries = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    d = None
    for i in range(tries):
        try:
            d = get()
            break
        except Exception as e:
            print("  (%d) %s" % (i + 1, e), flush=True)
            time.sleep(5)
    if d is None:
        print("no answer")
        return 1
    t = d.get("tempo", d.get("AudioReactive", {}).get("tempo", {}))
    print("tempo keys:", sorted(t.keys()))
    for k in KEYS:
        if k in t:
            print("  %-10s %s" % (k, t[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
