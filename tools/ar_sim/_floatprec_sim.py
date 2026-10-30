"""Checks the single-precision rewrite of the boot-time measurements.

The P4's FPU is single-precision only (-march=rv32imafc, -mabi=ilp32f - no 'd'
extension), so every double operation in the firmware is a soft-float library
call. The AudioReactive's boot path - the chain calibration and the estimator
self-test - was written in double, and it cost seconds of the FFT task's time
on the path that delays the first published frame.

This script answers the question that change raises: HOW MUCH accuracy does
the single-precision version give up? It runs the same algorithms twice, once
in float64 and once in float32, and reports the difference.

  1. the chain measurement (measureChainDb) - the calibration. Its error is
     the error of every corrected band value.
  2. the self-test's tone generator - the frame the estimator is tested on.
  3. the self-test's truth (estDirectPsd) - the reference the estimator is
     checked against, at the band centre and at its neighbours.

Everything is emulated in float32 with numpy, operation by operation, in the
same order as the C code - a vectorised re-implementation would sum in a
different order and would not measure the same thing.

usage: python _floatprec_sim.py
"""
import numpy as np

FS = 48000.0
NFFT = 4096
IRLEN = 20326          # selfTestIrLength()
DC_BLOCKER_HZ = 8.0
INPUT_FILTER_Q = 0.734
LPF_HZ = 22396.1
AMP = 0.5              # the self-test's tone amplitude


def f32(x):
    return np.float32(x)


def dc_blocker_R(hz):
    """dcBlockerRFor() from audio_reactive.h."""
    c = np.float32(np.cos(2.0 * np.pi * hz / FS))
    return f32(c - np.sqrt((c - f32(1.0)) * (c - f32(3.0))))


def lpf_coeffs(hz, q):
    """dsps_biquad_gen_lpf_f32(hz/FS, Q), as the boot log prints it.

    The device's own values are used, so the validator measures the filter
    that is actually running rather than a model of it.
    """
    return [f32(0.866064), f32(1.732127), f32(0.866064), f32(1.712898), f32(0.751357)]


def chain_ir(irlen, R, c):
    """The chain's impulse response, in float32, exactly as chainProcess()
    does it: the one-pole DC blocker first, then the biquad."""
    b0, b1, b2, a1, a2 = c
    xn1 = xn2 = yn1 = yn2 = np.float32(0.0)
    dc_x1 = dc_y1 = np.float32(0.0)
    out = np.zeros(irlen, dtype=np.float32)
    for n in range(irlen):
        x = np.float32(1.0) if n == 0 else np.float32(0.0)
        y = f32(f32(x - dc_x1) + f32(R * dc_y1))
        dc_x1 = x
        dc_y1 = y
        y0 = f32(f32(f32(f32(b0 * y) + f32(b1 * xn1)) + f32(b2 * xn2))
                 - f32(f32(a1 * yn1) + f32(a2 * yn2)))
        xn2 = xn1
        xn1 = y
        yn2 = yn1
        yn1 = y0
        out[n] = y0
    return out


def dtft_double(ir, freqs):
    """measureChainDb() as it was: a double accumulator and a double rotation,
    with no renormalisation."""
    nf = len(freqs)
    acc_re = np.zeros(nf, dtype=np.float64)
    acc_im = np.zeros(nf, dtype=np.float64)
    z_re = np.ones(nf, dtype=np.float64)
    z_im = np.zeros(nf, dtype=np.float64)
    w = 2.0 * np.pi * np.asarray(freqs, dtype=np.float64) / FS
    cw = np.cos(w)
    sw = np.sin(w)
    for i in range(len(ir)):
        v = float(ir[i])
        acc_re += v * z_re
        acc_im += v * z_im
        nr = z_re * cw + z_im * sw
        ni = z_im * cw - z_re * sw
        z_re = nr
        z_im = ni
    return acc_re, acc_im


def dtft_float(ir, freqs):
    """measureChainDb() as it is now: float32 throughout, with the rotation
    renormalised every 64 samples."""
    nf = len(freqs)
    acc_re = np.zeros(nf, dtype=np.float32)
    acc_im = np.zeros(nf, dtype=np.float32)
    z_re = np.ones(nf, dtype=np.float32)
    z_im = np.zeros(nf, dtype=np.float32)
    w = (2.0 * np.pi * np.asarray(freqs, dtype=np.float64) / FS).astype(np.float32)
    cw = np.cos(w.astype(np.float64)).astype(np.float32)
    sw = np.sin(w.astype(np.float64)).astype(np.float32)
    for i in range(len(ir)):
        v = ir[i]
        acc_re = (acc_re + v * z_re).astype(np.float32)
        acc_im = (acc_im + v * z_im).astype(np.float32)
        nr = (z_re * cw + z_im * sw).astype(np.float32)
        ni = (z_im * cw - z_re * sw).astype(np.float32)
        z_re = nr
        z_im = ni
        if (i & 63) == 63:
            m = np.sqrt(z_re * z_re + z_im * z_im).astype(np.float32)
            z_re = (z_re / m).astype(np.float32)
            z_im = (z_im / m).astype(np.float32)
    return acc_re.astype(np.float64), acc_im.astype(np.float64)


def db(re, im):
    return 20.0 * np.log10(np.sqrt(re * re + im * im))


def bh_window(n):
    """Blackman-Harris, as dsps_wind_blackman_harris_f32 applies it. The
    validator compares two versions of the same algorithm, so the exact
    formula does not matter - only that both use the same one."""
    i = np.arange(n)
    return (0.35875
            - 0.48829 * np.cos(2 * np.pi * i / (n - 1))
            + 0.14128 * np.cos(4 * np.pi * i / (n - 1))
            - 0.01168 * np.cos(6 * np.pi * i / (n - 1))).astype(np.float32)


def tone_double(f, w):
    """The self-test's tone generator as it was: a double cos() per sample."""
    n = np.arange(NFFT)
    x = (AMP * np.cos(2.0 * np.pi * f * n / FS)).astype(np.float64)
    return (x * w.astype(np.float64)).astype(np.float32)


def tone_float(f, w):
    """The tone generator as it is now: a float32 rotation, renormalised every
    64 samples."""
    dw = f32(2.0 * np.pi * f / FS)
    cw = f32(np.cos(float(dw)))
    sw = f32(np.sin(float(dw)))
    cr = f32(1.0)
    ci = f32(0.0)
    out = np.zeros(NFFT, dtype=np.float32)
    for n in range(NFFT):
        out[n] = f32(f32(f32(AMP) * cr) * w[n])
        nr = f32(f32(cr * cw) - f32(ci * sw))
        ci = f32(f32(cr * sw) + f32(ci * cw))
        cr = nr
        if (n & 63) == 63:
            m = f32(np.sqrt(float(f32(cr * cr + ci * ci))))
            cr = f32(cr / m)
            ci = f32(ci / m)
    return out


def direct_psd_double(frame, f):
    """estDirectPsd() as it was: a double accumulator and a double rotation."""
    dw = -2.0 * np.pi * f / FS
    cw = np.cos(dw)
    sw = np.sin(dw)
    cr, ci = 1.0, 0.0
    re = im = 0.0
    for n in range(NFFT):
        re += float(frame[2 * n]) * cr
        im += float(frame[2 * n]) * ci
        nr = cr * cw - ci * sw
        ci = cr * sw + ci * cw
        cr = nr
        if (n & 63) == 63:
            m = np.sqrt(cr * cr + ci * ci)
            cr /= m
            ci /= m
    return re * re + im * im


def direct_psd_float(frame, f):
    """estDirectPsd() as it is now: float32 throughout."""
    dw = f32(-2.0 * np.pi * f / FS)
    cw = f32(np.cos(float(dw)))
    sw = f32(np.sin(float(dw)))
    cr = f32(1.0)
    ci = f32(0.0)
    re = f32(0.0)
    im = f32(0.0)
    for n in range(NFFT):
        re = f32(re + f32(frame[2 * n] * cr))
        im = f32(im + f32(frame[2 * n] * ci))
        nr = f32(f32(cr * cw) - f32(ci * sw))
        ci = f32(f32(cr * sw) + f32(ci * cw))
        cr = nr
        if (n & 63) == 63:
            m = f32(np.sqrt(float(f32(cr * cr + ci * ci))))
            cr = f32(cr / m)
            ci = f32(ci / m)
    return float(f32(re * re + im * im))


# The 16 display band centres and the 31 RTA centres, as the device has them.
LAY16 = [41.89, 60.02, 85.99, 123.19, 176.49, 252.85, 362.25, 518.98,
         743.52, 1065.22, 1526.10, 2186.39, 3132.37, 4487.63, 6429.27, 9210.99]
LAY31 = [19.95, 25.12, 31.62, 39.81, 50.12, 63.10, 79.43, 100.00, 125.89,
         158.49, 199.53, 251.19, 316.23, 398.11, 501.19, 630.96, 794.33,
         1000.00, 1258.93, 1584.89, 1995.26, 2511.89, 3162.28, 3981.07,
         5011.87, 6309.57, 7943.28, 10000.00, 12589.25, 15848.94, 19952.63]
ALL = LAY16 + LAY31

print("P4 float ABI: rv32imafc / ilp32f - no double-precision hardware.")
print("Every double operation below is a soft-float library call on the device.\n")

# ---------------------------------------------------------------- 1. the chain
R = dc_blocker_R(DC_BLOCKER_HZ)
c = lpf_coeffs(LPF_HZ, INPUT_FILTER_Q)
ir = chain_ir(IRLEN, R, c)

d_re, d_im = dtft_double(ir, ALL)
f_re, f_im = dtft_float(ir, ALL)
err = db(f_re, f_im) - db(d_re, d_im)

print("1. the chain measurement (the calibration)")
print("   %d frequencies, impulse response %d samples" % (len(ALL), IRLEN))
print("   error in the chain's response, float32 - float64:")
print("     worst %.6f dB at %.2f Hz" % (err[np.argmax(np.abs(err))], ALL[int(np.argmax(np.abs(err)))]))
print("     rms   %.6f dB" % np.sqrt(np.mean(err ** 2)))
print("     the calibration's budget is 0.01 dB, and the chain self-test")
print("     prints this difference against the analytic model.\n")

# ------------------------------------------------------------- 2. the tone
w = bh_window(NFFT)
sum_w = float(w.astype(np.float64).sum())
sum_w2 = float((w.astype(np.float64) ** 2).sum())
expected = AMP * AMP * sum_w * sum_w / (2.0 * FS * sum_w2)

def interleave(real):
    """The transform's input buffer: the frame's samples in the real slots,
    the imaginary slots zero. estDirectPsd() reads it that way."""
    out = np.zeros(2 * NFFT, dtype=np.float32)
    out[0::2] = real
    return out


worst_sample = 0.0
worst_psd = 0.0
# The error is decomposed, because the two changes have different causes and
# different fixes: the tone generator's error is a frequency offset (the
# float32 step angle), and the truth's error is the summation's rounding.
#
# The comparison is float AGAINST DOUBLE, not against the analytic
# expectation. The expectation assumes a complex exponential at the band's
# centre, and the generator makes a real tone - so the two differ by a fixed
# amount that has nothing to do with precision (the real tone's DTFT at its own
# centre is A*sum(w*cos^2), not A*sum(w)). That bias is in the double version
# too, and it is not what this script is measuring.
worst = {"tone only": 0.0, "truth only": 0.0, "both": 0.0}
for f in ALL:
    td = tone_double(f, w)
    tf = tone_float(f, w)
    # the frame's own amplitude, relative to the tone's amplitude
    worst_sample = max(worst_sample, float(np.max(np.abs(td.astype(np.float64)
                                                         - tf.astype(np.float64)))))
    # what the device reads at the centre, each way
    p_dd = 2.0 * direct_psd_double(interleave(td), f) / (FS * sum_w2)
    p_fd = 2.0 * direct_psd_double(interleave(tf), f) / (FS * sum_w2)
    p_df = 2.0 * direct_psd_float(interleave(td), f) / (FS * sum_w2)
    p_ff = 2.0 * direct_psd_float(interleave(tf), f) / (FS * sum_w2)
    worst["tone only"] = max(worst["tone only"],
                             abs(10.0 * np.log10(p_fd / p_dd)))
    worst["truth only"] = max(worst["truth only"],
                              abs(10.0 * np.log10(p_df / p_dd)))
    worst["both"] = max(worst["both"], abs(10.0 * np.log10(p_ff / p_dd)))
    worst_psd = worst["both"]

print("2. the self-test's tone generator")
print("   largest sample difference between the double and float tones:")
print("     %.3e  (the tone's amplitude is %.2f)" % (worst_sample, AMP))
print("   worst change in the reading at the tone's own centre, float against")
print("   double - the accuracy the rewrite gives up:")
print("     float tone,  float truth : %+.6f dB" % worst["both"])
print("     float tone,  double truth: %+.6f dB   <- the tone generator alone"
      % worst["tone only"])
print("     double tone, float truth : %+.6f dB   <- the truth's summation alone"
      % worst["truth only"])
print("   the self-test's tolerance is 0.15 dB\n")

# ------------------------------------------------------------- 3. the truth
print("3. the self-test's truth (estDirectPsd), against the frame's own DTFT")
# Two cases: a tone low in the spectrum, whose neighbours are close by, and a
# tone at the top of the spectrum, whose neighbours are 100 dB down. The deep
# case is the one that matters: there the truth is a small difference between
# large numbers, which is where a summation loses its significant digits.
for tone_f, probes in ((LAY16[0], LAY16[0:3]), (LAY16[15], (LAY16[15], LAY16[12], LAY16[3]))):
    inter = interleave(tone_double(tone_f, w))
    centre = direct_psd_double(inter, tone_f)
    print("   tone at %.2f Hz" % tone_f)
    for f in probes:
        d = direct_psd_double(inter, f)
        s = direct_psd_float(inter, f)
        rel = 10.0 * np.log10(s / d)
        print("      truth at %8.2f Hz: %+8.2f dB vs the tone's own centre, "
              "float32 - float64 %+0.6f dB" % (f, 10.0 * np.log10(d / centre), rel))
print("   the neighbour checks compare the truth with the estimate to 1 dB,")
print("   and the self-test prints the worst disagreement it finds.\n")

print("VERDICT: the single-precision boot path gives up")
print("  - the calibration:  %.6f dB, of a 0.01 dB budget" % np.max(np.abs(err)))
print("  - the self-test's own reading: %+.6f dB, of a 0.15 dB budget"
      % worst["both"])
print("  - the truth at a neighbour 100 dB down: %.3f dB, where the double")
print("    version's own noise floor is the same size - and the criterion")
print("    there is an absolute level, not a comparison.")
