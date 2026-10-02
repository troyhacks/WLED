"""Predicts what the reworked band estimator should report, so the hardware
result has something to be checked against.

Reference behaviour for a constant-Q (fractional-octave) analyser on WHITE
NOISE: band power is proportional to band WIDTH, so the display should rise by
10*log10(width_k / width_0) across the spectrum. That is physics, not a fudge -
a 0.5-octave band at 9 kHz really does contain ~23 dB more noise power than one
at 48 Hz. Anything else means the two band paths still disagree.

Also reports the step across the sub-bin/binned boundary, which is the visible
symptom: a discontinuity there is the pipeline telling you where the two
estimators stop agreeing.
"""
import numpy as np

FS = 22050.0
NBANDS = 16
FLOW = 40.0
FFT_BIN_SCALE = 1.0 / 16.0
MAXPTS = 8


def bh_window(n):
    i = np.arange(n)
    return (0.35875
            - 0.48829 * np.cos(2 * np.pi * i / (n - 1))
            + 0.14128 * np.cos(4 * np.pi * i / (n - 1))
            - 0.01168 * np.cos(6 * np.pi * i / (n - 1)))


def layout(nfft):
    r = (FS / 2 / FLOW) ** (1.0 / NBANDS)
    binw = FS / nfft
    lo, hi, cen, sub, npts = [], [], [], [], []
    for k in range(NBANDS):
        a, b = FLOW * r ** k, FLOW * r ** (k + 1)
        lo.append(a); hi.append(b); cen.append(np.sqrt(a * b))
        w = b - a
        sub.append(w < 2.0 * binw)
        npts.append(min(MAXPTS, max(3, int(np.ceil(2.0 * w / binw)))))
    blo = np.full(NBANDS, nfft // 2, dtype=int)
    bhi = np.full(NBANDS, -1, dtype=int)
    for b in range(nfft // 2):
        f = b * binw
        k = 0 if f <= 0 else int(np.argmin([abs(np.log(f) - np.log(c)) for c in cen]))
        blo[k] = min(blo[k], b)
        bhi[k] = max(bhi[k], b)
    for k in range(NBANDS):
        if bhi[k] < blo[k]:
            sub[k] = True
    return dict(binw=binw, lo=lo, hi=hi, cen=cen, sub=sub, npts=npts,
                blo=blo, bhi=bhi, scale=np.sqrt(nfft / FS))


def dft_power(x, f):
    return abs(np.sum(x * np.exp(-2j * np.pi * f / FS * np.arange(len(x))))) ** 2


def measure(y, L, nfft):
    X = np.fft.rfft(y)
    out = np.zeros(NBANDS)
    for k in range(NBANDS):
        if L['sub'][k]:
            np_ = L['npts'][k]
            span = L['hi'][k] - L['lo'][k]
            f = L['lo'][k] + span * np.arange(np_) / (np_ - 1)
            acc = np.array([dft_power(y, fi) for fi in f])
            acc[[0, -1]] *= 0.5
            out[k] = FFT_BIN_SCALE * L['scale'] * np.sqrt(np.trapezoid(acc, f))
        else:
            out[k] = FFT_BIN_SCALE * np.sqrt(
                np.sum(np.abs(X[L['blo'][k]:L['bhi'][k] + 1]) ** 2))
    return out


rng = np.random.default_rng(5)
r = (FS / 2 / FLOW) ** (1.0 / NBANDS)
TRIALS = 300

# A single 23-93 ms frame of white noise gives a band estimate with enormous
# variance (a low band at N=512 rests on 2 bins, so it is roughly a 4-degree-of-
# freedom chi-squared). Bias cannot be read off one frame, so average the POWER
# over many independent realisations - the displayed value is sqrt of that, but
# the expectation of the square is what the normalisation has to get right.
print('NEW estimator, white noise, %d realisations averaged in power.' % TRIALS)
print('Display should rise 10*log10(width ratio); error is the remaining bias.\n')
for nfft in (512, 1024, 2048):
    L = layout(nfft)
    w = bh_window(nfft)
    acc = np.zeros(NBANDS)
    for _ in range(TRIALS):
        acc += measure(rng.standard_normal(nfft) * w, L, nfft) ** 2
    acc /= TRIALS
    db = 10 * np.log10(acc / acc[0])
    ideal = np.array([10 * np.log10((FLOW * r ** (k + 1) - FLOW * r ** k) /
                                   (FLOW * r - FLOW)) for k in range(NBANDS)])
    print('N=%d  (%.1f ms, sub-bin bands: %s)'
          % (nfft, nfft / FS * 1000,
             ','.join(str(k) for k in range(NBANDS) if L['sub'][k])))
    print('  measured ' + ' '.join('%6.1f' % d for d in db))
    print('  ideal    ' + ' '.join('%6.1f' % d for d in ideal))
    print('  bias     ' + ' '.join('%6.1f' % (a - b) for a, b in zip(db, ideal))
          + '   (rms %.2f dB)' % np.sqrt(np.mean((db - ideal) ** 2)))
    last = max(k for k in range(NBANDS) if L['sub'][k])
    if last < NBANDS - 1:
        print('  sub-bin/binned boundary: band %d -> %d = %+.2f dB (ideal %+.2f)'
              % (last, last + 1, db[last + 1] - db[last], ideal[last + 1] - ideal[last]))
    else:
        print('  every band sub-bin - no boundary to check')
    print()

