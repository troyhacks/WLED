#!/usr/bin/env python3
"""Check the live AudioReactive usermod against the invariants it must hold.

Fetches /json/ar from the board (or reads a saved copy) and checks the
things that can only be checked on the running board: that the 16 display
bands ARE 16 of the 31 RTA bands, that the peak comes from the bank, that
the frame is one hop, and that the task is paced by the audio rather than
by a timer.

    python tools/verify_ar.py                       # the board on the LAN
    python tools/verify_ar.py ar.json               # a saved capture
    python tools/verify_ar.py http://host/json/ar   # another board

Exit code 0 if every check passed, 1 if any of them did not.
"""

import json
import sys
import urllib.request

DEFAULT_URL = "http://192.168.6.249/json/ar"

failed = []


def check(ok, what, detail=""):
    print("  %-5s %s%s" % ("OK" if ok else "FAIL", what, ("  " + detail) if detail else ""))
    if not ok:
        failed.append(what)


def load(arg):
    if arg.startswith("http://") or arg.startswith("https://"):
        with urllib.request.urlopen(arg, timeout=10) as r:
            return json.loads(r.read().decode("utf-8"))
    with open(arg, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    d = load(arg)
    d = d.get("AudioReactive", d)

    print("source: %s" % arg)

    # The 16 display bands ARE 16 of the 31 RTA bands: same unit, same
    # window, same auto-range. The byte is the bar height, so the two have to
    # agree exactly - there is no tolerance to spend here.
    peak = d.get("peak", {})
    check(peak.get("peak_hz") == peak.get("bank_peak_hz"),
          "peak comes from the bank",
          "%s Hz" % peak.get("peak_hz"))

    geq = d.get("geq", {})
    rta = d.get("rta", {})
    rb = geq.get("rta_band", [])
    bars = geq.get("bars", [])
    rbars = rta.get("bars", [])
    raw = geq.get("raw_db", [])
    pw = rta.get("band_power_db", [])

    n = min(len(rb), len(bars), len(rbars))
    mism = [k for k in range(n) if bars[k] != rbars[rb[k]]]
    check(not mism, "geq.bars[k] == rta.bars[rta_band[k]]",
          "%d bands%s" % (n, (", mismatches: %s" % mism) if mism else ""))

    diffs = [abs(raw[k] - pw[rb[k]]) for k in range(min(len(rb), len(raw), len(pw)))
             if rb[k] < len(pw)]
    check(diffs and max(diffs) == 0.0,
          "geq.raw_db[k] == rta.band_power_db[rta_band[k]]",
          "max |diff| = %s dB" % (max(diffs) if diffs else "n/a"))

    # The frame is ONE HOP: 512 samples, 4096 bytes of frame buffer.
    mem = d.get("memory", {})
    check(mem.get("frame_samples") == 512 and mem.get("frame_buf_bytes") == 4096,
          "the frame is one hop",
          "%s samples, %s bytes" % (mem.get("frame_samples"), mem.get("frame_buf_bytes")))

    # The task is paced by the AUDIO: the read blocks for most of the frame,
    # and the cycle sits at the audio's rate (512 samples at 48 kHz = 10.667
    # ms). A cycle above that means the task takes samples out slower than
    # the codec puts them in, and the DMA ring fills until it starts
    # overwriting the samples it has not handed over.
    t = d.get("timing_us", {})
    parts = t.get("parts", {})
    cycle = t.get("cycle", 0.0)
    read = parts.get("read_carry", 0.0)
    rate = d.get("rtatopbank", {}).get("rate_hz", 48000)
    period_us = 512 * 1e6 / float(rate)
    check(abs(cycle - period_us) < 200.0,
          "the cycle sits at the audio's rate",
          "%.1f us (period %.1f us)" % (cycle, period_us))
    check(read > period_us * 0.5,
          "the read blocks - it is the pacer",
          "%.1f us" % read)

    # The overrun counter is CUMULATIVE, and the endpoint's own traffic adds
    # to it: reading /json/ar costs the analysis task one frame's compute
    # budget roughly once every seven polls (measured 2026-10-29: 30 polls
    # -> +3, idle 30 s -> +0, 15 x /json/version -> +0). So "the counter is
    # zero" is a property of a freshly booted board, not of a healthy one -
    # a board that has been up for hours fails it however well it is
    # behaving. What matters is the RATE: a board that is genuinely overrunning
    # its budget overruns every frame or close to it.
    overran = t.get("frames_overran", 0)
    total = t.get("frames_total", 0)
    rate = (float(overran) / total) if total else 0.0
    check(rate < 0.001,
          "the analysis task is not overrunning its budget",
          "%s of %s frames (%.4f%%)" % (overran, total, rate * 100.0))

    h = t.get("health", {})
    for k in ("budget", "pacing", "dma_surplus"):
        check(h.get(k) == "ok", "health.%s" % k, str(h.get(k)))
    check(h.get("read_blocking") is True, "health.read_blocking", str(h.get("read_blocking")))

    for name in ("bank", "rtatopbank"):
        b = d.get(name, {})
        check(b.get("ready") is True, "%s ready" % name)
    check(d.get("rtatopbank", {}).get("healthy") is True, "rtatopbank healthy")

    print()
    print("%d checks failed" % len(failed) if failed else "all checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
