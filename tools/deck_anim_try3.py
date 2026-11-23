"""Three ways of feeding the deck a frame sequence - one look tells us all.

    python tools/deck_anim_try3.py

  slot 12  BAT + data + STP for each frame   (what the official app does,
           and what our first test did: it plays once and holds)
  slot 13  BAT + data for each frame, and ONE STP at the end - maybe the
           deck needs the whole batch committed at once
  slot 14  the frames sent TWICE - if the deck's buffer rolls, the second
           pass plays again

Green, blue and red so the keys can be told apart.

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


def main():
    deck = deck_session.Deck()
    deck.dis()
    time.sleep(2.0)
    deck.lig(80)
    time.sleep(0.5)

    def frames(colour):
        return [frame_jpeg(i, FRAMES, colour) for i in range(FRAMES)]

    # 12: a commit after each frame - what the app does.
    green = frames((0, 255, 0))
    for j in green:
        deck.image(12, j)
        time.sleep(0.05)
    deck.stp()
    print("  slot 12: %d frames, a commit after each" % FRAMES)

    # 13: the frames with no commit between them, one commit at the end.
    blue = frames((0, 128, 255))
    for j in blue:
        deck.image(13, j, commit=False)
        time.sleep(0.05)
    deck.stp()
    print("  slot 13: %d frames, one commit at the end" % FRAMES)

    # 14: the frames sent twice.
    red = frames((255, 0, 0))
    for j in red:
        deck.image(14, j)
        time.sleep(0.05)
    deck.stp()
    print("  slot 14: the first pass")
    time.sleep(3.0)
    for j in red:
        deck.image(14, j)
        time.sleep(0.05)
    deck.stp()
    print("  slot 14: the second pass")

    print("\nWatching for 40 s - only the CONNECT keepalive goes out.")
    end = time.time() + 40
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)
    print("Done. Which keys played more than one pass?")


if __name__ == "__main__":
    main()
