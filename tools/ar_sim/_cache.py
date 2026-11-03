#!/usr/bin/env python3
"""Decode the real music ONCE, into a form every later experiment can use.

Decoding an MP3 takes about a second, and the estimator study wants to try
dozens of band-pass designs over the same audio.  Doing the decode inside
every experiment means paying that second over and over, and it is what made
the first full-corpus run crawl.

So: decode once, keep what the estimator actually needs.

The estimator listens in the 40-160 Hz band, so the signal only has to be
good to about 400 Hz.  6 kHz keeps 20-3 000 Hz clean and is still 8x smaller
than the raw 48 kHz.

The rate is not arbitrary.  The ODF's frame rate must stay EXACTLY 93.75
frames per second, because that rate is what maps a lag to a BPM and it is
what the firmware's hop of 512 samples at 48 kHz gives:

    6000 / 64 = 93.75 frames per second, exactly.

A rate that did not divide evenly into 93.75 would quietly change the
estimator's time base and every BPM it reports.  The first cut of this file
used 1000 Hz, where the hop would have been 10.667 samples - not a whole
number - and would have moved every BPM by 0.4 %.

    python tools/ar_sim/_cache.py            # build
    python tools/ar_sim/_cache.py --check    # what is in it

The cache is keyed by file name and window start, so a track that is decoded
again after a re-download replaces its own entry and nothing else.
"""

import argparse
import glob
import json
import os
import sys

import numpy as np

# Where the corpus lives.  Override it with AR_MUSIC_DIR to point the
# experiments at another folder of music; the cache is keyed by file name, so
# a different corpus simply means a different cache.
MUSIC_DIR = os.environ.get("AR_MUSIC_DIR", r"C:\Users\troys\Downloads\house_2026")
CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_cache")
META_PATH = os.path.join(CACHE_DIR, "_meta.json")

CACHE_RATE = 6000        # Nyquist 3000 Hz; the 40-160 Hz band sits far below it
CACHE_HOP = 64           # 6000/64 = 93.75 frames per second, exactly

# The windows, as seconds into the track.  Three of them, from the steady
# part: an estimator that is right by luck gives different answers in
# different windows, and an estimator that is wrong gives the same wrong
# answer everywhere.  The two look different on the page.
WINDOWS = (90.0, 150.0, 210.0)
WINDOW_SPAN = 40.0      # seconds decoded per window

MIN_DECODE = 25.0       # seconds; below this the window is not usable


def decode_window(path, start_s, dur_s, src_rate=CACHE_RATE):
    """Decode a stretch of an audio file to mono float32 at src_rate.

    PyAV's resampler is swresample - a proper polyphase filter, not a sample
    drop - so the resampled signal is what a DAC would play.  Doing the
    decimation here, once, with a proper anti-alias filter, is much better
    than letting each experiment box-average its way down.
    """
    import av
    chunks = []
    got = 0
    with av.open(path) as container:
        stream = container.streams.audio[0]
        if start_s > 0:
            tb = float(stream.time_base)
            container.seek(int(start_s / tb), stream=stream)
        resampler = av.AudioResampler(format="flt", layout="mono", rate=src_rate)
        for frame in container.decode(stream):
            for out in resampler.resample(frame):
                a = out.to_ndarray()
                chunks.append(a.reshape(-1).astype(np.float32))
                got += a.shape[-1]
                if got >= dur_s * src_rate:
                    break
            if got >= dur_s * src_rate:
                break
    if not chunks:
        return np.zeros(0, dtype=np.float32)
    return np.concatenate(chunks)[:int(dur_s * src_rate)]


def duration_of(path):
    """The track's length in seconds, or None if it cannot be read."""
    try:
        import av
        with av.open(path) as container:
            for stream in container.streams.audio:
                if stream.duration is not None and stream.time_base:
                    return float(stream.duration * stream.time_base)
            if container.duration is not None:
                return float(container.duration) / 1e6
    except Exception:                    # noqa: BLE001
        pass
    return None


def key_for(path, start_s):
    base = os.path.splitext(os.path.basename(path))[0]
    return "%s@%d" % (base, int(start_s))


def build(files=None, windows=WINDOWS):
    os.makedirs(CACHE_DIR, exist_ok=True)
    fs = sorted(glob.glob(os.path.join(MUSIC_DIR, "*.mp3")))
    if files:
        fs = [f for f in fs if any(k in os.path.basename(f) for k in files)]

    meta = {}
    if os.path.exists(META_PATH):
        try:
            with open(META_PATH, "r", encoding="utf-8") as fh:
                meta = json.load(fh)
        except Exception:                # noqa: BLE001
            meta = {}

    total = len(fs) * len(windows)
    done = 0
    skipped = 0
    for f in fs:
        name = os.path.basename(f)
        dur = duration_of(f)
        meta[name] = dur
        for w in windows:
            out = os.path.join(CACHE_DIR, key_for(f, w) + ".npy")
            if os.path.exists(out):
                done += 1
                continue

            # A track shorter than the window start has no audio there.
            # Clamp to what the track actually has, so a 3-minute mix still
            # contributes its last usable stretch instead of being dropped.
            start = w
            if dur is not None:
                start = min(start, max(0.0, dur - MIN_DECODE))
                if dur < MIN_DECODE:
                    print("  %-44s: only %.1f s long, skipped"
                          % (name[:44], dur))
                    skipped += 1
                    continue

            try:
                x = decode_window(f, start, WINDOW_SPAN)
            except Exception as exc:        # noqa: BLE001
                print("  %s @ %d: ERROR %s" % (name[:40], w, exc))
                skipped += 1
                continue
            if len(x) < MIN_DECODE * CACHE_RATE:
                print("  %s @ %d: too short (%d samples)"
                      % (name[:40], w, len(x)))
                skipped += 1
                continue
            np.save(out, x)
            done += 1
            if done % 20 == 0:
                print("  %d/%d" % (done, total), flush=True)

    with open(META_PATH, "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=1, sort_keys=True)
    print("cache: %d entries (%d skipped) in %s" % (done, skipped, CACHE_DIR))


def load(key):
    """One cached window, or None."""
    p = os.path.join(CACHE_DIR, key + ".npy")
    if not os.path.exists(p):
        return None
    return np.load(p)


def keys():
    if not os.path.isdir(CACHE_DIR):
        return []
    return sorted(os.path.splitext(f)[0] for f in os.listdir(CACHE_DIR)
                  if f.endswith(".npy"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--files", nargs="*", default=None)
    args = ap.parse_args()

    if args.check:
        ks = keys()
        print("%d cached windows at %d Hz (hop %d = %.4f fps)"
              % (len(ks), CACHE_RATE, CACHE_HOP, CACHE_RATE / CACHE_HOP))
        for k in ks[:10]:
            y = load(k)
            print("  %-58s %8d samples" % (k[:56], len(y)))
        return 0

    build(args.files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
