"""The Soomfon CN003's HID report descriptor and report sizes, from Windows.

The HID class driver refuses IOCTL_HID_GET_REPORT_DESCRIPTOR for this
device, but the driver parses the descriptor at enumeration, so the
preparsed data it holds has it: HidD_GetPreparsedData() then
HidP_ReportDesc().

The caps also give the output report's size, which is what the failing
write was about.

    python tools/deck_hid_caps.py
"""
import ctypes
import ctypes.wintypes as w
import hid

hidapi = ctypes.WinDLL('hid')
user32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

CreateFile = k32.CreateFileW
CreateFile.restype = w.HANDLE
CreateFile.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                       w.DWORD, w.DWORD, w.HANDLE]
CloseHandle = k32.CloseHandle

HidD_GetPreparsedData = hidapi.HidD_GetPreparsedData
HidD_GetPreparsedData.restype = w.BOOL
HidD_GetPreparsedData.argtypes = [w.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
HidD_FreePreparsedData = hidapi.HidD_FreePreparsedData
HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
HidP_ReportDesc = hidapi.HidP_ReportDesc
HidP_ReportDesc.restype = w.ULONG
HidP_ReportDesc.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
HidP_GetCaps = hidapi.HidP_GetCaps
HidP_GetCaps.restype = w.LONG
HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", w.USHORT), ("UsagePage", w.USHORT),
        ("InputReportByteLength", w.USHORT), ("OutputReportByteLength", w.USHORT),
        ("FeatureReportByteLength", w.USHORT),
        ("Reserved", ctypes.c_ushort * 17),
        ("NumberLinkCollectionNodes", w.USHORT), ("NumberInputButtonCaps", w.USHORT),
        ("NumberInputValueCaps", w.USHORT), ("NumberInputDataIndices", w.USHORT),
        ("NumberOutputButtonCaps", w.USHORT), ("NumberOutputValueCaps", w.USHORT),
        ("NumberOutputDataIndices", w.USHORT), ("NumberFeatureButtonCaps", w.USHORT),
        ("NumberFeatureValueCaps", w.USHORT), ("NumberFeatureDataIndices", w.USHORT),
    ]


def hexdump(b, base=0):
    return '\n'.join(f"  {base + i:04X}: " + ' '.join(f'{x:02X}' for x in b[i:i + 16])
                     for i in range(0, len(b), 16))


def main():
    for d in hid.enumerate():
        if d.get('vendor_id') != 0x1500:
            continue
        path = d['path'].decode('utf-8') if isinstance(d['path'], bytes) else d['path']
        print(f"=== interface {d['interface_number']} "
              f"usage_page=0x{d['usage_page']:04X} usage=0x{d['usage']:04X}")
        h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, None,
                       OPEN_EXISTING, 0, None)
        if h in (-1, 0xFFFFFFFFFFFFFFFF):
            print(f"  open failed: {ctypes.get_last_error()}")
            continue
        pp = ctypes.c_void_p()
        if not HidD_GetPreparsedData(h, ctypes.byref(pp)):
            print(f"  HidD_GetPreparsedData failed: {ctypes.get_last_error()}")
            CloseHandle(h)
            continue
        buf = ctypes.c_void_p()
        size = HidP_ReportDesc(pp, ctypes.byref(buf))
        print(f"  report descriptor: {size} bytes")
        if size:
            print(hexdump(ctypes.string_at(buf, size)))
        caps = HIDP_CAPS()
        if HidP_GetCaps(pp, ctypes.byref(caps)) == 0x00110000:
            print(f"  input  report length : {caps.InputReportByteLength}")
            print(f"  output report length : {caps.OutputReportByteLength}")
            print(f"  feature report length: {caps.FeatureReportByteLength}")
            print(f"  usage page 0x{caps.UsagePage:04X} usage 0x{caps.Usage:04X}")
        else:
            print(f"  HidP_GetCaps failed: {ctypes.get_last_error()}")
        HidD_FreePreparsedData(pp)
        CloseHandle(h)


if __name__ == '__main__':
    main()
