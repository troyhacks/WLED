"""Does the CN003 animate on its own, and does it loop?

    python tools/deck_anim.py [--slot 12] [--frames 7] [--fast] [--wait 30]

Sends a few frames to one slot and then stops sending pictures - only
the CONNECT keepalive goes on, so the session stays alive.

If the deck is still animating after the frames, it plays them itself.
If it animates for as long as the keepalive goes on, it loops them.

The official Stream Controller app must be CLOSED while this runs, or it
will overwrite the frames.
"""
import argparse
import io
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deck_session  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402


def frame_jpeg(n, total, size=(112, 112)):
    """One frame: a block that grows around the frame number."""
    img = Image.new("RGB", size, (0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = int(size[0] * 0.45 * (n + 1) / total)
    d.rectangle([size[0] // 2 - pad, size[1] // 2 - pad,
                  size[0] // 2 + pad, size[1] // 2 + pad],
                 fill=(0, 255, 0))
    d.text((4, 4), str(n + 1), fill=(255, 255, 255))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=90)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slot", type=int, default=12, help="the BAT slot")
    ap.add_argument("--frames", type=int, default=7, help="how many frames")
    ap.add_argument("--fast", action="store_true",
                    help="send all the frames with no gap between them")
    ap.add_argument("--wait", type=float, default=30.0,
                    help="how long to keep the session alive and watch")
    ap.add_argument("--no-dis", action="store_true",
                    help="do not send DIS (it resets the deck's display)")
    ap.add_argument("--header", action="store_true",
                    help="send the GIF header before the frames")
    args = ap.parse_args()

    deck = deck_session.Deck()
    if not args.no_dis:
        # DIS resets the deck's display, and it needs a moment to come
        # back up before anything is drawn on it.
        deck.dis()
        time.sleep(2.0)
    deck.lig(80)
    time.sleep(0.5)

    if args.header:
        # The GIF header the Soomfon library builds:
        #   "CRT" + <frame count> + 0xFF + "LOG" + <size, 4 bytes big-endian>
        # (sendGIFToJPGInfo(size, imageCount) in SDLibrary1.dll)
        total = 0
        frames = [frame_jpeg(i, args.frames) for i in range(args.frames)]
        total = sum(len(f) for f in frames)
        pkt = bytearray(b"CRT" + bytes([args.frames]) + b"\xffLOG" +
                        total.to_bytes(4, "big"))
        pkt = bytes(pkt) + bytes(1025 - len(pkt))
        deck.send(pkt)
        print("  header: %d frames, %d bytes total" % (args.frames, total))
        time.sleep(0.2)

    for i in range(args.frames):
        jpeg = frame_jpeg(i, args.frames)
        deck.image(args.slot, jpeg)
        print("  frame %d/%d  %d bytes -> slot %d" % (i + 1, args.frames, len(jpeg), args.slot))
        if not args.fast:
            time.sleep(0.05)
    deck.stp()
    print("\n%d frames sent to slot %d. No more pictures." % (args.frames, args.slot))
    print("Keeping the session alive with CONNECT for %.0f s." % args.wait)
    end = time.time() + args.wait
    while time.time() < end:
        deck.connect()          # the device drops the session without it
        time.sleep(2.0)
    print("Done. Is the deck still animating?")


if __name__ == "__main__":
    main()
