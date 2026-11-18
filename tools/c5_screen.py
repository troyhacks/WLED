"""Fetch the C5 remote's screen and put it in a PNG.

The screen comes over HTTP in tiles of 40 rows (a whole picture in one
request is more than this board's Wi-Fi likes), and this puts the tiles
back together.

Usage:  python tools/c5_screen.py [ip] [out.png]
"""

import struct
import subprocess
import sys
import time
import urllib.request

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.6.160"
OUT = sys.argv[2] if len(sys.argv) > 2 else "c5screen.png"
TILE_ROWS = 40
W = H = 320


def wait_for_board(timeout=90):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen("http://%s/json/info" % IP, timeout=3).read()
            return True
        except Exception:
            time.sleep(2)
    return False


def fetch_tile(row):
    url = "http://%s/c5screen.bmp?row=%d" % (IP, row)
    data = urllib.request.urlopen(url, timeout=30).read()
    if len(data) < 54 or data[:2] != b"BM":
        raise SystemExit("tile %d is not a BMP (%d bytes)" % (row, len(data)))
    tw = struct.unpack_from("<i", data, 14)[0]
    th = struct.unpack_from("<i", data, 18)[0]
    rows = []
    for r in range(th):
        off = 54 + r * tw * 3
        rows.append(data[off:off + tw * 3])
    return rows


if not wait_for_board():
    raise SystemExit("the board never answered")

print("the board is up - fetching the screen")
rows = []
row = 0
while row < H:
    # A BMP stores its rows bottom up, so each tile is the other way up.
    rows.extend(reversed(fetch_tile(row)))
    row += TILE_ROWS
print("got %d rows" % len(rows))

# The picture is BGR in the file; the PNG wants RGB.
raw = b""
for r in rows:
    out = bytearray()
    for i in range(0, len(r), 3):
        out += bytes((r[i + 2], r[i + 1], r[i]))
    raw += b"\x00" + bytes(out)

import zlib


def chunk(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload +
            struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 6))
png += chunk(b"IEND", b"")
with open(OUT, "wb") as f:
    f.write(png)
print("wrote", OUT, len(png), "bytes")
