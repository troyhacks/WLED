"""Turn a BMP into a PNG, so that it can be looked at.

The C5 remote serves what its panel is showing as a BMP (/c5screen.bmp).
This reads that and writes a PNG beside it.

Usage:  python tools/bmp_to_png.py <in.bmp> [out.png]
"""

import struct
import sys
import zlib


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise SystemExit("not a BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bits = struct.unpack_from("<H", data, 28)[0]
    if bits != 16:
        raise SystemExit("only 16 bit BMPs are handled")
    flip = height > 0
    height = abs(height)
    rows = []
    for y in range(height):
        row = data[offset + y * width * 2: offset + (y + 1) * width * 2]
        rgb = bytearray()
        for x in range(width):
            b = row[x * 2]
            g = row[x * 2 + 1]
            r = row[x * 2 + 2]
            rgb += bytes((r, g, b))
        rows.append(bytes(rgb))
    if flip:
        rows.reverse()
    return width, height, rows


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".png"
    w, h, rows = read_bmp(src)
    write_png(dst, w, h, rows)
    print("wrote", dst, w, "x", h)
