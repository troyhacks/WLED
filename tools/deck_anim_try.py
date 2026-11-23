"""Try the ways of asking the CN003 to loop a frame sequence.

    python tools/deck_anim_try.py

Sends the same 7 frames to three keys, each with a different way of
telling the deck about them, so one look tells us which one works:

  slot 12  the frames on their own          (the baseline: it plays once)
  slot 13  a header with the frame count    "CRT" + count + 0xFF + "LOG" + size
  slot 14  a header with the frame count 0  (0 = loop forever)

The frames are different colours so the keys can be told apart: green for
the baseline, blue for the header, red for the forever header.

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
    """The header SDLibrary1.dll's sendGIFToJPGInfo builds."""
    pkt = bytearray(b"CRT" + bytes([count]) + b"\xffLOG" + size.to_bytes(4, "big"))
    return bytes(pkt) + bytes(1025 - len(pkt))


def send(deck, slot, colour, header=None):
    if header is not None:
        deck.send(header)
        print("  slot %d: header %s" % (slot, header[:16].hex(" ")))
        time.sleep(0.2)
    total = 0
    for i in range(FRAMES):
        jpeg = frame_jpeg(i, FRAMES, colour)
        total += len(jpeg)
        deck.image(slot, jpeg)
        time.sleep(0.05)
    deck.stp()
    print("  slot %d: %d frames, %d bytes" % (slot, FRAMES, total))
    return total


def main():
    deck = deck_session.Deck()
    deck.dis()
    time.sleep(2.0)
    deck.lig(80)
    time.sleep(0.5)

    # The baseline: the frames and nothing else.
    total = send(deck, 12, (0, 255, 0))

    # A header with the frame count.
    send(deck, 13, (0, 128, 255), log_header(FRAMES, total))

    # A header with 0 - "loop forever".
    send(deck, 14, (255, 0, 0), log_header(0, total))

    print("\nWatching for 40 s - only the CONNECT keepalive goes out.")
    end = time.time() + 40
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)
    print("Done. Which keys are still animating?")


if __name__ == "__main__":
    main()
