#!/usr/bin/env python3
"""Check the USB audio source on the running board.

Fetches /json/ar and reports what the USB audio client is doing: whether a
device is claimed, the format it is streaming, and the three counters that
say whether the stream is healthy.

    python tools/uac_check.py                       # the board on the LAN
    python tools/uac_check.py http://host/json/ar   # another board

The counters are the point of this script:

  skipped_packets - isochronous packets the controller could not service in
                    time. Every one of them is audio that was lost on the way
                    in, and it is the number that says the stream is not
                    keeping up.
  overruns        - samples dropped because the ring was full. The analysis
                    could not keep up with the device.
  underruns       - analysis frames that had to be padded with silence
                    because the device stopped talking.

Exit code 0 when the source is streaming and nothing is being lost, 1 when it
is not, 2 when the board is unreachable.
"""

import json
import sys
import urllib.request

DEFAULT_URL = "http://192.168.6.249/json/ar"

url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL

try:
    with urllib.request.urlopen(url, timeout=8) as r:
        doc = json.load(r)
except Exception as e:
    print("could not fetch %s: %s" % (url, e))
    sys.exit(2)

ar = doc.get("AudioReactive", {})
usb = ar.get("usb", {})

print("source      : %s" % ar.get("source"))
print("codec       : %s" % (ar.get("codec_name") or ar.get("codec")))
print("processing  : %s" % ar.get("processing"))
print()
if usb:
    for k in ("connected", "rate", "channels", "bits",
              "skipped_packets", "overruns", "underruns", "urbs"):
        if k in usb:
            print("  %-15s %s" % (k, usb[k]))
else:
    print("  (no usb object: the source is not the USB one)")

connected = bool(usb.get("connected"))
skipped = int(usb.get("skipped_packets", 0) or 0)
overruns = int(usb.get("overruns", 0) or 0)
underruns = int(usb.get("underruns", 0) or 0)
urbs = int(usb.get("urbs", 0) or 0)
rate = int(usb.get("rate", 0) or 0)

print()
ok = True
if not connected:
    print("NOT STREAMING: no USB audio device is claimed.")
    ok = False
else:
    if rate != 48000:
        print("WRONG RATE: %d Hz. The analysis' band centres are absolute "
              "frequencies, so anything but 48000 puts every band on the "
              "wrong axis." % rate)
        ok = False
    if urbs == 0:
        print("NO TRANSFERS: the device is claimed but not a single "
              "isochronous transfer has completed.")
        ok = False
    if skipped:
        print("LOST AUDIO: %d isochronous packets were skipped. The "
              "controller could not service them in time." % skipped)
        ok = False
    if overruns:
        print("LOST AUDIO: %d samples were dropped - the ring was full." % overruns)
        ok = False
    if underruns:
        print("GAPS: %d analysis frames were padded with silence." % underruns)
        ok = False

print()
print("OK" if ok else "PROBLEMS FOUND")
sys.exit(0 if ok else 1)
