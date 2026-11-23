"""Can the host make the deck play the animation again?

    python tools/deck_anim_try6.py

  slot 12  the header and the frames, sent twice with a gap
  slot 13  the frames only, sent twice with a gap (the plain way)

If the deck replays on the second pass, a pulse is just "send the header
and the frames again" - and with the pictures already cached on the P4
that costs packets, not encoding.

The official Stream Controller app must be CLOSED while this runs.
"""
import io
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deck_session  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402

FRAMES = 7
SIZE = (112, 112)
GAP = 0.33


def frame_jpeg(n, total, colour):
    img = Image.new("RGB", SIZE, (0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = int(SIZE[0] * 0.45 * (n + 1) / total)
    d.rectangle([SIZE[0] // 2 - pad, SIZE[1] // 2 - pad,
                  SIZE[0] // 2 + pad, SIZE[1] // 2 + pad], fill=colour)
    d.text((4, 4), str(n + 1), fill=(255, 255, 255))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=90)
    return buf.getvalue()


def log_header(count, size):
    pkt = bytearray(b"CRT" + bytes([count]) + b"\xffLOG" + size.to_bytes(4, "big"))
    return bytes(pkt) + bytes(1025 - len(pkt))


def main():
    deck = deck_session.Deck()
    deck.dis()
    time.sleep(2.0)
    deck.lig(80)
    time.sleep(0.5)

    green = [frame_jpeg(i, FRAMES, (0, 255, 0)) for i in range(FRAMES)]
    blue = [frame_jpeg(i, FRAMES, (0, 128, 255)) for i in range(FRAMES)]
    total = sum(len(j) for j in green)

    def pass_(slot, js, header=None):
        if header is not None:
            deck.send(header)
            time.sleep(0.3)
        for j in js:
            deck.image(slot, j)
            time.sleep(GAP)

    print("\n--- slot 12 (green): the header and the frames, twice ---")
    pass_(12, green, log_header(FRAMES, total))
    print("  the first pass is away - a gap, then the second")
    time.sleep(4.0)
    pass_(12, green, log_header(FRAMES, total))
    print("  the second pass is away")

    print("\n--- slot 13 (blue): the frames only, twice ---")
    pass_(13, blue)
    print("  the first pass is away - a gap, then the second")
    time.sleep(4.0)
    pass_(13, blue)
    print("  the second pass is away")

    print("\nWatching for 20 s - only the CONNECT keepalive goes out.")
    end = time.time() + 20
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)
    print("Done. Which keys played TWICE?")


if __name__ == "__main__":
    main()
