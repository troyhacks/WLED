"""Try the CN003's own picture-size header as the way to loop a sequence.

    python tools/deck_anim_try2.py

SDLibrary1.dll's sendPicSizeCommand(size, imageCount) builds a BAT
whose byte 13 holds the FRAME COUNT where a BAT normally holds the
key. So the deck may be told "an animation of N frames follows" with
that header, and it may loop them.

Two keys, two ways of announcing it:

  slot 12  a BAT whose byte 13 is the frame count, length = the total
           size of the frames that follow
  slot 13  the same, but the length is the size of ONE frame

The frames follow as ordinary BATs.

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


def pic_size_header(count, size):
    """The BAT sendPicSizeCommand builds: a BAT whose key byte is the
    frame count."""
    pkt = bytearray(b"CRT\x00\x00BAT" + b"\x00\x00" +
                    size.to_bytes(2, "big") + bytes([count]))
    return bytes(pkt) + bytes(1025 - len(pkt))


def send(deck, slot, colour, header):
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
    print("  slot %d: %d frames, %d bytes in all" % (slot, FRAMES, total))


def main():
    deck = deck_session.Deck()
    deck.dis()
    time.sleep(2.0)
    deck.lig(80)
    time.sleep(0.5)

    frames = [frame_jpeg(i, FRAMES, (0, 255, 0)) for i in range(FRAMES)]
    total = sum(len(f) for f in frames)
    one = total // FRAMES

    # The header with the total size, then the frames.
    send(deck, 12, (0, 255, 0), pic_size_header(FRAMES, total))
    # The header with the size of one frame, then the frames.
    send(deck, 13, (0, 128, 255), pic_size_header(FRAMES, one))

    print("\nWatching for 40 s - only the CONNECT keepalive goes out.")
    end = time.time() + 40
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)
    print("Done. Which keys are still animating?")


if __name__ == "__main__":
    main()
