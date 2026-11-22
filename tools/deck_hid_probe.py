"""What can the Windows HID stack actually do with the CN003?

The HID class driver parses the deck's reports as nothing at all (its
caps are zero), so hidapi's write fails and its read errors out. This
asks the driver directly, with the error codes, so we know whether the
device is usable from Windows at all or whether it needs a raw USB stack.

    python tools/deck_hid_probe.py
"""
import ctypes
import ctypes.wintypes as w
import hid

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
hidapi = ctypes.WinDLL('hid')

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
HidD_GetFeature = hidapi.HidD_GetFeature
HidD_GetFeature.restype = w.BOOL
HidD_GetFeature.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD]
HidD_SetFeature = hidapi.HidD_SetFeature
HidD_SetFeature.restype = w.BOOL
HidD_SetFeature.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD]

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3


def err(name):
    e = ctypes.get_last_error()
    print(f"    {name}: FAILED, win32 error {e}")


def main():
    dev = None
    for d in hid.enumerate():
        if d.get('vendor_id') == 0x1500 and d.get('usage_page') == 0xFFA0:
            dev = d
            break
    if dev is None:
        raise SystemExit("the CN003 is not on this machine")
    path = dev['path'].decode('utf-8') if isinstance(dev['path'], bytes) else dev['path']
    h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
    if h in (-1, 0xFFFFFFFFFFFFFFFF):
        raise SystemExit(f"cannot open the device: {ctypes.get_last_error()}")

    # 1. The firmware version, which the reference reads as a FEATURE report
    #    whose first byte is 0x01.
    buf = (ctypes.c_ubyte * 64)()
    buf[0] = 0x01
    if HidD_GetFeature(h, ctypes.byref(buf), 64):
        print(f"  FEATURE report 1 -> {bytes(buf[:40])!r}")
    else:
        err("HidD_GetFeature(1)")

    # 2. The output report: the deck's 1025-byte "CRT" command.
    pkt = (ctypes.c_ubyte * 1025)()
    for i, b in enumerate([0x00, 0x43, 0x52, 0x54, 0x00, 0x00, 0x44, 0x49, 0x53]):
        pkt[i] = b   # DIS
    n = w.DWORD(0)
    if WriteFile(h, ctypes.byref(pkt), 1025, ctypes.byref(n), None):
        print(f"  WriteFile(1025 bytes: DIS) -> {n.value} bytes written")
    else:
        err("WriteFile(DIS, 1025)")

    # 3. The same command at the sizes the driver might accept.
    for size in (1024, 513, 64, 9):
        p2 = (ctypes.c_ubyte * size)()
        ctypes.memmove(p2, pkt, size)
        if WriteFile(h, p2, size, ctypes.byref(n), None):
            print(f"  WriteFile({size} bytes) -> {n.value} bytes written")
        else:
            print(f"  WriteFile({size} bytes): win32 error {ctypes.get_last_error()}")

    # 4. Can we read the input reports?
    rbuf = (ctypes.c_ubyte * 512)()
    if ReadFile(h, ctypes.byref(rbuf), 512, ctypes.byref(n), None):
        print(f"  ReadFile -> {n.value} bytes: {bytes(rbuf[:32]).hex()}")
    else:
        err("ReadFile(512)")

    k32.CloseHandle(h)


if __name__ == '__main__':
    main()
