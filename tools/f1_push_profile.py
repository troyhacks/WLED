"""Push the shipped HIDCTL profile to the board.

The profile is a file on the board's filesystem, so changing it does not
need a rebuild: extract the JSON out of the firmware header and upload it
over the /upload endpoint. The board reads the profiles at boot, so a
restart is needed afterwards (plugging the controller out and in also
re-reads them).

Usage:
    python tools/f1_push_profile.py [http://192.168.6.249]
"""
import re
import sys

import requests

HEADER = "usermods/usermod_v3_hidctl/hidctl_default_profile.h"
TARGET = "/hidctl-traktor-kontrol-f1.json"


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "http://192.168.6.249"
    with open(HEADER, encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(r'R"JSON\((.*?)\)JSON"', text, re.S)
    if not m:
        sys.exit(f"no profile JSON found in {HEADER}")
    profile = m.group(1)

    files = {"upload": (TARGET.split("/")[-1], profile.encode("utf-8"),
                        "application/json")}
    r = requests.post(f"{base}/upload", files=files, timeout=30)
    print(f"POST {base}/upload -> {r.status_code}: {r.text[:200]}")


if __name__ == "__main__":
    main()
