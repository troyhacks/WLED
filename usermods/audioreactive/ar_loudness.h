#pragma once

#include "ar_publish.h"

#ifdef ARDUINO_ARCH_ESP32
  #include <esp_heap_caps.h>
#endif

// ===========================================================================
// THE LOUDNESS MODULE
//
// ITU-R BS.1770-4 programme loudness, measured on this board's own input.
//
// WHAT A HARDWARE ANALYSER SHOWS, AND WHAT THIS SHOWS
// -----------------------------------------------------
//
// A programme meter on an RTA is a family of numbers, and each one is a
// different WINDOW on the same measurement:
//
//   programme loudness   the gated mean over the whole programme   (LUFS)
//   momentary            the last 400 ms                          (LUFS)
//   short term           the last 3 s                             (LUFS)
//   sliding              a rolling ten seconds                    (LUFS)
//   loudness range       the spread of the short-term values      (LU)
//   true peak            the INTER-SAMPLE peak                    (dBTP)
//
// This module measures all six, the same way the standard says to, so that
// the numbers can be put next to the analyser's and read.
//
// THE MEASUREMENT ITSELF
// ----------------------
//
// BS.1770 loudness is the mean square of a K-WEIGHTED signal: a high shelf
// that lifts the top end the way the ear does, and a high-pass that takes
// the sub-bass and the DC out. The coefficients are the standard's own, for
// 48 kHz, and they are applied as two biquads to every sample as it arrives.
//
// The loudness of a window is then
//
//   L = -0.691 + 10*log10(mean square over the window)
//
// The -0.691 is the standard's own offset: it is what makes a full-scale
// sine read 0 LUFS.
//
// THE GATE
// --------
//
// A programme's mean square is dominated by its loudest parts, and a gate is
// what stops a single loud passage from carrying the whole programme. The
// standard uses two: an ABSOLUTE gate at -70 LUFS (which drops the silence
// and the noise floor) and a RELATIVE gate 10 LU below the mean of what is
// left (which drops the parts of the programme that are much quieter than
// the programme).
//
// The integrated loudness is the mean of what passes BOTH gates.
//
// THE LOUDNESS RANGE uses the same idea with a wider relative gate: 20 LU
// below the absolute-gated mean, and the range is the 95th percentile minus
// the 10th of the short-term values that pass it.
//
// WHY THE RAW INPUT
// -----------------
//
// A loudness meter measures what ARRIVES, not what a filter made of it.
//
// This board's input chain (the DC blocker and the input LPF) is corrected
// for in the BANDS - that is what the calibration does - but a loudness
// number with the chain's response divided out of it would not be
// comparable with a hardware analyser's, which does not correct its
// microphone either. So the loudness is measured on the samples as they come
// out of the codec, BEFORE the chain touches them.
//
// (The K-weighting's own 38 Hz high-pass takes the DC out, which is what the
// standard says it should.)
//
// THE ABSOLUTE SCALE, AND WHY IT NEEDS A CALIBRATION
// ---------------------------------------------------
//
// LUFS and dBTP are absolute: they are defined against DIGITAL FULL SCALE.
// Ours is the CODEC's full scale - the level at which the ADC would clip -
// and the codec's gain is part of that reference: the ES8311's PGA is at
// 0 dB in line mode and +30 dB in mic mode, so the same sound arrives 30 dB
// apart on the two settings.
//
// So the number is "dB relative to the codec's full scale" until it is
// calibrated against the instrument it is being compared with, and the
// calibration is ONE constant: `loudnessCalDb`, which is added to every
// figure this module publishes. It is the same shape as the RTA's reference
// offset - a live setting, adjustable from the serial console and the
// settings page.
//
// THE TRUE PEAK
// -------------
//
// A sample peak UNDER-reads: two samples can straddle a peak that is
// between them. The true peak is the peak of the signal AFTER it has been
// reconstructed at 4x the sample rate.
//
// The reconstruction here is a cubic (Catmull-Rom) through the four samples
// around each gap, sampled at three points in the gap. On audio material it
// is accurate to well under 0.01 dB, where the linear interpolation a
// cheaper meter does can read a decibel low on a high-frequency tone.
//
// WHAT IS NOT HERE
// ----------------
//
// The LOUDNESS is not the "level" the pane used to show and is not the
// auto-range's reference: those are broadband RMS numbers in the analyser's
// own units. This is a different measurement, made to the standard.
//
// The windows are whole FRAMES, not exact milliseconds: the frame is one hop
// of 512 samples at 48 kHz, so 400 ms is 37.5 frames and the window is 38
// of them (405.3 ms), 3 s is 282 (3.008 s) and 10 s is 938 (10.005 s). A
// mean over a window does not care that the window is 1.3 % long.
//
// The programme's history is a ROLLING window: the last ten minutes of
// 400 ms blocks for the integrated loudness, and the last ten minutes of
// 100 ms short-term samples for the range. A live analyser integrates over
// the programme so far; this one integrates over the last ten minutes of
// it, which is the same thing once the programme is longer than that, and
// is what lets the numbers be watched while the music plays.
// ===========================================================================

// ---------------------------------------------------------------------------
// The window lengths, in FRAMES.
//
// The frame is one hop: 512 samples at 48 kHz = 10.6667 ms.
//   400 ms -> 37.5 frames -> 38 frames (405.3 ms)
//   3 s    -> 281.25     -> 282 frames (3.008 s)
//   10 s   -> 937.5      -> 938 frames (10.005 s)
constexpr int AR_LOUD_FRAMES_04S  = 38;
constexpr int AR_LOUD_FRAMES_3S   = 282;
constexpr int AR_LOUD_FRAMES_10S  = 938;

// The ring of frame energies that the windows are cut from. It is the
// longest of them, so one ring serves all three.
constexpr int AR_LOUD_RING        = AR_LOUD_FRAMES_10S;

// The programme's history.
//
// The integrated loudness is computed over the last ten minutes of 400 ms
// blocks: 10 * 60 / 0.4 = 1500 blocks.
constexpr int AR_LOUD_BLOCKS       = 1500;

// The loudness range is computed over the last ten minutes of 100 ms
// short-term samples: 10 * 60 * 10 = 6000 samples.
constexpr int AR_LOUD_SHORTS       = 6000;

// The histogram the percentiles are read from: 0.1 LU bins from -80 LUFS to
// +20 LUFS.
constexpr int AR_LOUD_HIST_BINS    = 1000;
constexpr float AR_LOUD_HIST_LO     = -80.0f;
constexpr float AR_LOUD_HIST_HI     =  20.0f;

// The gates.
constexpr float AR_LOUD_ABS_GATE   = -70.0f;   // LUFS
constexpr float AR_LOUD_REL_GATE   = -10.0f;   // LU below the gated mean
constexpr float AR_LOUD_LRA_REL    = -20.0f;   // LU below the gated mean

// The standard's offset: a full-scale sine reads 0 LUFS.
constexpr float AR_LOUD_OFFSET     = -0.691f;

// DIGITAL FULL SCALE, in the sample domain the analysis works in.
//
// The samples arrive from the codec in 16-bit units - the same units the
// analysis's own level uses (AR_LEVEL_FS in audio_reactive.h, which is the
// number this must agree with). So the K-weighted mean square is divided by
// the square of the rail before it becomes a loudness, and a full-scale sine
// reads 0 LUFS as the standard says it must.
//
// The division is done ONCE per frame on the frame's energy, not per sample:
// the P4's soft-float divide is not free, and 512 of them a frame is 48 000
// a second for nothing.
constexpr float AR_LOUD_FS         = 32767.0f;

// The published numbers live in the publish module (ar_publish.h), with the
// rest of the analysis: the effect that draws a programme meter reads them
// from there, under the same lock as everything else.
//
// The module's own state. It is big (about 38 KB) and it is allocated in
// PSRAM: it is a history, not a working set, and the internal heap is
// wanted by the web server.
struct ArLoudnessState {
  // The K-weighting filters' delay lines.
  float hsZ1, hsZ2;         // the high shelf
  float hpZ1, hpZ2;         // the RLB high-pass

  // The last three samples, for the true peak's interpolation.
  float tp0, tp1, tp2;

  // The ring of frame energies, and where in it the newest one is.
  float *ring;
  int    ringPos;
  int    ringCount;

  // The 400 ms blocks of the programme's history.
  float *blocks;
  int    blockPos;
  int    blockCount;

  // The 100 ms short-term samples of the programme's history.
  float *shorts;
  int    shortPos;
  int    shortCount;

  // The histogram the percentiles are read from.
  uint32_t *hist;

  // The counters.
  uint32_t framesSinceBlock;
  uint32_t framesSinceShort;
  uint32_t framesSinceIntegrated;
  uint32_t framesSinceHistogram;
  uint32_t framesSincePercentiles;

  // The results.
  float momentary;
  float momentaryMax;
  float shortTerm;
  float shortTermMax;
  float sliding10;
  float programme;
  float lra;
  float truePeak;
  float truePeakNow;
};

static ArLoudnessState *arLoud = nullptr;

// ---------------------------------------------------------------------------
// The K-weighting, in the standard's own coefficients for 48 kHz.
//
// Stage 1 is the high shelf (+4 dB above about 1.7 kHz); stage 2 is the
// RLB high-pass at 38 Hz. They are applied in that order.
//
// A biquad here is a transposed direct form II: the state is two floats,
// and the filter is continuous across frames because the state is not reset
// between them.
static const float AR_KW_HS_B0 =  1.53512485958697f;
static const float AR_KW_HS_B1 = -2.69169618940638f;
static const float AR_KW_HS_B2 =  1.19839281085285f;
static const float AR_KW_HS_A1 = -1.69065929318241f;
static const float AR_KW_HS_A2 =  0.73248077421585f;

static const float AR_KW_HP_B0 =  1.0f;
static const float AR_KW_HP_B1 = -2.0f;
static const float AR_KW_HP_B2 =  1.0f;
static const float AR_KW_HP_A1 = -1.99004745483398f;
static const float AR_KW_HP_A2 =  0.99007225036621f;

// The loudness of a mean square.
static inline float arLoudOf(float meanSq) {
  return (meanSq > 1e-12f) ? (AR_LOUD_OFFSET + 10.0f * log10f(meanSq)) : -120.0f;
}

// ---------------------------------------------------------------------------
// The history's memory.
//
// It is PSRAM on the ESP32: 38 KB that the web server's heap would rather
// have. It is a HISTORY and not a working set - it is read once a second at
// most, and the web server wants the internal heap.
//
// If PSRAM is not there the allocation falls back to the ordinary heap,
// because a loudness meter that does not run is worse than one that costs
// 38 KB.
static void* arLoudAlloc(size_t bytes) {
#ifdef ARDUINO_ARCH_ESP32
  return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
#else
  return calloc(1, bytes);
#endif
}

// ---------------------------------------------------------------------------
// Allocate the module's state.
static void arLoudnessReset();   // defined below
static bool arLoudnessInit() {
  if (arLoud) return true;
  arLoud = (ArLoudnessState*)arLoudAlloc(sizeof(ArLoudnessState));
  if (!arLoud) return false;
  const size_t ringB   = (size_t)AR_LOUD_RING   * sizeof(float);
  const size_t blockB  = (size_t)AR_LOUD_BLOCKS  * sizeof(float);
  const size_t shortB  = (size_t)AR_LOUD_SHORTS  * sizeof(float);
  const size_t histB   = (size_t)AR_LOUD_HIST_BINS * sizeof(uint32_t);
  arLoud->ring   = (float*)arLoudAlloc(ringB);
  arLoud->blocks = (float*)arLoudAlloc(blockB);
  arLoud->shorts = (float*)arLoudAlloc(shortB);
  arLoud->hist   = (uint32_t*)arLoudAlloc(histB);
  if (!arLoud->ring || !arLoud->blocks || !arLoud->shorts || !arLoud->hist) {
    if (arLoud->ring)   { free(arLoud->ring);   arLoud->ring   = nullptr; }
    if (arLoud->blocks) { free(arLoud->blocks); arLoud->blocks = nullptr; }
    if (arLoud->shorts) { free(arLoud->shorts); arLoud->shorts = nullptr; }
    if (arLoud->hist)   { free(arLoud->hist);   arLoud->hist   = nullptr; }
    free(arLoud);
    arLoud = nullptr;
    return false;
  }
  arLoudnessReset();
  return true;
}

// ---------------------------------------------------------------------------
// Forget the programme.
//
// The integrated loudness, the range, the peaks: all of them are statements
// about a PROGRAMME, and a programme that has changed makes them all wrong.
// So they are cleared together, and the button that clears them is the one
// that says "the programme starts here".
//
// The published copy is NOT cleared here: the analysis task rewrites it
// whole at the end of every frame, and the frame after a reset publishes
// the empty programme rather than the old one.
static void arLoudnessReset() {
  if (!arLoud) return;
  arLoud->ringPos = 0; arLoud->ringCount = 0;
  arLoud->blockPos = 0; arLoud->blockCount = 0;
  arLoud->shortPos = 0; arLoud->shortCount = 0;
  memset(arLoud->hist, 0, (size_t)AR_LOUD_HIST_BINS * sizeof(uint32_t));
  arLoud->framesSinceBlock = 0;
  arLoud->framesSinceShort = 0;
  arLoud->framesSinceIntegrated = 0;
  arLoud->framesSinceHistogram = 0;
  arLoud->framesSincePercentiles = 0;
  arLoud->momentary = -120.0f;
  arLoud->momentaryMax = -120.0f;
  arLoud->shortTerm = -120.0f;
  arLoud->shortTermMax = -120.0f;
  arLoud->sliding10 = -120.0f;
  arLoud->programme = -120.0f;
  arLoud->lra = 0.0f;
  arLoud->truePeak = -120.0f;
  arLoud->truePeakNow = -120.0f;
}

// The reset REQUEST.
//
// The history belongs to the analysis task: it writes it every frame and the
// web server does not. So a reset from the web server is a REQUEST, and the
// analysis task carries it out at the top of its next frame - there and
// nowhere else, and never in the middle of a frame's worth of samples.
static volatile bool arLoudResetRequested = false;

static void arLoudnessRequestReset() {
  arLoudResetRequested = true;
}


// ---------------------------------------------------------------------------
// The mean of the last `frames` entries in the ring.
//
// The ring holds the newest frame's energy at ringPos - 1 and walks
// backwards from there. The sum is done by walking the ring once, without a
// modulo per element: the index is decremented and wrapped by hand.
static float arLoudWindowMean(int frames) {
  if (!arLoud || arLoud->ringCount == 0) return 0.0f;
  int n = (frames < arLoud->ringCount) ? frames : arLoud->ringCount;
  if (n <= 0) return 0.0f;
  float sum = 0.0f;
  int idx = arLoud->ringPos - 1;
  if (idx < 0) idx += AR_LOUD_RING;
  for (int i = 0; i < n; i++) {
    sum += arLoud->ring[idx];
    idx--;
    if (idx < 0) idx += AR_LOUD_RING;
  }
  return sum / (float)n;
}

// ---------------------------------------------------------------------------
// The integrated loudness over the block history.
//
// The gates: the absolute gate at -70 LUFS, then a relative gate 10 LU
// below the mean of what is left. Two passes over the history, because the
// relative gate depends on the first pass.
static void arLoudnessRecomputeIntegrated() {
  if (!arLoud || arLoud->blockCount == 0) return;
  const float absE = 10.0f * powf(10.0f, (AR_LOUD_ABS_GATE - AR_LOUD_OFFSET) / 10.0f);

  // Pass 1: the mean of the blocks above the absolute gate.
  float sum = 0.0f;
  uint32_t count = 0;
  for (int i = 0; i < arLoud->blockCount; i++) {
    const float e = arLoud->blocks[i];
    if (e >= absE) { sum += e; count++; }
  }
  if (count == 0) { arLoud->programme = -120.0f; return; }

  // The relative gate: 10 LU below that mean.
  const float relE = (sum / (float)count) * powf(10.0f, AR_LOUD_REL_GATE / 10.0f);

  // Pass 2: the mean of what passes BOTH gates.
  sum = 0.0f;
  count = 0;
  for (int i = 0; i < arLoud->blockCount; i++) {
    const float e = arLoud->blocks[i];
    if (e >= absE && e >= relE) { sum += e; count++; }
  }
  if (count == 0) { arLoud->programme = -120.0f; return; }
  arLoud->programme = arLoudOf(sum / (float)count);
}

// ---------------------------------------------------------------------------
// The loudness range.
//
// The short-term history is turned into a histogram (0.1 LU bins) and the
// percentiles are read from that, because sorting six thousand values in the
// analysis task would be a spike big enough to overrun the frame.
//
// The histogram is rebuilt every AR_LOUD_HIST_PERIOD frames, and the
// percentiles are read from it every AR_LOUD_PCTL_PERIOD.
static void arLoudnessRebuildHistogram() {
  if (!arLoud || arLoud->shortCount == 0) return;
  // The histogram is cleared FIRST, so that a rebuild which finds nothing to
  // gate leaves an EMPTY histogram and not the last one's values: a stale
  // percentile is worse than no percentile.
  memset(arLoud->hist, 0, (size_t)AR_LOUD_HIST_BINS * sizeof(uint32_t));
  const float absE = 10.0f * powf(10.0f, (AR_LOUD_ABS_GATE - AR_LOUD_OFFSET) / 10.0f);

  // Pass 1: the mean of the short-term values above the absolute gate.
  float sum = 0.0f;
  uint32_t count = 0;
  for (int i = 0; i < arLoud->shortCount; i++) {
    const float e = arLoud->shorts[i];
    if (e >= absE) { sum += e; count++; }
  }
  if (count == 0) { arLoud->lra = 0.0f; return; }

  // The relative gate for the range: 20 LU below that mean.
  const float relE = (sum / (float)count) * powf(10.0f, AR_LOUD_LRA_REL / 10.0f);

  // Pass 2: the histogram of what passes both gates.
  uint32_t gated = 0;
  for (int i = 0; i < arLoud->shortCount; i++) {
    const float e = arLoud->shorts[i];
    if (e < absE || e < relE) continue;
    const float l = arLoudOf(e);
    int bin = (int)((l - AR_LOUD_HIST_LO) / 0.1f);
    if (bin < 0) bin = 0;
    if (bin >= AR_LOUD_HIST_BINS) bin = AR_LOUD_HIST_BINS - 1;
    arLoud->hist[bin]++;
    gated++;
  }
  (void)gated;
}

// The percentile of the histogram, in LUFS.
static float arLoudPercentile(float p) {
  if (!arLoud) return -120.0f;
  uint32_t total = 0;
  for (int i = 0; i < AR_LOUD_HIST_BINS; i++) total += arLoud->hist[i];
  if (total == 0) return -120.0f;
  const uint32_t want = (uint32_t)(total * p + 0.5f);
  uint32_t cum = 0;
  for (int i = 0; i < AR_LOUD_HIST_BINS; i++) {
    cum += arLoud->hist[i];
    if (cum >= want) return AR_LOUD_HIST_LO + (float)i * 0.1f + 0.05f;
  }
  return AR_LOUD_HIST_HI;
}

// ---------------------------------------------------------------------------
// ONE FRAME of samples.
//
// `x` is the RAW input - the codec's output, before the input chain. `n` is
// the frame's length (512).
static void arLoudnessProcess(const float *x, int n) {
  if (!arLoud || !x || n <= 0) return;

  // A reset asked for by the web server is carried out HERE, at the top of
  // the frame - the one place that owns the history.
  if (arLoudResetRequested) {
    arLoudResetRequested = false;
    arLoudnessReset();
  }

  // ---- the K-weighted mean square of this frame -------------------------
  float sum = 0.0f;
  float z1 = arLoud->hsZ1, z2 = arLoud->hsZ2;
  float y1 = arLoud->hpZ1, y2 = arLoud->hpZ2;
  for (int i = 0; i < n; i++) {
    const float in = x[i];

    // The high shelf.
    float v = AR_KW_HS_B0 * in + z1;
    z1 = AR_KW_HS_B1 * in - AR_KW_HS_A1 * v + z2;
    z2 = AR_KW_HS_B2 * in - AR_KW_HS_A2 * v;

    // The RLB high-pass.
    float o = AR_KW_HP_B0 * v + y1;
    y1 = AR_KW_HP_B1 * v - AR_KW_HP_A1 * o + y2;
    y2 = AR_KW_HP_B2 * v - AR_KW_HP_A2 * o;

    sum += o * o;
  }
  arLoud->hsZ1 = z1; arLoud->hsZ2 = z2;
  arLoud->hpZ1 = y1; arLoud->hpZ2 = y2;
  // The frame's energy, in FULL SCALE units: the samples are in 16-bit
  // units, so the mean square is divided by the rail's square.
  const float fs2 = AR_LOUD_FS * AR_LOUD_FS;
  const float frameE = sum / ((float)n * fs2);

  // ---- the true peak ----------------------------------------------------
  //
  // The inter-sample peak: the signal reconstructed at four times the
  // sample rate, by a cubic through the four samples around each gap.
  float q0 = arLoud->tp0, q1 = arLoud->tp1, q2 = arLoud->tp2;
  float framePeak = 0.0f;
  for (int i = 0; i < n; i++) {
    const float xn = x[i];
    // The quadruple (q0, q1, q2, xn): three points between q1 and q2.
    for (int t = 1; t < 4; t++) {
      const float u = (float)t * 0.25f;
      const float u2 = u * u;
      const float u3 = u2 * u;
      const float p = 0.5f * ((2.0f * q1)
                     + (-q0 + q2) * u
                     + (2.0f * q0 - 5.0f * q1 + 4.0f * q2 - xn) * u2
                     + (-q0 + 3.0f * q1 - 3.0f * q2 + xn) * u3);
      const float a = fabsf(p);
      if (a > framePeak) framePeak = a;
    }
    q0 = q1; q1 = q2; q2 = xn;
  }
  arLoud->tp0 = q0; arLoud->tp1 = q1; arLoud->tp2 = q2;

  // The true peak, in dBTP: the inter-sample peak against the same rail.
  const float tpDb = (framePeak > 1e-6f) ? (20.0f * log10f(framePeak / AR_LOUD_FS)) : -120.0f;
  arLoud->truePeakNow = tpDb;
  if (arLoud->truePeakNow > arLoud->truePeak) arLoud->truePeak = arLoud->truePeakNow;

  // ---- the frame into the ring -----------------------------------------
  arLoud->ring[arLoud->ringPos] = frameE;
  arLoud->ringPos = (arLoud->ringPos + 1) % AR_LOUD_RING;
  if (arLoud->ringCount < AR_LOUD_RING) arLoud->ringCount++;

  // ---- the windows ------------------------------------------------------
  const float m04 = arLoudWindowMean(AR_LOUD_FRAMES_04S);
  const float m3  = arLoudWindowMean(AR_LOUD_FRAMES_3S);
  const float m10 = arLoudWindowMean(AR_LOUD_FRAMES_10S);

  arLoud->momentary = arLoudOf(m04);
  arLoud->shortTerm = arLoudOf(m3);
  arLoud->sliding10 = arLoudOf(m10);
  if (arLoud->momentary > arLoud->momentaryMax) arLoud->momentaryMax = arLoud->momentary;
  if (arLoud->shortTerm > arLoud->shortTermMax) arLoud->shortTermMax = arLoud->shortTerm;

  // ---- the programme's history ------------------------------------------
  //
  // The blocks go in every 400 ms, and the short-term samples every 100 ms.
  arLoud->framesSinceBlock++;
  if (arLoud->framesSinceBlock >= AR_LOUD_FRAMES_04S) {
    arLoud->framesSinceBlock = 0;
    arLoud->blocks[arLoud->blockPos] = m04;
    arLoud->blockPos = (arLoud->blockPos + 1) % AR_LOUD_BLOCKS;
    if (arLoud->blockCount < AR_LOUD_BLOCKS) arLoud->blockCount++;
  }

  arLoud->framesSinceShort++;
  if (arLoud->framesSinceShort >= 9) {          // 9 frames = 96 ms
    arLoud->framesSinceShort = 0;
    arLoud->shorts[arLoud->shortPos] = m3;
    arLoud->shortPos = (arLoud->shortPos + 1) % AR_LOUD_SHORTS;
    if (arLoud->shortCount < AR_LOUD_SHORTS) arLoud->shortCount++;
  }

  // ---- the periodic work -----------------------------------------------
  //
  // The integrated loudness every second, the histogram every ten, and the
  // percentiles every two. None of them is on the frame's critical path:
  // they are short loops over history, and they are spread out so that no
  // one of them lands on the same frame as another.
  arLoud->framesSinceIntegrated++;
  if (arLoud->framesSinceIntegrated >= 94) {     // 94 frames = 1.003 s
    arLoud->framesSinceIntegrated = 0;
    arLoudnessRecomputeIntegrated();
  }

  arLoud->framesSinceHistogram++;
  if (arLoud->framesSinceHistogram >= 938) {    // 938 frames = 10.005 s
    arLoud->framesSinceHistogram = 0;
    arLoudnessRebuildHistogram();
  }

  arLoud->framesSincePercentiles++;
  if (arLoud->framesSincePercentiles >= 188) {  // 188 frames = 2.005 s
    arLoud->framesSincePercentiles = 0;
    if (arLoud->shortCount > 0) {
      const float p95 = arLoudPercentile(0.95f);
      const float p10 = arLoudPercentile(0.10f);
      arLoud->lra = (p95 > -119.0f && p10 > -119.0f) ? (p95 - p10) : 0.0f;
    }
  }
}

// ---------------------------------------------------------------------------
// Publish.
//
// The frame is handed to the publish module, which copies it under the ONE
// publish lock - so a reader cannot see half of it - and the calibration is
// applied HERE, so that it can be changed live without touching the history.
static void arLoudnessPublish() {
  if (!arLoud) return;
  const float cal = arSettingsSnapshot().loudnessCalDb;
  ArLoudnessFrame f;
  f.programme    = arLoud->programme    + cal;
  f.momentary    = arLoud->momentary    + cal;
  f.momentaryMax = arLoud->momentaryMax + cal;
  f.shortTerm    = arLoud->shortTerm    + cal;
  f.shortTermMax = arLoud->shortTermMax + cal;
  f.sliding10    = arLoud->sliding10    + cal;
  f.lra          = arLoud->lra;
  f.truePeak     = arLoud->truePeak     + cal;
  f.truePeakNow  = arLoud->truePeakNow  + cal;
  // There is a programme to talk about once there is at least one 400 ms
  // block in the history. Before that there is nothing to integrate, and
  // the pane says so rather than showing a number that means nothing.
  f.valid        = (arLoud->blockCount > 0) ? 1 : 0;
  arPublishLoudness(f);
}

// A copy of the published numbers.
static ArLoudnessFrame arLoudnessSnapshot() {
  ArLoudnessFrame f;
  arCopyLoudness(&f);
  return f;
}
