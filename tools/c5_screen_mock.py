"""Draw the C5 remote's three screens, as they will look on the panel.

This is a MOCK-UP: it uses the real console font (the same one the panel
uses, read out of wled00/src/font/console_font_6x8.h) and the same layout,
but it draws on the host.  It is how the screen was checked before the
board would boot again.

Usage:  python tools/c5_screen_mock.py [outdir]
"""

import io
import os
import re
import sys
import zlib

W = H = 320

# The font: the same file the panel uses.
FONT_H = 8
FONT_W = 6
ADVANCE = FONT_W + 1


def load_font(path):
    """Read the font out of the header the panel uses.

    The values are on lines like "    0x10,  /* 000100 */" - the value is
    before the comment, and the comment holds the same number in binary.
    """
    text = io.open(path, encoding="utf-8", errors="ignore").read()
    body = text[text.index("{") + 1: text.rindex("}")]
    # The comments hold numbers of their own (the ASCII art and the code
    # numbers), so they go first.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    vals = []
    for line in body.splitlines():
        code = line.split("//")[0]
        for m in re.finditer(r"0x[0-9A-Fa-f]+|\b\d+\b", code):
            vals.append(int(m.group(0), 0))
    return vals


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


class Canvas:
    def __init__(self, font):
        self.px = [[0] * W for _ in range(H)]
        self.font = font

    def fill_rect(self, x, y, w, h, color):
        for yy in range(max(0, y), min(H, y + h)):
            row = self.px[yy]
            for xx in range(max(0, x), min(W, x + w)):
                row[xx] = color

    def text(self, x, y, s, fg, bg):
        for i, ch in enumerate(s):
            code = ord(ch)
            if code < 32 or code > 126:
                code = 63
            gx = x + i * ADVANCE
            for row in range(FONT_H):
                bits = self.font[(code - 32) * FONT_H + row]
                for col in range(FONT_W):
                    color = fg if (bits >> (7 - col)) & 1 else bg
                    xx, yy = gx + col, y + row
                    if 0 <= xx < W and 0 <= yy < H:
                        self.px[yy][xx] = color

    def text_right(self, x_right, y, s, fg, bg):
        self.text(x_right - len(s) * ADVANCE + 1, y, s, fg, bg)

    def png(self, path):
        raw = b""
        for y in range(H):
            raw += b"\x00"
            for x in range(W):
                c = self.px[y][x]
                raw += bytes((((c >> 11) & 0x1F) << 3, (c >> 2) & 0xFC, (c << 3) & 0xF8))
        def chunk(tag, payload):
            return (len(payload).to_bytes(4, "big") + tag + payload +
                    (zlib.crc32(tag + payload) & 0xFFFFFFFF).to_bytes(4, "big"))
        out = b"\x89PNG\r\n\x1a\n"
        out += chunk(b"IHDR", W.to_bytes(4, "big") + H.to_bytes(4, "big") + b"\x08\x02\x00\x00\x00")
        out += chunk(b"IDAT", zlib.compress(raw, 6))
        out += chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(out)


BLACK = 0x0000
WHITE = 0xFFFF
DIM = 0x8410
ACCENT = 0x07FF
GOOD = 0x07E0
BAD = 0xF800
TITLE_BG = 0x01E0
FOOT_BG = 0x1082
PANEL_BG = 0x0861


def draw_sync(c, font, senders):
    c.fill_rect(0, 0, W, 14, TITLE_BG)
    c.text(2, 3, "SYNC live", ACCENT, TITLE_BG)
    y = 18
    for (name, ip, packets, rate, age, bands) in senders:
        c.text(2, y, name, WHITE, BLACK)
        c.text_right(W - 2, y, "%d/s  %dms ago" % (rate, age), GOOD if age < 2500 else BAD, BLACK)
        y += 10
        c.text(2, y, "%s  %d pkts" % (ip, packets), DIM, BLACK)
        y += 12
    if not senders:
        c.text(2, y, "waiting for a board to send", DIM, BLACK)
        c.text(2, y + 12, "the audio sync is on 239.0.0.1:11988", DIM, BLACK)
    meter_top = H - 12 - 96
    c.fill_rect(0, meter_top - 2, W, 98, PANEL_BG)
    if senders:
        bands = senders[-1][5]
        bw = (W - 8) // 16
        for i, v in enumerate(bands):
            bh = v * 92 // 255
            col = rgb565(255 - v // 3, v, 40)
            c.fill_rect(4 + i * bw, meter_top + 92 - bh, bw - 1, bh, col)
        c.text_right(W - 4, meter_top + 2, "level %d" % senders[-1][4], DIM, PANEL_BG)
    c.fill_rect(0, H - 12, W, 12, FOOT_BG)
    c.text(2, H - 10, "< prev   next >", DIM, FOOT_BG)
    c.text_right(W - 2, H - 10, "beats >", DIM, FOOT_BG)


def draw_beat(c, bpm, beat, quality, drift, hold, downbeat, mode, preset, bri):
    c.fill_rect(0, 0, W, 14, TITLE_BG)
    c.text(2, 3, "BEAT", ACCENT, TITLE_BG)
    c.text_right(W - 2, 3, "LOCKED" if hold else "free", WHITE, TITLE_BG)
    c.text(2, 20, "%.1f BPM" % bpm, GOOD, BLACK)
    c.text(2, 34, "beat %d of 4" % beat, WHITE, BLACK)
    c.text(2, 44, "quality %.2f   drift %+.0f ms" % (quality, drift), DIM, BLACK)
    c.fill_rect(2, 58, W - 4, 16, PANEL_BG)
    c.fill_rect(2, 58, int((W - 4) * 0.42), 16, ACCENT)
    c.text(2, 80, "mode: %s" % mode, DIM, BLACK)
    c.text(2, 90, "downbeat: marked" if downbeat else "downbeat: not marked",
           GOOD if downbeat else DIM, BLACK)
    c.text(2, 104, "preset %d   bri %d" % (preset, bri), DIM, BLACK)
    c.fill_rect(0, H - 12, W, 12, FOOT_BG)
    c.text(2, H - 10, "BOOT: downbeat", DIM, FOOT_BG)
    c.text_right(W - 2, H - 10, "hold: lock", DIM, FOOT_BG)


def draw_board(c, ssid, ip, uptime, heap, acc, gyr, battery, host, online, action):
    c.fill_rect(0, 0, W, 14, TITLE_BG)
    c.text(2, 3, "BOARD", ACCENT, TITLE_BG)
    c.text_right(W - 2, 3, "linked" if online else "alone", WHITE, TITLE_BG)
    c.text(2, 20, "wifi:", DIM, BLACK)
    c.text(38, 20, ssid, WHITE, BLACK)
    c.text(2, 30, "ip:", DIM, BLACK)
    c.text(38, 30, ip, WHITE, BLACK)
    c.text(2, 42, "up %d:%02d   heap %d" % (uptime // 60, uptime % 60, heap), DIM, BLACK)
    c.text(2, 56, "acc %+.2f %+.2f %+.2f" % acc, DIM, BLACK)
    c.text(2, 66, "gyr %+.2f %+.2f %+.2f" % gyr, DIM, BLACK)
    c.text(2, 80, "battery %d" % battery, DIM, BLACK)
    c.text(2, 94, "board:", DIM, BLACK)
    c.text(38, 94, host, WHITE if online else DIM, BLACK)
    if action:
        c.text(2, 110, action, ACCENT, BLACK)
    c.fill_rect(0, H - 12, W, 12, FOOT_BG)
    c.text(2, H - 10, "< prev   next >", DIM, FOOT_BG)


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    here = os.path.dirname(os.path.abspath(__file__))
    font = load_font(os.path.join(here, "..", "wled00", "src", "font", "console_font_6x8.h"))
    print("font bytes:", len(font))

    c = Canvas(font)
    draw_sync(c, font, [
        ("WLED-PC", "192.168.6.232", 4807, 94, 7,
         [240, 36, 0, 0, 0, 146, 86, 114, 84, 107, 91, 141, 135, 108, 110, 40]),
        ("wled-aabbcc", "192.168.6.163", 5299, 94, 2, [0] * 16),
    ])
    c.png(os.path.join(outdir, "c5_screen_sync.png"))

    c = Canvas(font)
    draw_beat(c, 111.5, 1, 0.30, 218.6, False, True, "dj set", 3, 255)
    c.png(os.path.join(outdir, "c5_screen_beat.png"))

    c = Canvas(font)
    draw_board(c, "YOUR_NETWORK", "192.168.2.47", 412, 36428,
               (-0.48, 0.24, -8.75), (-0.02, 0.06, -0.01), 1870,
               "192.168.6.249", True, "downbeat set")
    c.png(os.path.join(outdir, "c5_screen_board.png"))
    print("wrote the three screens into", outdir)
