"""
enable_stagelinq.py - switch the StageLinq usermod on in the board's config.

The usermod is off by default, and a usermod's "enabled" only takes
effect at boot, so this fetches the board's cfg.json, flips the one
switch, and posts it back.  The board restarts itself.

    python tools/enable_stagelinq.py [--host 192.168.6.249] [--disable]
"""

import argparse
import json
import urllib.request
import uuid

BOARD = "http://192.168.6.249"


def get(url):
    with urllib.request.urlopen(url, timeout=15) as r:
        return r.read()


def post_cfg(host, text):
    """POST a file named cfg.json to /upload - the board stores it and reboots."""
    boundary = "----wled" + uuid.uuid4().hex
    body = (
        ("--%s\r\n" % boundary).encode()
        + b'Content-Disposition: form-data; name="upload"; filename="cfg.json"\r\n'
        + b"Content-Type: application/json\r\n\r\n"
        + text.encode()
        + ("\r\n--%s--\r\n" % boundary).encode()
    )
    req = urllib.request.Request(
        host + "/upload", data=body,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status, r.read().decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=BOARD)
    ap.add_argument("--disable", action="store_true", help="turn it OFF instead")
    args = ap.parse_args()

    cfg = json.loads(get(args.host + "/cfg.json").decode())
    um = cfg.setdefault("um", {})
    sl = um.setdefault("Denon_StageLinq", {})
    sl["enabled"] = not args.disable
    print("Denon_StageLinq.enabled ->", sl["enabled"])

    text = json.dumps(cfg)
    status, reply = post_cfg(args.host, text)
    print("POST /upload ->", status, reply.strip())
    print("the board is rebooting; give it 40-60 s")


if __name__ == "__main__":
    main()
