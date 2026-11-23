"""The LOG header's count byte is the loop count - try 0, and the sizes.

    python tools/deck_anim_try5.py

With "CRT" + count + 0xFF + "LOG" + size the deck played the frames
slowly, at its own pace - so it understands the header. The count byte
is where a loop count belongs, and a GIF's loop count of 0 means
"forever".

  slot 12  count 0, the size is the total of all the frames
  slot 13  count 7, the size is ONE frame
  slot 14  count 0, the size is ONE frame

The frames are sent slowly so a loop is easy to see, and each key gets
a watch window.

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
WATCH = 9.0


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

    total = sum(len(frame_jpeg(i, FRAMES, (0, 0, 0))) for i in range(FRAMES))
    one = total // FRAMES
    print("the frames are %d bytes in all, %d each" % (total, one))

    def run(slot, colour, count, size):
        js = [frame_jpeg(i, FRAMES, colour) for i in range(FRAMES)]
        deck.send(log_header(count, size))
        print("  slot %d: count=%d size=%d  header %s"
              % (slot, count, size, log_header(count, size)[:16].hex(" ")))
        time.sleep(0.3)
        for j in js:
            deck.image(slot, j)
            time.sleep(GAP)
        end = time.time() + WATCH
        while time.time() < end:
            deck.connect()
            time.sleep(2.0)

    print("\n--- slot 12 (green): count 0, the total size ---")
    run(12, (0, 255, 0), 0, total)
    print("\n--- slot 13 (blue): count 7, one frame's size ---")
    run(13, (0, 128, 255), FRAMES, one)
    print("\n--- slot 14 (red): count 0, one frame's size ---")
    run(14, (255, 0, 0), 0, one)

    print("\nDone. Which keys kept playing after their last frame?")


if __name__ == "__main__":
    main()
