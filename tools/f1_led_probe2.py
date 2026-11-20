#!/usr/bin/env python3
"""Blink the F1's LEDs through every plausible SET_REPORT shape.

hidapi on Windows only exposes send_feature_report(), so the Output-report
path needs the raw control transfer. This opens the HID interface directly
and issues SET_REPORT (IOCTL_HID_SET_REPORT / IOCTL_HID_SET_FEATURE) with
every combination of:

    report type   Output (0x02) or Feature (0x03)
    report ID     0x80 in wValue (and the data starts with it) or no ID
    data length   80 or 81 bytes

Each candidate lights EVERYTHING (the 16 pads red, the buttons white, the
display showing 88) for four seconds, then blanks it for a second and a
half. Watch the controller and count the flashes.

Usage:
    python tools/f1_led_probe2.py [rounds]
"""
import ctypes
import sys
import time

import hid

F1_VID, F1_PID = 0x17CC, 0x1120
REPORT_ID = 0x80

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = 0xFFFFFFFFFFFFFFFF

IOCTL_HID_SET_REPORT = 0x000B0024    # Output report
IOCTL_HID_SET_FEATURE = 0x000B002C   # Feature report

k = ctypes.WinDLL("kernel32", use_last_error=True)
k.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                           ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                           ctypes.c_void_p]
k.CreateFileW.restype = ctypes.c_void_p
k.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                               ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                               ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
k.DeviceIoControl.restype = ctypes.c_int
k.CloseHandle.argtypes = [ctypes.c_void_p]

# The supported way to send a report: the HID class APIs.
hidapi = ctypes.WinDLL("hid.dll")
hidapi.HidD_SetOutputReport.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_ulong]
hidapi.HidD_SetOutputReport.restype = ctypes.c_int
hidapi.HidD_SetFeature.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.c_ulong]
hidapi.HidD_SetFeature.restype = ctypes.c_int


class SetupPacket(ctypes.Structure):
    _fields_ = [
        ("bmRequestType", ctypes.c_ubyte),
        ("bRequest", ctypes.c_ubyte),
        ("wValue", ctypes.c_uint16),
        ("wIndex", ctypes.c_uint16),
        ("wLength", ctypes.c_uint16),
    ]


def find_path():
    for d in hid.enumerate():
        if d["vendor_id"] == F1_VID and d["product_id"] == F1_PID:
            p = d["path"]
            return p.decode("utf-8", "replace") if isinstance(p, bytes) else p
    return None


def build(data_len, pads=True):
    """The output report's data bytes (no report ID byte)."""
    buf = bytearray(data_len)
    for i in range(16):                       # both digits, all strokes
        buf[i] = 0x7F
    for i in range(16, 24):                   # the special buttons
        buf[i] = 0x7F
    for i in range(72, min(80, data_len)):    # the four lower buttons
        buf[i] = 0x7F
    if pads:
        for p in range(16):
            base = 24 + p * 3
            if base + 2 < data_len:
                buf[base + 0] = 0x00           # blue
                buf[base + 1] = 0x7F           # red
                buf[base + 2] = 0x00           # green
    return bytes(buf)


def send(handle, report_type, with_id, data):
    """One SET_REPORT. Returns (ok, error).

    Uses the documented HID class APIs (HidD_SetOutputReport /
    HidD_SetFeature) rather than raw IOCTLs: this device's IOCTLs come back
    as ERROR_INVALID_FUNCTION, but the class APIs are the supported path.
    """
    payload = bytes([REPORT_ID]) + data if with_id else data
    buf = (ctypes.c_ubyte * len(payload)).from_buffer_copy(payload)
    if report_type == 0x02:
        ok = hidapi.HidD_SetOutputReport(handle, buf, len(payload))
    else:
        ok = hidapi.HidD_SetFeature(handle, buf, len(payload))
    return bool(ok), ctypes.get_last_error()


def main():
    path = find_path()
    if not path:
        sys.exit("F1 not found - plug it into this PC")
    handle = k.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           None, OPEN_EXISTING, 0, None)
    if not handle or handle == INVALID_HANDLE_VALUE:
        sys.exit(f"open failed: {ctypes.get_last_error()}")

    candidates = []
    for report_type, tname in ((0x02, "Output"), (0x03, "Feature")):
        for data_len in (80, 81):
            candidates.append((report_type, tname, True, data_len))

    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    for r in range(rounds):
        for n, (rtype, tname, with_id, dlen) in enumerate(candidates):
            ok, err = send(handle, rtype, with_id, build(dlen))
            label = f"{tname} report, {'ID 0x80' if with_id else 'no ID'}, {dlen} data bytes"
            print(f"round {r} candidate {n + 1}: {label} -> "
                  f"{'sent' if ok else f'FAILED ({err})'}", flush=True)
            time.sleep(4.0)
            send(handle, rtype, with_id, build(dlen, pads=False))
            time.sleep(1.5)
    print("\ndone - which candidate lit up?", flush=True)
    k.CloseHandle(handle)


if __name__ == "__main__":
    main()
