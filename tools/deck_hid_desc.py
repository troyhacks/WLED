"""Fetch the Soomfon CN003's HID report descriptor on Windows.

The deck's report descriptor is what settles the input report's size, its
report-ID prefix and its structure. The board could not get it (the device
stalls GET_DESCRIPTOR(Report)), but the deck is HID, so the Windows HID
class driver has it.

    python tools/deck_hid_desc.py
"""
import ctypes
import ctypes.wintypes as w
import hid

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
CreateFile = k32.CreateFileW
CreateFile.restype = w.HANDLE
CreateFile.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                       w.DWORD, w.DWORD, w.HANDLE]
DeviceIoControl = k32.DeviceIoControl
DeviceIoControl.restype = w.BOOL
DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD,
                            ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD),
                            ctypes.c_void_p]
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
IOCTL_HID_GET_REPORT_DESCRIPTOR = 0x000B0403
IOCTL_HID_GET_STRING = 0x000B0408


def hexdump(b, base=0):
    return '\n'.join(f"  {base + i:04X}: " + ' '.join(f'{x:02X}' for x in b[i:i + 16])
                     for i in range(0, len(b), 16))


def main():
    for d in hid.enumerate():
        if d.get('vendor_id') != 0x1500:
            continue
        path = d['path'].decode('utf-8') if isinstance(d['path'], bytes) else d['path']
        print(f"=== interface {d['interface_number']}  "
              f"usage_page=0x{d['usage_page']:04X} usage=0x{d['usage']:04X}")
        print(f"    {path}")
        h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, None,
                       OPEN_EXISTING, 0, None)
        if h in (-1, 0xFFFFFFFFFFFFFFFF):
            print(f"  read/write open failed, err={ctypes.get_last_error()}")
            h = CreateFile(path, GENERIC_READ, 0, None, OPEN_EXISTING, 0, None)
            if h in (-1, 0xFFFFFFFFFFFFFFFF):
                print(f"  read-only open failed, err={ctypes.get_last_error()}")
                continue
        buf = (ctypes.c_ubyte * 1024)()
        got = w.DWORD(0)
        if DeviceIoControl(h, IOCTL_HID_GET_REPORT_DESCRIPTOR, None, 0,
                           buf, 1024, ctypes.byref(got), None):
            print(f"  REPORT DESCRIPTOR ({got.value} bytes):")
            print(hexdump(bytes(buf[:got.value])))
        else:
            print(f"  GET_REPORT_DESCRIPTOR failed, err={ctypes.get_last_error()}")
        k32.CloseHandle(h)


if __name__ == '__main__':
    main()
