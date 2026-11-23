"""The two headers that might tell the deck "an animation of N frames".

    python tools/deck_anim_try4.py

  slot 12  the plain way: BAT + data + STP for each frame. This is what
           the official app does and it plays the sequence once.
  slot 13  sendPicSizeCommand's header first - a BAT whose byte 13 holds
           the FRAME COUNT where a key would go - and then the frames
  slot 14  sendGIFToJPGInfo's header first - "CRT" + count + 0xFF +
           "LOG" + the size - and then the frames

The frames are sent slowly (a third of a second apart) so a loop is
easy to see, and each key is given a watch window.

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
GAP = 0.33          # between the frames, so a loop is visible
WATCH = 9.0         # how long to let it play before the next key


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


def pic_size_header(count, size):
    """sendPicSizeCommand: a BAT whose key byte is the frame count."""
    pkt = bytearray(b"CRT\x00\x00BAT" + b"\x00\x00" +
                    size.to_bytes(2, "big") + bytes([count]))
    return bytes(pkt) + bytes(1025 - len(pkt))


def log_header(count, size):
    """sendGIFToJPGInfo: "CRT" + count + 0xFF + "LOG" + size (4 bytes)."""
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
    red = [frame_jpeg(i, FRAMES, (255, 0, 0)) for i in range(FRAMES)]

    def frames(deck, slot, js, header=None):
        if header is not None:
            deck.send(header)
            print("  slot %d: header %s" % (slot, header[:16].hex(" ")))
            time.sleep(0.3)
        for j in js:
            deck.image(slot, j)
            time.sleep(GAP)

    # 12: the plain way.
    print("\n--- slot 12 (green): BAT + data + STP per frame ---")
    frames(deck, 12, green)
    end = time.time() + WATCH
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)

    # 13: the picture-size header.
    total = sum(len(j) for j in blue)
    print("\n--- slot 13 (blue): the picture-size header, then the frames ---")
    frames(deck, 13, blue, pic_size_header(FRAMES, total))
    end = time.time() + WATCH
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)

    # 14: the LOG header.
    print("\n--- slot 14 (red): the LOG header, then the frames ---")
    frames(deck, 14, red, log_header(FRAMES, total))
    end = time.time() + WATCH
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)

    print("\nDone. Which keys kept playing after their last frame?")


if __name__ == "__main__":
    main()
