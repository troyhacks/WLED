"""
enable_usermod.py - switch a usermod on (or off) in the board's config.

A usermod's "enabled" only takes effect at boot, so this fetches the
board's cfg.json, changes the one switch, and posts it back - and the
board restarts itself.

    python tools/enable_usermod.py Ableton_Link
    python tools/enable_usermod.py Denon_StageLinq --off
"""

import argparse
import json
import urllib.request

BOARD = "http://192.168.6.249"


def post_cfg(host, text):
    import uuid
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
    ap.add_argument("usermod")
    ap.add_argument("--host", default=BOARD)
    ap.add_argument("--off", action="store_true", help="turn it OFF instead")
    args = ap.parse_args()

    with urllib.request.urlopen(args.host + "/cfg.json", timeout=15) as r:
        cfg = json.loads(r.read().decode())

    um = cfg.setdefault("um", {})
    section = um.setdefault(args.usermod, {})
    section["enabled"] = not args.off
    print("%s.enabled -> %s" % (args.usermod, section["enabled"]))

    status, reply = post_cfg(args.host, json.dumps(cfg))
    print("POST /upload ->", status, reply.strip())
    print("the board is rebooting; give it 40-60 s")


if __name__ == "__main__":
    main()
