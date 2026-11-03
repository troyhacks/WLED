#!/usr/bin/env python3
"""The tempo estimator against REAL house music.

The synthetic tracks in _tempo2_sim.py are a controlled test: they have a
known tempo and they have the failure modes built in.  But they are made of
sine kicks and filtered noise, and real music is neither - it has a bass
line that wanders, vocals, a room, a crowd, and a mix that puts the kick
somewhere other than the front.

So: real files, decoded to the same 48 kHz the board runs at, through the same
causal band-pass the firmware runs, and the same spectrum.

The firmware's spectrum is a running DFT; here it is an offline FFT of the
same 12 s window.  The two were measured to agree to 2e-14 relative, so the
FFT is a stand-in that costs nothing - and the real music is 100 files, not 48.

    python tools/ar_sim/_real_sim.py --list
    python tools/ar_sim/_real_sim.py --all
    python tools/ar_sim/_real_sim.py --file "004. Andrea*"
    python tools/ar_sim/_real_sim.py --all --design bpf85
"""

import argparse
import glob
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _tempo3_sim import (                       # noqa: E402
    SR, RATE, LEN, NFFT, K_MIN, K_MAX, filter_designs, apply_sections,
    r2_3_2, bin_bpm,
)

MUSIC_DIR = r"C:\Users\troys\Downloads\house_2026"

# How much of each track to look at, and where.
#
# A 12 s window is what the estimator uses.  Reading three windows from the
# steady part of a track (past the intro, before the outro) shows whether the
# estimate STABLE - an estimator that is right by luck gives different answers
# in different windows, and an estimator that is wrong gives the same wrong
# answer everywhere.  The two look different on the page.
WINDOWS = (90.0, 150.0, 210.0)     # seconds into the track
WINDOW_SPAN = 40.0                 # seconds decoded per window


# --------------------------------------------------------------------------
# Decoding
# --------------------------------------------------------------------------

def decode(path, start_s, dur_s, sr=SR):
    """Decode a stretch of an audio file to mono float32 at sr.

    The board hears 48 kHz; an MP3 is 44.1 kHz, so this resamples.  PyAV's
    resampler is swresample - a proper polyphase filter, not a sample
    drop - so the resampled signal is what a 48 kHz DAC would play.
    """
    import av
    chunks = []
    got = 0
    with av.open(path) as container:
        stream = container.streams.audio[0]
        if start_s > 0:
            tb = float(stream.time_base)
            container.seek(int(start_s / tb), stream=stream)
        resampler = av.AudioResampler(format="flt", layout="mono", rate=sr)
        for frame in container.decode(stream):
            for out in resampler.resample(frame):
                a = out.to_ndarray()
                chunks.append(a.reshape(-1).astype(np.float32))
                got += a.shape[-1]
                if got >= dur_s * sr:
                    break
            if got >= dur_s * sr:
                break
    if not chunks:
        return np.zeros(0, dtype=np.float32)
    return np.concatenate(chunks)[:int(dur_s * sr)]


# --------------------------------------------------------------------------
# The estimator, on one window
# --------------------------------------------------------------------------

def spectrum(odf):
    """The same 12 s window's spectrum.

    The firmware slides the window with a running DFT; offline the same
    window is one FFT.  They agree to 2e-14 relative (see _tempo3_sim.py
    --check), and the FFT is a great deal faster over a hundred files.
    """
    x = odf[-LEN:].astype(np.float64)
    P = np.abs(np.fft.rfft(x, NFFT))
    return P[K_MIN:K_MAX + 1]


def odf_of(x, design):
    """The firmware's ODF: the band-passed signal's per-frame energy in dB,
    and the rectified difference between frames."""
    sections = filter_designs()[design]
    y = apply_sections(x, sections)
    n = (len(y) // 512) * 512
    if n == 0:
        return np.zeros(0)
    frames = y[:n].reshape(-1, 512)
    p = (frames.astype(np.float64) ** 2).mean(axis=1)
    db = 10.0 * np.log10(p + 1e-12)
    return np.maximum(np.diff(db, prepend=db[0]), 0.0)


def estimate_window(x, design):
    odf = odf_of(x, design)
    if len(odf) < LEN:
        return None, 0.0
    bpm, mag = r2_3_2(spectrum(odf))
    return bpm, mag


def analyse(path, design, windows=WINDOWS):
    """Every window's estimate for one file."""
    out = []
    for w in windows:
        x = decode(path, w, WINDOW_SPAN)
        if len(x) < SR * 12:
            continue
        bpm, mag = estimate_window(x, design)
        out.append((w, bpm, mag))
    return out


# --------------------------------------------------------------------------
# The report
# --------------------------------------------------------------------------

def files():
    return sorted(glob.glob(os.path.join(MUSIC_DIR, "*.mp3")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--file", default=None)
    ap.add_argument("--design", default="hp40_lp160x2")
    ap.add_argument("--windows", default=",".join(str(w) for w in WINDOWS))
    args = ap.parse_args()

    fs = files()
    if args.list:
        for f in fs:
            print(os.path.basename(f))
        print("%d files" % len(fs))
        return 0

    wins = tuple(float(v) for v in args.windows.split(","))

    if args.file:
        sel = [f for f in fs if args.file in os.path.basename(f)]
        fs = sel
    elif not args.all:
        ap.print_help()
        return 0

    print("design %s   %d files   windows %s s"
          % (args.design, len(fs), ",".join(str(int(w)) for w in wins)))
    print("%-58s %8s %8s %8s  %s" % ("file", "w1", "w2", "w3", "spread"))

    all_bpms = []
    for f in fs:
        name = os.path.basename(f)
        if len(name) > 56:
            name = name[:24] + "..." + name[-28:]
        try:
            res = analyse(f, args.design, wins)
        except Exception as exc:                 # noqa: BLE001
            print("%-58s  ERROR %s" % (name, exc))
            continue
        bpms = [r[1] for r in res if r[1] is not None]
        if not bpms:
            print("%-58s  no estimate" % name)
            continue
        cells = " ".join("%8.2f" % (r[1] if r[1] is not None else 0.0) for r in res)
        spread = (max(bpms) - min(bpms)) if len(bpms) > 1 else 0.0
        flag = ""
        if spread > 1.0:
            flag = "  UNSTABLE"
        med = float(np.median(bpms))
        all_bpms.append(med)
        print("%-58s %s  %6.2f %s" % (name, cells, med, flag))

    if all_bpms:
        a = np.array(all_bpms)
        print()
        print("%d files   median %.2f   mean %.2f   min %.2f   max %.2f"
              % (len(a), np.median(a), a.mean(), a.min(), a.max()))
        hist, edges = np.histogram(a, bins=18, range=(90, 180))
        for h, lo, hi in zip(hist, edges[:-1], edges[1:]):
            if h:
                print("  %5.1f-%5.1f  %s %d" % (lo, hi, "#" * h, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
