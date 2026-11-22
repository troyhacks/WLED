"""One session with the Soomfon CN003: push images, read the keys.

The deck disappears from the HID enumeration while it is open
exclusively, so reading and writing have to share ONE handle - which is
why this does both.

    python tools/deck_session.py --read 60          # watch the keys
    python tools/deck_session.py --push             # the test pattern
    python tools/deck_session.py --push --read 60   # both

The protocol is ported from the reference: `ambiso/mirajazz` (the crate
opendeck-akp05 links against) and `ambiso/opendeck-akp05` for the
device-specific parts.
"""
import argparse
import math
import ctypes
import ctypes.wintypes as w
import math
import sys
import time

VID, PID = 0x1500, 0x3002
USAGE_PAGE = 0xFFA0          # the vendor interface; the other is a keyboard

WIRE_LEN = 1025              # every packet is padded to this
PACKET_SIZE = 1024           # the image chunk, without the leading byte
IN_BUF = 1024                # what the Windows HID driver wants to read
KEY_W, KEY_H = 112, 112      # the key images
SLOT_TOP_FIRST, SLOT_BOTTOM_FIRST = 11, 6

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t),
                ("InternalHigh", ctypes.c_size_t),
                ("Offset", w.DWORD),
                ("OffsetHigh", w.DWORD),
                ("hEvent", w.HANDLE)]


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
                      ctypes.POINTER(w.DWORD), ctypes.POINTER(OVERLAPPED)]
GetOverlappedResult = k32.GetOverlappedResult
GetOverlappedResult.restype = w.BOOL
GetOverlappedResult.argtypes = [w.HANDLE, ctypes.POINTER(OVERLAPPED),
                                 ctypes.POINTER(w.DWORD), ctypes.c_void_p,
                                 w.BOOL]
WaitForSingleObject = k32.WaitForSingleObject
WaitForSingleObject.restype = w.DWORD
WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
CreateEvent = k32.CreateEventW
CreateEvent.restype = w.HANDLE
CreateEvent.argtypes = [ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
ResetEvent = k32.ResetEvent
ResetEvent.argtypes = [w.HANDLE]

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
WAIT_TIMEOUT = 0x102

# The message IDs, from opendeck-akp05/src/inputs.rs.
ENCODER_TWIST = {0xa0: (0, -1), 0xa1: (0, 1), 0x50: (1, -1), 0x51: (1, 1),
                 0x90: (2, -1), 0x91: (2, 1), 0x70: (3, -1), 0x71: (3, 1)}
ENCODER_PUSH = {0x37: 0, 0x35: 1, 0x33: 2, 0x36: 3}
PLAIN = {0x00: 11, 0x40: 12, 0x41: 13, 0x42: 14, 0x43: 15}


def describe(msg, state):
    if msg in ENCODER_TWIST:
        enc, delta = ENCODER_TWIST[msg]
        return f'encoder {enc} twist {delta:+d}'
    if msg in ENCODER_PUSH:
        return f'encoder {ENCODER_PUSH[msg]} push'
    if msg in PLAIN:
        return f'plain button {PLAIN[msg]}'
    if 1 <= msg <= 10:
        return f'key {msg} ({"pressed" if state else "released"})'
    return f'unknown message 0x{msg:02X}'


def hdr(mnem, tail=b''):
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
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if self.h in (-1, 0xFFFFFFFFFFFFFFFF):
            raise SystemExit(f"cannot open the deck: {ctypes.get_last_error()}")
        self.event = CreateEvent(None, True, False, None)
        self.buf = (ctypes.c_ubyte * IN_BUF)()
        self.ov = OVERLAPPED()
        self.ov.hEvent = self.event
        print(f"opened {path}")

    # -- the wire ---------------------------------------------------------

    def reset_ov(self):
        """A fresh OVERLAPPED for each operation, keeping the event: a
        structure that still says 'pending' makes the driver refuse the
        next one."""
        event = self.ov.hEvent
        ctypes.memset(ctypes.byref(self.ov), 0, ctypes.sizeof(OVERLAPPED))
        self.ov.hEvent = event

    def send(self, body, length=WIRE_LEN):
        """One packet. The handle is opened for overlapped I/O, so the write
        has to be overlapped too - a plain WriteFile on it is refused."""
        pkt = (ctypes.c_ubyte * length)()
        ctypes.memmove(pkt, body, min(len(body), length))
        n = w.DWORD(0)
        self.reset_ov()
        ResetEvent(self.event)
        if not WriteFile(self.h, pkt, length, ctypes.byref(n), ctypes.byref(self.ov)):
            e = ctypes.get_last_error()
            if e != ERROR_IO_PENDING:
                raise SystemExit(f"write failed: win32 error {e}")
            WaitForSingleObject(self.event, 2000)
            if not GetOverlappedResult(self.h, ctypes.byref(self.ov),
                                       ctypes.byref(n), None, True):
                raise SystemExit(f"write did not complete: {ctypes.get_last_error()}")
        time.sleep(0.02)

    # -- the commands -----------------------------------------------------

    def dis(self):
        self.send(hdr(b'DIS'))

    def lig(self, percent):
        self.send(hdr(b'LIG', bytes([0x00, 0x00, min(100, percent)])))

    def cle(self, key=0xFF):
        self.send(hdr(b'CLE', bytes([0x00, 0x00, 0x00,
                                     0xFF if key == 0xFF else key + 1])))

    def stp(self):
        self.send(hdr(b'STP'))

    def connect(self):
        self.send(hdr(b'CONNECT'))

    def han(self):
        self.send(hdr(b'HAN'))

    def bgpic(self, jpeg, w, h, x=0, y=0):
        """One picture across the whole wide strip: "BGPIC" with the
        length, the position and the size, then the JPEG in chunks, then
        STP. The reference calls this experimental - the CN003 is not one
        of the devices it ships for."""
        head = bytes([0x00, 0x43, 0x52, 0x54, 0x00, 0x00,
                      0x42, 0x47, 0x50, 0x49, 0x43])          # "BGPIC"
        head += bytes([(len(jpeg) >> 24) & 0xFF, (len(jpeg) >> 16) & 0xFF,
                        (len(jpeg) >> 8) & 0xFF, len(jpeg) & 0xFF])
        head += bytes([(x >> 8) & 0xFF, x & 0xFF, (y >> 8) & 0xFF, y & 0xFF,
                        (w >> 8) & 0xFF, w & 0xFF, (h >> 8) & 0xFF, h & 0xFF])
        head += bytes([0x00, 0x00])
        self.send(head)
        for off in range(0, len(jpeg), PACKET_SIZE):
            chunk = jpeg[off:off + PACKET_SIZE]
            self.send(bytes([0x00]) + chunk + bytes(WIRE_LEN - 1 - len(chunk)),
                      WIRE_LEN)
        self.stp()

    def image(self, slot, jpeg):
        """BAT, the image in 1024-byte chunks, then STP."""
        self.send(hdr(b'BAT', bytes([0x00, 0x00,
                                     (len(jpeg) >> 8) & 0xFF, len(jpeg) & 0xFF,
                                     slot])))
        for off in range(0, len(jpeg), PACKET_SIZE):
            chunk = jpeg[off:off + PACKET_SIZE]
            self.send(bytes([0x00]) + chunk + bytes(WIRE_LEN - 1 - len(chunk)),
                      WIRE_LEN)
        self.stp()

    # -- the input --------------------------------------------------------

    def poll(self, timeout_ms=200):
        """One report, or None if nothing arrived."""
        self.reset_ov()
        ResetEvent(self.event)
        n = w.DWORD(0)
        if not ReadFile(self.h, ctypes.byref(self.buf), IN_BUF,
                        ctypes.byref(n), ctypes.byref(self.ov)):
            e = ctypes.get_last_error()
            if e != ERROR_IO_PENDING:
                print(f"  read failed: win32 error {e}")
                return None
            if WaitForSingleObject(self.event, timeout_ms) == WAIT_TIMEOUT:
                return None
            if not GetOverlappedResult(self.h, ctypes.byref(self.ov),
                                        ctypes.byref(n), None, True):
                print(f"  overlapped result failed: {ctypes.get_last_error()}")
                return None
        return bytes(self.buf[:n.value])


def key_image(text, colour):
    """A key image: the size the device wants, JPEG, rotated 180 degrees."""
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new('RGB', (KEY_W, KEY_H), colour)
    d = ImageDraw.Draw(img)
    if text:
        try:
            font = ImageFont.truetype('arial.ttf', int(KEY_H * 0.55))
        except OSError:
            font = ImageFont.load_default()
        box = d.textbbox((0, 0), text, font=font)
        d.text(((KEY_W - box[2] + box[0]) / 2, (KEY_H - box[3] + box[1]) / 2),
               text, fill=(255, 255, 255), font=font)
    img = img.rotate(180)          # the device's images are upside down
    import io
    buf = io.BytesIO()
    img.save(buf, format='JPEG', quality=90)
    return buf.getvalue()


def strip_image(colour, size=(176, 112), text=None):
    """An image for one of the touch strip's zones."""
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new('RGB', size, colour)
    d = ImageDraw.Draw(img)
    if text:
        try:
            font = ImageFont.truetype('arial.ttf', int(size[1] * 0.45))
        except OSError:
            font = ImageFont.load_default()
        box = d.textbbox((0, 0), text, font=font)
        d.text(((size[0] - box[2] + box[0]) / 2, (size[1] - box[3] + box[1]) / 2),
               text, fill=(255, 255, 255), font=font)
    img = img.rotate(180)
    import io
    buf = io.BytesIO()
    img.save(buf, format='JPEG', quality=90)
    return buf.getvalue()


def push_strip(deck, bgpic=False):
    """The wide touch strip: four zone images, or one picture across it."""
    if bgpic:
        print("pushing one picture across the whole strip (BGPIC)...")
        jpeg = strip_image((0, 90, 40), (800, 128), text="THE WIDE STRIP")
        deck.bgpic(jpeg, 800, 128)
        print(f"  BGPIC, {len(jpeg)} bytes")
        return
    print("pushing the four strip zones...")
    for i in range(4):
        colour = (int(127 * (1 + 0.9 * math.cos(math.pi * (i * 36) / 180))),
                  int(127 * (1 + 0.9 * math.cos(math.pi * (i * 36 + 120) / 180))),
                  int(127 * (1 + 0.9 * math.cos(math.pi * (i * 36 + 240) / 180))))
        jpeg = strip_image(colour, text=f"ENC {i + 1}")
        deck.image(i + 1, jpeg)          # the strip zones are slots 1..4
        print(f"  zone {i + 1} -> slot {i + 1} ({len(jpeg)} bytes)")
    deck.stp()


def push_pattern(deck, number=None):
    deck.dis()
    deck.lig(50)
    deck.cle(0xFF)
    deck.stp()
    time.sleep(0.3)
    for i in range(10):
        slot = (SLOT_TOP_FIRST if i < 5 else SLOT_BOTTOM_FIRST) + (i % 5)
        if number is not None:
            jpeg = key_image(str(number), (0, 40, 90))
        else:
            hue = i * 36
            r = int(127 * (1 + 0.9 * math.cos(math.pi * hue / 180)))
            g = int(127 * (1 + 0.9 * math.cos(math.pi * (hue + 120) / 180)))
            b = int(127 * (1 + 0.9 * math.cos(math.pi * (hue + 240) / 180)))
            jpeg = key_image(str(i + 1), (r, g, b))
        deck.image(slot, jpeg)
        print(f"  key {i + 1} -> slot {slot} ({len(jpeg)} bytes)")
    deck.stp()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--read', type=float, default=0.0, help='read for N seconds')
    ap.add_argument('--push', action='store_true', help='push the test pattern')
    ap.add_argument('--number', type=int, default=None)
    ap.add_argument('--strip', action='store_true',
                    help='push the four touch strip zone images')
    ap.add_argument('--bgpic', action='store_true',
                    help='push one picture across the whole strip (BGPIC)')
    args = ap.parse_args()

    deck = Deck()
    if args.push:
        print("pushing...")
        push_pattern(deck, args.number)
    if args.strip:
        push_strip(deck)

    if args.read:
        print(f"reading for {args.read}s - press the keys, turn the encoders")
        end = time.time() + args.read
        last_keepalive = time.time()
        while time.time() < end:
            data = deck.poll()
            if data:
                # The reference reads the report with "ACK" at byte 0 and the
                # message ID at byte 9. Windows' HID driver hands us the
                # report with a leading zero, so everything is one byte
                # further along - find the "ACK" and work from there.
                ack = 0 if data[:3] == b'ACK' else (1 if data[1:4] == b'ACK' else -1)
                if ack >= 0 and len(data) > ack + 10:
                    msg, state = data[ack + 9], data[ack + 10]
                    print(f"  ACK  msg=0x{msg:02X} state=0x{state:02X} -> "
                          f"{describe(msg, state)}   body={data[ack + 3:ack + 12].hex()}")
                else:
                    print(f"  {len(data)} bytes (not an ACK report) {data[:16].hex()}")
            now = time.time()
            if now - last_keepalive >= 9:
                deck.connect()          # the device drops the session without it
                last_keepalive = now


if __name__ == '__main__':
    main()
