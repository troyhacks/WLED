"""Does the CN003 keep a separate set of pictures for each page?

    python tools/deck_page.py [raw|plus30]

The device has a page: SDDevice holds _currentPage, starts it at 1, and
the host sets it with

    "CRT\\0\\0MOD" + 00 00 + <mode>

The Mirabox reference puts 0x30 + the page in that byte; the Soomfon
library puts the page itself. This tries both:

  raw     the byte is the page (1, 2)
  plus30  the byte is 0x30 + the page (0x31, 0x32)

The test:

  1. MOD page 1, then the ten keys' and the four tiles' pictures - green
     squares marked A
  2. MOD page 2, then the same fourteen again - blue squares marked B
  3. MOD page 1, and NOTHING ELSE IS SENT
  4. MOD page 2, and nothing else is sent either

If the deck goes back to the green A squares at step 3 it keeps a
picture set per page, and a page change is four bytes instead of
fourteen pictures.

The official Stream Controller app must be CLOSED while this runs.
"""
import io
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deck_session  # noqa: E402

from PIL import Image, ImageDraw  # noqa: E402

KEY_SLOTS = [6, 7, 8, 9, 10, 11, 12, 13, 14, 15]   # the ten keys
TILE_SLOTS = [1, 2, 3, 4]                          # the strip's four tiles
SIZE = (112, 112)
ENCODING = sys.argv[1] if len(sys.argv) > 1 else "raw"


def mode_byte(page):
    return (0x30 + page) if ENCODING == "plus30" else page


def mod_command(page):
    """The MOD command: "CRT" + 00 00 + "MOD" + 00 00 + the mode."""
    pkt = bytearray(b"CRT\x00\x00MOD" + b"\x00\x00" + bytes([mode_byte(page)]))
    return bytes(pkt) + bytes(1025 - len(pkt))


def picture(mark, colour):
    img = Image.new("RGB", SIZE, (0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([8, 8, SIZE[0] - 8, SIZE[1] - 8], fill=colour)
    d.text((48, 40), mark, fill=(0, 0, 0))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=90)
    return buf.getvalue()


def push(deck, slot, mark, colour):
    deck.image(slot, picture(mark, colour))


def main():
    deck = deck_session.Deck()
    deck.dis()
    time.sleep(2.0)
    deck.lig(80)
    time.sleep(0.5)
    print("the mode byte is %s (page 1 -> 0x%02x, page 2 -> 0x%02x)"
          % (ENCODING, mode_byte(1), mode_byte(2)))

    print("\n1. MOD page 1, then the A (green) pictures")
    deck.send(mod_command(1))
    time.sleep(0.5)
    for slot in KEY_SLOTS + TILE_SLOTS:
        push(deck, slot, "A", (0, 255, 0))
    time.sleep(2.0)
    print("   the green A squares should be on the deck")

    print("\n2. MOD page 2, then the B (blue) pictures")
    deck.send(mod_command(2))
    time.sleep(0.5)
    for slot in KEY_SLOTS + TILE_SLOTS:
        push(deck, slot, "B", (0, 128, 255))
    time.sleep(2.0)
    print("   the blue B squares should be on the deck")

    print("\n3. MOD page 1, and NOTHING else is sent")
    deck.send(mod_command(1))
    time.sleep(3.0)
    print("   green A means the deck keeps a picture set per page")

    print("\n4. MOD page 2, and nothing else is sent either")
    deck.send(mod_command(2))
    print("\nWatching for 25 s - only the CONNECT keepalive goes out.")
    end = time.time() + 25
    while time.time() < end:
        deck.connect()
        time.sleep(2.0)
    print("Done. Which set is on the deck now - green A or blue B?")


if __name__ == "__main__":
    main()
