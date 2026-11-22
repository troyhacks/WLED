"""Talk to the Soomfon CN003 from the laptop: put images on its keys.

The deck speaks the OpenDeck "CRT" protocol - every command is ASCII and
zero-padded to 1025 bytes - over the standard HID output report. Windows'
HID class driver takes that happily (exactly 1025 bytes, nothing else
works), so no extra driver is needed.

    python tools/deck_push.py                # a test pattern on all 10 keys
    python tools/deck_push.py --number 7     # the preset number on every key
    python tools/deck_push.py --read         # try to read the key presses

The protocol is ported from the reference: `ambiso/mirajazz` (the crate
opendeck-akp05 links against) and `ambiso/opendeck-akp05` for the
device-specific parts.
"""
import argparse
import ctypes
import ctypes.wintypes as w
import sys
import time

VID, PID = 0x1500, 0x3002
USAGE_PAGE = 0xFFA0          # the vendor interface; the keyboard one is 0x0001

# Every packet is zero-padded to this length. The device ignores a short one.
WIRE_LEN = 1025
PACKET_SIZE = 1024

# The key images: 112x112 JPEG, rotated 180 degrees.
KEY_W, KEY_H = 112, 112

# The BAT slots. 1-4 are the four encoders' zones on the touch strip, 5 is
# unused, 6-10 are the bottom row of keys and 11-15 the top row.
SLOT_TOP_FIRST, SLOT_BOTTOM_FIRST = 11, 6

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
CreateFile = k32.CreateFileW
CreateFile.restype = w.HANDLE
CreateFile.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                       w.DWORD, w.DWORD, w.HANDLE]
WriteFile = k32.WriteFile
WriteFile.restype = w.BOOL
WriteFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                       ctypes.POINTER(w.DWORD), ctypes.c_void_p]
ReadFile = k32.ReadFile
ReadFile.restype = w.BOOL
ReadFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                      ctypes.POINTER(w.DWORD), ctypes.c_void_p]
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3


def hdr(mnem: bytes, tail: bytes = b'') -> bytes:
    """The "CRT" header, the mnemonic and any payload."""
    return bytes([0x00, 0x43, 0x52, 0x54, 0x00, 0x00]) + mnem + tail


class Deck:
    def __init__(self):
        import hid
        dev = None
        for d in hid.enumerate():
            if (d.get('vendor_id') == VID and d.get('product_id') == PID
                    and d.get('usage_page') == USAGE_PAGE):
                dev = d
                break
        if dev is None:
            raise SystemExit("the CN003 is not on this machine")
        path = dev['path'].decode('utf-8') if isinstance(dev['path'], bytes) else dev['path']
        self.h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, None,
                            OPEN_EXISTING, 0, None)
        if self.h in (-1, 0xFFFFFFFFFFFFFFFF):
            raise SystemExit(f"cannot open the deck: {ctypes.get_last_error()}")
        print(f"opened {path}")

    def send(self, body: bytes) -> None:
        """One command, padded to the wire length."""
        pkt = (ctypes.c_ubyte * WIRE_LEN)()
        ctypes.memmove(pkt, body, min(len(body), WIRE_LEN))
        n = w.DWORD(0)
        if not WriteFile(self.h, pkt, WIRE_LEN, ctypes.byref(n), None):
            raise SystemExit(f"write failed: win32 error {ctypes.get_last_error()}")
        time.sleep(0.02)          # the device wants a moment between packets

    def send_raw(self, data: bytes) -> None:
        """Exactly this many bytes, with no padding (the image chunks)."""
        pkt = (ctypes.c_ubyte * len(data))()
        ctypes.memmove(pkt, data, len(data))
        n = w.DWORD(0)
        if not WriteFile(self.h, pkt, len(data), ctypes.byref(n), None):
            raise SystemExit(f"write failed: win32 error {ctypes.get_last_error()}")
        time.sleep(0.02)

    # -- the commands ------------------------------------------------------

    def dis(self):
        self.send(hdr(b'DIS'))

    def lig(self, percent: int):
        self.send(hdr(b'LIG', bytes([0x00, 0x00, min(100, percent)])))

    def cle(self, key=0xFF):
        # the slot is at wire offset 12, after three zero bytes
        self.send(hdr(b'CLE', bytes([0x00, 0x00, 0x00,
                                     0xFF if key == 0xFF else key + 1])))

    def stp(self):
        self.send(hdr(b'STP'))

    def connect(self):
        self.send(hdr(b'CONNECT'))

    def han(self):
        self.send(hdr(b'HAN'))

    def bat(self, slot: int, length: int):
        self.send(hdr(b'BAT', bytes([0x00, 0x00,
                                     (length >> 8) & 0xFF, length & 0xFF,
                                     slot])))

    def image(self, slot: int, jpeg: bytes):
        """BAT, the image in 1024-byte chunks, then STP."""
        self.bat(slot, len(jpeg))
        for off in range(0, len(jpeg), PACKET_SIZE):
            chunk = jpeg[off:off + PACKET_SIZE]
            self.send_raw(bytes([0x00]) + chunk + bytes(WIRE_LEN - 1 - len(chunk)))
        self.stp()

    def read_reports(self, seconds: float):
        """The input reports. Windows may not give us these at all."""
        buf = (ctypes.c_ubyte * 512)()
        n = w.DWORD(0)
        end = time.time() + seconds
        while time.time() < end:
            if not ReadFile(self.h, ctypes.byref(buf), 512, ctypes.byref(n), None):
                print(f"  read failed: win32 error {ctypes.get_last_error()}")
                return
            print(f"  {n.value} bytes: {bytes(buf[:32]).hex()}")


def make_image(text: str, colour, size=(KEY_W, KEY_H)):
    """A key image: the size the device wants, JPEG, rotated 180 degrees."""
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new('RGB', size, colour)
    d = ImageDraw.Draw(img)
    if text:
        try:
            font = ImageFont.truetype('arial.ttf', int(size[1] * 0.55))
        except OSError:
            font = ImageFont.load_default()
        # centred
        box = d.textbbox((0, 0), text, font=font)
        d.text(((size[0] - box[2] + box[0]) / 2,
                (size[1] - box[3] + box[1]) / 2), text, fill=(255, 255, 255), font=font)
    img = img.rotate(180)          # the device's images are upside down
    import io
    buf = io.BytesIO()
    img.save(buf, format='JPEG', quality=90)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--number', type=int, default=None,
                    help='print this number on every key')
    ap.add_argument('--read', action='store_true', help='try to read the reports')
    ap.add_argument('--keepalive', type=float, default=0.0,
                    help='send CONNECT every N seconds for this long')
    args = ap.parse_args()

    deck = Deck()
    print("DIS...")
    deck.dis()
    print("LIG 50...")
    deck.lig(50)
    print("clear all + STP...")
    deck.cle(0xFF)
    deck.stp()
    time.sleep(0.3)

    print("pushing the key images...")
    # the ten keys with displays: the top row is slots 11..15, the bottom 6..10
    for i in range(10):
        slot = (SLOT_TOP_FIRST if i < 5 else SLOT_BOTTOM_FIRST) + (i % 5)
        if args.number is not None:
            jpeg = make_image(str(args.number), (0, 40, 90))
        else:
            # a test pattern: a distinct colour and the key's own number
            hue = (i * 36) % 360
            r = int(127 * (1 + 0.9 * __import__('math').cos(3.14159 * hue / 180)))
            g = int(127 * (1 + 0.9 * __import__('math').cos(3.14159 * (hue + 120) / 180)))
            b = int(127 * (1 + 0.9 * __import__('math').cos(3.14159 * (hue + 240) / 180)))
            jpeg = make_image(str(i + 1), (r, g, b))
        deck.image(slot, jpeg)
        print(f"  key {i + 1} -> slot {slot} ({len(jpeg)} bytes)")
    deck.stp()

    if args.keepalive:
        print(f"keepalive for {args.keepalive}s...")
        end = time.time() + args.keepalive
        while time.time() < end:
            deck.connect()
            time.sleep(10)

    if args.read:
        print("reading...")
        deck.read_reports(5)


if __name__ == '__main__':
    main()
