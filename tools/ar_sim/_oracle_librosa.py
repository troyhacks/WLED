#!/usr/bin/env python3
"""A reference tempo for every cached window, from librosa.

The 100 house MP3s carry no BPM tag - mutagen found no TBPM and no
TXXX:BPM in any of them - so the corpus has no ground truth of its own.
Without ground truth an estimator can be tuned into agreement with itself
and with nothing else.

librosa's beat tracker is an independent implementation (Ellis' dynamic
programming over a tempo prior, with a comb-filter phase score), written by
somebody else, against a different set of assumptions.  It is not truth
either - it has its own failures - but two independent estimators agreeing
is evidence, and one estimator agreeing with itself is not.

So this produces, for every cached window:
    bpm     - librosa's tempo estimate
    beats   - the beat times it found, in seconds
    conf    - how peaked the local tempo distribution was

The beats are worth keeping: they let a candidate estimator be scored by
whether ITS beats line up with librosa's, which is a much sharper test than
comparing two BPM numbers that both round to the same grid.

    python tools/ar_sim/_oracle_librosa.py            # build the oracle
    python tools/ar_sim/_oracle_librosa.py --check    # what is in it
"""

import argparse
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cache                                      # noqa: E402

ORACLE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_oracle")

# librosa works at its own frame rate; this is the hop that gives the SAME
# 93.75 frames per second the firmware sees.  librosa wants a hop in samples
# at the audio rate it was given, so the audio is loaded at 6000 Hz and the
# hop is 64 - the same pair the cache uses.
HOP = 64


def analyse(x, sr=_cache.CACHE_RATE):
    """librosa's tempo and beat times for one window of audio."""
    import librosa
    y = x.astype(np.float32)

    onset = librosa.onset.onset_strength(y=y, sr=sr, hop_length=HOP)
    tempo, beats = librosa.beat.beat_track(
        onset_envelope=onset, sr=sr, hop_length=HOP, units="time",
        trim=False)
    # beat_track returns an array for the tempo; take the dominant one.
    bpm = float(np.atleast_1d(tempo)[0]) if np.atleast_1d(tempo).size else 0.0

    bt = np.atleast_1d(np.asarray(beats, dtype=float))
    bt = bt[np.isfinite(bt)]

    # the local tempo distribution: how peaked is it around the estimate
    try:
        tempogram = librosa.feature.tempogram(
            onset_envelope=onset, sr=sr, hop_length=HOP)
        conf = float(np.max(tempogram) / (np.mean(tempogram) + 1e-12))
    except Exception:                            # noqa: BLE001
        conf = 0.0

    return bpm, bt.tolist(), conf


def build():
    os.makedirs(ORACLE_DIR, exist_ok=True)
    ks = _cache.keys()
    print("oracle: %d windows" % len(ks))
    done = 0
    for k in ks:
        out = os.path.join(ORACLE_DIR, k + ".json")
        if os.path.exists(out):
            done += 1
            continue
        x = _cache.load(k)
        if x is None or len(x) < 25 * _cache.CACHE_RATE:
            continue
        try:
            bpm, beats, conf = analyse(x)
        except Exception as exc:                 # noqa: BLE001
            print("  %s: ERROR %s" % (k[:50], exc))
            continue
        with open(out, "w", encoding="utf-8") as fh:
            json.dump({"bpm": bpm, "beats": beats, "conf": conf}, fh)
        done += 1
        if done % 20 == 0:
            print("  %d/%d" % (done, len(ks)), flush=True)
    print("oracle: %d entries in %s" % (done, ORACLE_DIR))


def load(key):
    p = os.path.join(ORACLE_DIR, key + ".json")
    if not os.path.exists(p):
        return None
    with open(p, "r", encoding="utf-8") as fh:
        return json.load(fh)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.check:
        ks = sorted(os.path.splitext(f)[0] for f in os.listdir(ORACLE_DIR)
                    if f.endswith(".json")) if os.path.isdir(ORACLE_DIR) else []
        print("%d oracle entries" % len(ks))
        for k in ks[:15]:
            o = load(k)
            print("  %-56s bpm %7.2f  beats %3d  conf %6.2f"
                  % (k[:54], o["bpm"], len(o["beats"]), o["conf"]))
        return 0
    build()
    return 0


if __name__ == "__main__":
    sys.exit(main())
