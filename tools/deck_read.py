"""Read the CN003's input reports on the laptop.

Windows' HID class driver needs FILE_FLAG_OVERLAPPED for ReadFile - a
synchronous read on a HID handle is refused - so this does the overlapped
dance: an event, an OVERLAPPED structure, and GetOverlappedResult.

Each report that carries input starts with "ACK" (0x41 0x43 0x4B); byte 9
is the message ID and byte 10 the state.

    python tools/deck_read.py [seconds]
"""
import ctypes
import ctypes.wintypes as w
import sys
import time

VID, PID = 0x1500, 0x3002
USAGE_PAGE = 0xFFA0

class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t),
                ("InternalHigh", ctypes.c_size_t),
                ("Offset", w.DWORD),
                ("OffsetHigh", w.DWORD),
                ("hEvent", w.HANDLE)]


k32 = ctypes.WinDLL('kernel32', use_last_error=True)
CreateFile = k32.CreateFileW
CreateFile.restype = w.HANDLE
CreateFile.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                       w.DWORD, w.DWORD, w.HANDLE]
ReadFile = k32.ReadFile
ReadFile.restype = w.BOOL
ReadFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                      ctypes.POINTER(w.DWORD), ctypes.POINTER(OVERLAPPED)]
GetOverlappedResult = k32.GetOverlappedResult
GetOverlappedResult.restype = w.BOOL
GetOverlappedResult.argtypes = [w.HANDLE, ctypes.POINTER(OVERLAPPED),
                                 w.DWORD, ctypes.POINTER(w.DWORD),
                                 ctypes.c_void_p, w.BOOL]
WaitForSingleObject = k32.WaitForSingleObject
WaitForSingleObject.restype = w.DWORD
WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
CreateEvent = k32.CreateEventW
CreateEvent.restype = w.HANDLE
CreateEvent.argtypes = [ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
ResetEvent = k32.ResetEvent
ResetEvent.argtypes = [w.HANDLE]
CloseHandle = k32.CloseHandle

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 0x102
INFINITE = 0xFFFFFFFF

# The message IDs, from opendeck-akp05/src/inputs.rs.
KEYS = {i: f'key {i}' for i in range(1, 11)}
ENCODER_TWIST = {0xa0: (0, -1), 0xa1: (0, 1), 0x50: (1, -1), 0x51: (1, 1),
                 0x90: (2, -1), 0x91: (2, 1), 0x70: (3, -1), 0x71: (3, 1)}
ENCODER_PUSH = {0x37: 0, 0x35: 1, 0x33: 2, 0x36: 3}
PLAIN = {0x00: 11, 0x40: 12, 0x41: 13, 0x42: 14, 0x43: 15}


def describe(msg: int, state: int) -> str:
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


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
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

    h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING,
                   FILE_FLAG_OVERLAPPED, None)
    if h in (-1, 0xFFFFFFFFFFFFFFFF):
        raise SystemExit(f"cannot open the deck: {ctypes.get_last_error()}")
    event = CreateEvent(None, True, False, None)
    print(f"opened {path}; reading for {secs}s - press the keys")

    size = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    buf = (ctypes.c_ubyte * size)()
    ov = OVERLAPPED()
    ov.hEvent = event
    end = time.time() + secs
    while time.time() < end:
        ResetEvent(event)
        n = w.DWORD(0)
        if not ReadFile(h, ctypes.byref(buf), size, ctypes.byref(n), ctypes.byref(ov)):
            e = ctypes.get_last_error()
            if e == 997:            # ERROR_IO_PENDING
                r = WaitForSingleObject(event, 200)
                if r == WAIT_TIMEOUT:
                    continue
                if not GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), None, True):
                    print(f"  overlapped result failed: {ctypes.get_last_error()}")
                    continue
            else:
                msg = ctypes.create_unicode_buffer(256)
                k32.FormatMessageW(0x1000, None, e, 0, msg, 256, None)
                print(f"  read failed: win32 error {e} ({msg.value.strip()})")
                break
        data = bytes(buf[:n.value])
        if data[:3] == b'ACK':
            msg, state = data[9], data[10]
            print(f"  {n.value} bytes  ACK  msg=0x{msg:02X} state=0x{state:02X}  "
                  f"-> {describe(msg, state)}   body={data[3:12].hex()}")
        else:
            print(f"  {n.value} bytes  (not an ACK report) {data[:16].hex()}")
    CloseHandle(h)


if __name__ == '__main__':
    main()
