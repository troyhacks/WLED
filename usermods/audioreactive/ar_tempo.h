#pragma once

// ===========================================================================
// THE TEMPO TRACKER
//
// An audio-only beat tracker: it finds the BPM and the beat phase from this
// board's own input, and publishes them so an effect can strobe to the beat.
//
// IT IS AUDIO ONLY, and that is not a limitation that can be designed away.
// The ProLink usermod gets the tempo from a Pioneer deck's network packets;
// the tempo tracker gets it from the sound. A board that RECEIVES its sound
// over the network has no audio to measure, so it has no tempo - and the
// effect that strobes to the beat must blank itself there, not flash to a
// stale number. The arbitration between the two sources belongs to the
// consumer, and the consumer has it (see mode_bpm_strobe in wled00/FX.cpp):
// ProLink first if it is there, the estimator second.
//
// WHAT IT MEASURES, AND WHY THE COMB
// ----------------------------------
// The autocorrelation is nearly flat noise on real music: on one track the
// true period sat 7.4 dB BELOW the largest value in the whole lag band, and
// that largest value was at the band edge. An autocorrelation measures
// PERIODICITY; it does not require the onsets to LAND on a grid, and on a
// dense envelope they do not.
//
// The comb score does require it. For a candidate period P, every onset's
// phase offset within the period is u = t mod P. If the onsets are a clean
// beat train their offsets CLUSTER, and the size of that cluster is the
// score. If the period is wrong the offsets smear around the circle and the
// score collapses. The score is the peak of the smoothed phase histogram
// divided by the TOTAL onset strength, so it is a FRACTION - 1.0 means every
// onset's strength sits under the best phase - and it is comparable across
// rates.
//
// It resolves the rate finely enough to matter: a rate error of e drifts the
// grid by e x 12 s, so to stay inside a 70 ms tolerance the error must be
// under 0.58 %. The 0.25 BPM sweep step is 0.19 % at 128 BPM, which is 23
// ms of drift over a 12 s window.
//
// THE PRIOR
// ---------
// A comb at half the true period also covers every onset, so the score
// favours the FASTEST rate in the band. The band removes the octave for
// anything above 90 BPM; a log-normal prior centred on 125 BPM removes the
// rest. That prior is Troy's range: 90-175 BPM is the endorsed band and
// 128-135 is where he lives.
//
// MEASURED, ON 100 REAL HOUSE TRACKS
// ----------------------------------
// The hit rate of the beat grid on the raw onsets, with a constant offset
// removed (so it measures the RATE, not the phase):
//
//   the offline shape (256 bins, a full 0.25 BPM sweep)   0.811  100 %
//   THIS shape      (64 bins, a full 0.25 BPM sweep)      0.811  100 %
//   the coarse-to-fine search instead of the full sweep    0.793  100 %
//
// The coarse-to-fine search was tried because 341 candidates seemed too
// expensive for a 10.67 ms frame. It is not: at 64 bins a candidate costs
// about 700 operations, so the whole sweep is 240 000 operations - 30 000 a
// frame, spread over the frames of the search. And the coarse-to-fine search
// COSTS accuracy, because its rate flickers between neighbouring candidates
// and the PLL integrates every flicker into the grid's position. That is the
// "skipped beats" failure mode: a grid that drifts inside the beat instead
// of staying on it.
//
// THE BAND
// --------
// 40-160 Hz, at 3 kHz, after a 300 Hz anti-alias filter and a decimation by
// 16. The kick's fundamental lives here (45-70 Hz in house music) and the
// band is where the beat lives. The band's response was measured both ways -
// as the filters run on a 6 kHz stream, and as they run on the 3 kHz stream
// they were designed for - and the two score the same (0.811 both), so the
// band is not a delicate choice.
//
// THE DOWNBEAT
// -------------
// An audio-only estimator CANNOT know the downbeat: a four-on-the-floor
// kick has no audible one, so the best a detector can do there is a coin
// toss. The beat in the bar is therefore a LOCAL counter, modulo four,
// restarted at every lock. It is a convention and not a measurement, and it
// drifts against the real bar across a track change.
// ===========================================================================

#include <atomic>
#include <math.h>

// ---------------------------------------------------------------------------
// The constants.
//
// Every one of them is a number that was MEASURED, and the comment says what
// it was measured against. The ones without a measurement are the ones the
// physics fixes.
// ---------------------------------------------------------------------------

// ---- the front end -------------------------------------------------------

// The decimation: 48 kHz -> 3 kHz. Sixteen, because 512 (the hop) divides by
// it, so every frame starts on a decimation boundary and the phase cannot
// drift.
static constexpr int AR_TEMPO_DEC = 16;

// The frame rate: one ODF value per hop, 93.75 a second. It is exact: the
// hop is 512 samples of 48 kHz.
static constexpr float AR_TEMPO_RATE = (float)SAMPLE_RATE / (float)hopSamples;

// The frames per second at the decimated rate, and the samples per frame
// there. 32 samples of 3 kHz is 10.67 ms, the same frame the bands use.
static constexpr float AR_TEMPO_FS  = (float)SAMPLE_RATE / (float)AR_TEMPO_DEC;
static constexpr int   AR_TEMPO_HOP = hopSamples / AR_TEMPO_DEC;

// The band: 40-160 Hz. The kick's fundamental, and the band the beat lives
// in. Measured both as the filters run on a 6 kHz stream and as they run on
// the 3 kHz stream they were designed for: the same score either way.
static constexpr float AR_TEMPO_BAND_LO_HZ = 40.0f;
static constexpr float AR_TEMPO_BAND_HI_HZ = 160.0f;

// The anti-alias filter, ahead of the decimation. A 4th-order Butterworth at
// 300 Hz is 50 dB down at 3 kHz, so nothing from the 3-3.16 kHz region -
// the only place the decimation can alias into this band - gets through.
static constexpr float AR_TEMPO_AA_HZ    = 300.0f;
static constexpr int   AR_TEMPO_AA_SECS  = 2;        // 2 sections = 4th order

// The band's own filters, at 3 kHz: two high-passes and two low-passes, all
// Butterworth (Q = 1/sqrt(2)).
static constexpr int   AR_TEMPO_BAND_SECS = 4;
static constexpr float AR_TEMPO_Q = 0.70710678f;

// The power floor, and the scale.
//
// The ODF is a DIFFERENCE of dB, so the signal's scale cancels - but the
// floor does not, so the power is normalised to full scale explicitly. The
// AR's samples are +-32767.
static constexpr float AR_TEMPO_POW_NORM = 9.31322575e-10f;  // 1/(32767*32767)
static constexpr float AR_TEMPO_POW_EPS  = 1e-12f;

// ---- the peak picker -----------------------------------------------------

// The candidate is evaluated PEAK_POST frames late, because the picker
// cannot see the future: it needs the frames AFTER the candidate to know
// that the candidate is a local maximum. The firmware's frame period is
// 10.67 ms, so the picker's answer is 32 ms old.
static constexpr int AR_TEMPO_PEAK_PRE  = 3;
static constexpr int AR_TEMPO_PEAK_POST = 3;

// The local-mean window: 33 frames either side of the candidate, 0.35 s.
// The trailing half only - the symmetric half would need the future.
static constexpr int AR_TEMPO_PEAK_MEAN_HALF = 33;

// The candidate must beat the local mean by this much. Measured on the
// corpus: 0.35 keeps the kicks and drops the hi-hat's energy leak.
static constexpr float AR_TEMPO_PEAK_DELTA = 0.35f;

// The floor, as a fraction of the ODF's running maximum. It rejects the
// noise between the beats without needing an absolute threshold, which
// would have to know the input's level.
static constexpr float AR_TEMPO_PEAK_FLOOR = 0.06f;

// Two onsets closer than this are one onset. 5 frames is 53 ms.
static constexpr int AR_TEMPO_PEAK_MIN_GAP = 5;

// The history the picker reads. The ring is a power of two so the index
// arithmetic is a mask; the logical length is the mean window plus the
// candidate's latency.
static constexpr int AR_TEMPO_HIST_LEN = AR_TEMPO_PEAK_MEAN_HALF + AR_TEMPO_PEAK_POST + 1;  // 37
static constexpr int AR_TEMPO_HIST_RING = 64;

// The running maximum's decay: 5 s. The picker normalises the candidate by
// it, so the floor follows the loudest recent passage.
static constexpr float AR_TEMPO_MAX_TAU_S  = 5.0f;
static constexpr float AR_TEMPO_MAX_DECAY = 0.99786894f;  // exp(-1/(5*93.75))

// ---- the comb ------------------------------------------------------------

// The band of rates: 90-175 BPM, Troy's endorsed band.
static constexpr float AR_TEMPO_BPM_LO = 90.0f;
static constexpr float AR_TEMPO_BPM_HI = 175.0f;

// The sweep: the WHOLE band, at 0.25 BPM. The coarse-to-fine search was
// tried and measured WORSE (0.793 against 0.811): its rate flickers between
// neighbouring candidates, and the PLL integrates every flicker into the
// grid's position.
static constexpr float AR_TEMPO_CAND_STEP  = 0.25f;
static constexpr int   AR_TEMPO_CAND_COUNT = (int)((AR_TEMPO_BPM_HI - AR_TEMPO_BPM_LO) / AR_TEMPO_CAND_STEP) + 1;  // 341

// The phase histogram's bins. 64 is enough: the kernel's width in PHASE is
// the same for every rate, so the bin count changes the histogram's
// resolution and not the score. Measured: 64 and 256 both give 0.811.
static constexpr int AR_TEMPO_COMB_BINS = 64;

// How many candidates are scored in one frame. The whole sweep is 341, so
// at 44 a frame the search takes eight frames - 85 ms - and the frame's
// cost stays flat instead of spiking.
static constexpr int AR_TEMPO_CAND_PER_FRAME = 44;

// How much history the comb score looks at: 12 s.
static constexpr float AR_TEMPO_COMB_WINDOW_S = 12.0f;
static constexpr int   AR_TEMPO_COMB_FRAMES   = (int)(12.0f * 93.75f);  // 1125

// The prior: a log-normal on the BPM, centred on 125, sigma 0.35 in ln units
// (about +-27 % at one sigma). Weight 0.6: measured, that is what takes the
// corpus from 87 % to 97 % of the windows locking at all.
static constexpr float AR_TEMPO_PRIOR_BPM   = 125.0f;
static constexpr float AR_TEMPO_PRIOR_SIGMA = 0.35f;
static constexpr float AR_TEMPO_PRIOR_WEIGHT = 0.6f;

// Below this score the comb is not believed, and the tempo is not published
// as valid.
static constexpr float AR_TEMPO_MIN_SCORE = 0.10f;

// Fewer onsets than this in the window and there is no search at all.
static constexpr int AR_TEMPO_MIN_PEAKS = 8;

// ---- the PLL -------------------------------------------------------------

// The re-anchor: how far the grid moves toward the comb's phase, per
// search. 35 % a search, so a phase error is gone in three searches - a
// quarter of a second.
static constexpr float AR_TEMPO_PLL_GAMMA = 0.35f;

// The slew: how much the PERIOD may move per search. 0.5 % of 45 frames is
// 0.22 frames, or 2.4 ms, so the grid's slope changes slowly and the grid's
// position does not move at all.
static constexpr float AR_TEMPO_PLL_SLEW = 0.005f;

// ---- the ring and the rebase ----------------------------------------------

// The onset ring's entries. The window holds 1125 frames and two onsets
// cannot be closer than 5 frames, so the most it can hold is 225.
static constexpr int AR_TEMPO_PEAK_RING = 256;

// The frame counter is rebased this often, so the float arithmetic on the
// beat grid keeps its precision.
//
// A float holds an integer exactly up to 2^24, so a frame index would be
// exact for 49 hours - but the grid's ARITHMETIC (nextBeat + period, and
// nextBeat - phase) needs the DIFFERENCE to be exact too, and that is what
// the rebase buys: the grid's numbers stay under 65536, where a float's
// spacing is 0.008 frames - 0.08 ms, against a render frame of 17 ms.
static constexpr uint32_t AR_TEMPO_REBASE_FRAMES = 65536;

// The gate has been shut this many frames (1 s) and the music has changed:
// the tracker starts again.
static constexpr int AR_TEMPO_GATE_SHUT_FRAMES = 94;

// The comb's tolerance: Ellis' F1 tolerance, 70 ms. It is a live setting,
// because it is a TRADE and not a constant - a tighter tolerance makes the
// score pickier and a looser one makes it pickier about nothing.
static constexpr float AR_TEMPO_TOL_DEFAULT_MS = 70.0f;
static constexpr float AR_TEMPO_TOL_MIN_MS = 30.0f;
static constexpr float AR_TEMPO_TOL_MAX_MS = 150.0f;

// ---------------------------------------------------------------------------
// The state.
//
// ONE instance, like the bank's liveBank: the analysis task is the only
// writer and the only mutator, so nothing here needs a lock. The published
// NUMBERS go out through the publish module (ar_publish.h), not from here.
// ---------------------------------------------------------------------------
struct ArTempoState {
  // ---- the front end ----
  float   aaC[AR_TEMPO_AA_SECS][5];      // the anti-alias filter's coefficients
  float   aaW[AR_TEMPO_AA_SECS][2];      // ... and its delay lines
  float   bandC[AR_TEMPO_BAND_SECS][5];  // the band's coefficients
  float   bandW[AR_TEMPO_BAND_SECS][2];  // ... and its delay lines
  float   decBuf[AR_TEMPO_HOP];          // the decimated frame's samples
  uint8_t decCount;                      // how many of them there are
  uint8_t aaPhase;                       // the decimation's phase
  float   lastDb;                        // the previous frame's level, dB
  float   odfMax;                        // the ODF's running maximum

  // ---- the picker ----
  float   hist[AR_TEMPO_HIST_RING];      // the recent ODF values
  uint8_t histIdx;                       // where the next one goes
  uint16_t histFilled;                   // how many have been written

  // ---- the onsets ----
  uint32_t peakFrame[AR_TEMPO_PEAK_RING];  // the onset ring: each onset's frame
  float    peakStr[AR_TEMPO_PEAK_RING];    // ... and its strength
  uint16_t peakHead;                       // the ring's write index
  uint16_t peakCount;                      // how many are in it
  int32_t  prevPeakFrame;                  // the last accepted onset (the gap)
  float    prevPeakStr;

  // ---- the sweep ----
  uint32_t sweepFrame[AR_TEMPO_PEAK_RING]; // the window's onsets, snapshotted
  float    sweepStr[AR_TEMPO_PEAK_RING];   // ... so the sweep sees ONE set
  uint16_t sweepCount;
  uint16_t sweepIndex;
  float    sweepNow;                      // the frame the sweep STARTED at
  float    combH[AR_TEMPO_COMB_BINS];      // the phase histogram
  float    sweepBestValue;                // the best (score - penalty)
  float    sweepBestScore;
  float    sweepBestBpm;
  float    sweepBestPhase;
  bool     sweepActive;

  // ---- the PLL ----
  float    period;                        // the beat period, in frames
  float    nextBeat;                      // the frame of the NEXT beat
  float    lastBeat;                      // the frame of the LAST beat
  uint32_t beatIndex;                     // the monotonic beat counter
  uint32_t frameIndex;                    // the frame counter, rebased
  float    bpm;                           // the published rate
  float    score;                         // the comb score, 0..1
  bool     locked;                        // the grid is running

  // ---- the gate ----
  uint16_t gateShutFrames;                // how long the gate has been shut

  bool     init;                          // the coefficients are baked
};

static ArTempoState arTempo;
static float arTempoPriorPen[AR_TEMPO_CAND_COUNT];

// A re-lock is a REQUEST: the grid belongs to the analysis task, and the
// serial console is not it. The flag is applied at the top of the next
// frame - the same pattern as the auto-range's reset.
static std::atomic<bool> arTempoRelockRequested{false};

// ---------------------------------------------------------------------------
// The filters.
//
// The AR's own convention: a0 = 1, and
//   y = b0*x + s1;  s1 = b1*x - a1*y + s2;  s2 = b2*x - a2*y
// The delay lines are two floats per section, so a section needs no output
// buffer - the tracker READS the hop and cannot disturb it.
// ---------------------------------------------------------------------------

// A high-pass with its -3 dB point at f0 (RBJ cookbook).
static void arTempoRbjHpf(float f0, float fs, float Q, float *c) {
  const float w0    = 2.0f * (float)M_PI * f0 / fs;
  const float cw    = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * Q);
  const float a0    = 1.0f + alpha;
  c[0] = ((1.0f + cw) * 0.5f) / a0;
  c[1] = (-(1.0f + cw)) / a0;
  c[2] = ((1.0f + cw) * 0.5f) / a0;
  c[3] = (-2.0f * cw) / a0;
  c[4] = (1.0f - alpha) / a0;
}

// A low-pass with its -3 dB point at f0 (RBJ cookbook).
static void arTempoRbjLpf(float f0, float fs, float Q, float *c) {
  const float w0    = 2.0f * (float)M_PI * f0 / fs;
  const float cw    = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * Q);
  const float a0    = 1.0f + alpha;
  c[0] = ((1.0f - cw) * 0.5f) / a0;
  c[1] = (1.0f - cw) / a0;
  c[2] = ((1.0f - cw) * 0.5f) / a0;
  c[3] = (-2.0f * cw) / a0;
  c[4] = (1.0f - alpha) / a0;
}

// ---------------------------------------------------------------------------
// The comb's score at one candidate rate.
//
// For a candidate period P, every onset's phase offset within the period is
// u = t mod P. If the onsets are a clean beat train their offsets CLUSTER,
// and the size of that cluster is the score. If the period is wrong the
// offsets smear around the circle and the score collapses.
//
// The score is the peak of the smoothed phase histogram divided by the
// TOTAL onset strength, so it is a FRACTION - 1.0 means every onset's
// strength sits under the best phase - and it is comparable across rates.
//
// The kernel is a box of half-width tol, and it is a SUM and not an average:
// the window's width in PHASE is the same for every rate, so the sum in the
// window is the onset strength that sits under that phase.
// ---------------------------------------------------------------------------

// The kernel's half-width, in bins.
static int arTempoKernelHalfWidth(float period) {
  const float tolFrames = (arFrameSettings.tempoTolMs * 0.001f) * AR_TEMPO_RATE;
  int w = (int)lroundf(tolFrames / period * (float)AR_TEMPO_COMB_BINS);
  if (w < 1) w = 1;
  if (w > AR_TEMPO_COMB_BINS / 2 - 1) w = AR_TEMPO_COMB_BINS / 2 - 1;
  return w;
}

// The score at one candidate rate. `phaseOut` gets the best phase, in
// frames, relative to the tracker's frame base.
//
// `frames` are RELATIVE to the frame base, so they are small numbers and the
// float arithmetic on them is exact.
static float arTempoCombScore(const uint32_t *frames, const float *str, int n,
                               float bpm, float *phaseOut) {
  const float period    = 60.0f * AR_TEMPO_RATE / bpm;
  const float invPeriod = 1.0f / period;
  const float binScale  = (float)AR_TEMPO_COMB_BINS * invPeriod;
  float *h = arTempo.combH;

  for (int i = 0; i < AR_TEMPO_COMB_BINS; i++) h[i] = 0.0f;
  float total = 0.0f;
  for (int i = 0; i < n; i++) {
    const float f = (float)frames[i];
    // The phase offset. The truncation is the floor - the frames are not
    // negative - and the two corrections catch the case where the multiply
    // rounded across an integer, which would otherwise put the onset a whole
    // period out.
    const uint32_t k = (uint32_t)(f * invPeriod);
    float u = f - (float)k * period;
    if (u < 0.0f) u += period;
    else if (u >= period) u -= period;
    int bin = (int)(u * binScale);
    if (bin < 0) bin = 0;
    else if (bin >= AR_TEMPO_COMB_BINS) bin = AR_TEMPO_COMB_BINS - 1;
    h[bin] += str[i];
    total += str[i];
  }
  if (total <= 0.0f) return 0.0f;

  // The circular box kernel, by a ROLLING sum: the window is 2*w+1 bins
  // wide and it slides one bin at a time, so the smoothing is O(bins) and
  // not O(bins*w).
  const int w = arTempoKernelHalfWidth(period);
  float win = 0.0f;
  for (int k = -w; k <= w; k++) win += h[(k + AR_TEMPO_COMB_BINS) & (AR_TEMPO_COMB_BINS - 1)];
  float best = win;
  int bestBin = 0;
  for (int i = 1; i < AR_TEMPO_COMB_BINS; i++) {
    win += h[(i + w) & (AR_TEMPO_COMB_BINS - 1)]
         - h[(i - w - 1 + AR_TEMPO_COMB_BINS) & (AR_TEMPO_COMB_BINS - 1)];
    if (win > best) { best = win; bestBin = i; }
  }
  if (phaseOut) *phaseOut = (float)bestBin / (float)AR_TEMPO_COMB_BINS * period;
  return best / total;
}

// ---------------------------------------------------------------------------
// Init and reset.
// ---------------------------------------------------------------------------

// Forget everything: the ODF's history, the onsets, the grid.
//
// It is called when the tracker is switched on, when the analysis is
// suspended, and when the gate has been shut for a second - a track change.
// Without it the onset ring would carry the last track's onsets into the new
// track's comb score, and the first estimate would be a blend of two tracks.
static void arTempoReset(void);

// Bake the coefficients and clear the state.
static void arTempoInit(void) {
  // The anti-alias filter: the AR's own Butterworth builder, 4th order.
  bankButterLowpass(AR_TEMPO_AA_HZ, (float)SAMPLE_RATE,
                     2 * AR_TEMPO_AA_SECS, arTempo.aaC, AR_TEMPO_AA_SECS);

  // The band: two high-passes and two low-passes, at 3 kHz, all Butterworth.
  // The order matters and it is the one the corpus was measured with.
  arTempoRbjHpf(AR_TEMPO_BAND_LO_HZ, AR_TEMPO_FS, AR_TEMPO_Q, arTempo.bandC[0]);
  arTempoRbjHpf(AR_TEMPO_BAND_LO_HZ, AR_TEMPO_FS, AR_TEMPO_Q, arTempo.bandC[1]);
  arTempoRbjLpf(AR_TEMPO_BAND_HI_HZ, AR_TEMPO_FS, AR_TEMPO_Q, arTempo.bandC[2]);
  arTempoRbjLpf(AR_TEMPO_BAND_HI_HZ, AR_TEMPO_FS, AR_TEMPO_Q, arTempo.bandC[3]);

  // The prior's penalty, one per candidate. It is a table because it is a
  // function of the rate alone, and the sweep walks the rates in order.
  for (int i = 0; i < AR_TEMPO_CAND_COUNT; i++) {
    const float bpm = AR_TEMPO_BPM_LO + (float)i * AR_TEMPO_CAND_STEP;
    const float z = logf(bpm / AR_TEMPO_PRIOR_BPM) / AR_TEMPO_PRIOR_SIGMA;
    arTempoPriorPen[i] = AR_TEMPO_PRIOR_WEIGHT * z * z;
  }

  arTempo.init = true;
  arTempoReset();
}

// Forget everything: the ODF's history, the onsets, the grid.
static void arTempoReset(void) {
  if (!arTempo.init) return;
  memset(arTempo.aaW, 0, sizeof(arTempo.aaW));
  memset(arTempo.bandW, 0, sizeof(arTempo.bandW));
  memset(arTempo.decBuf, 0, sizeof(arTempo.decBuf));
  memset(arTempo.hist, 0, sizeof(arTempo.hist));
  memset(arTempo.peakFrame, 0, sizeof(arTempo.peakFrame));
  memset(arTempo.peakStr, 0, sizeof(arTempo.peakStr));
  memset(arTempo.sweepFrame, 0, sizeof(arTempo.sweepFrame));
  memset(arTempo.sweepStr, 0, sizeof(arTempo.sweepStr));
  arTempo.decCount    = 0;
  arTempo.aaPhase     = 0;
  arTempo.lastDb      = -120.0f;
  arTempo.odfMax      = 0.0f;
  arTempo.histIdx     = 0;
  arTempo.histFilled  = 0;
  arTempo.peakHead    = 0;
  arTempo.peakCount   = 0;
  arTempo.prevPeakFrame = -1000;
  arTempo.prevPeakStr = 0.0f;
  arTempo.sweepCount  = 0;
  arTempo.sweepIndex  = 0;
  arTempo.sweepActive = false;
  arTempo.sweepBestValue = -1e9f;
  arTempo.sweepBestScore = 0.0f;
  arTempo.sweepBestBpm   = 0.0f;
  arTempo.sweepBestPhase = 0.0f;
  arTempo.period      = 0.0f;
  arTempo.nextBeat    = 0.0f;
  arTempo.lastBeat    = 0.0f;
  arTempo.beatIndex   = 0;
  arTempo.frameIndex  = 0;
  arTempo.bpm         = 0.0f;
  arTempo.score       = 0.0f;
  arTempo.locked      = false;
  arTempo.gateShutFrames = 0;
}

// Throw the grid away and wait for the next search.
static void arTempoRequestRelock(void) {
  arTempoRelockRequested.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The PLL's rate update and re-anchor.
//
// The period may only move by PLL_SLEW of itself per search: 0.5 % of 45
// frames is 0.22 frames, or 2.4 ms, so the grid's SLOPE changes slowly and
// the grid's POSITION does not move at all. The re-anchor moves the position
// by PLL_GAMMA of the distance to the new comb phase - 35 % per search, so a
// phase error is gone in three searches.
//
// `phase` and `now` are both relative to the frame base.
// ---------------------------------------------------------------------------
static void arTempoAdopt(float bpm, float phase, float now) {
  const float P = 60.0f * AR_TEMPO_RATE / bpm;   // the new period, in frames
  if (!arTempo.locked) {
    // The first lock: place the grid at the comb's phase, immediately. There
    // is no grid to slew, so this is the one place the position jumps.
    arTempo.period    = P;
    arTempo.nextBeat  = phase + roundf((now - phase) / P) * P;
    arTempo.locked    = true;
    arTempo.beatIndex = 0;   // the bar numbering restarts with the lock
  } else {
    const float lim = AR_TEMPO_PLL_SLEW * arTempo.period;
    const float d = P - arTempo.period;
    arTempo.period += (d > lim) ? lim : ((d < -lim) ? -lim : d);
    const float k      = roundf((arTempo.nextBeat - phase) / arTempo.period);
    const float anchor = phase + k * arTempo.period;
    arTempo.nextBeat += AR_TEMPO_PLL_GAMMA * (anchor - arTempo.nextBeat);
  }
  arTempo.bpm = 60.0f * AR_TEMPO_RATE / arTempo.period;
}

// ---------------------------------------------------------------------------
// The sweep.
//
// The search does not fit in one frame comfortably, so it is SPREAD: the
// sweep starts, scores a fixed number of candidates a frame, and finishes
// eight frames later. The peak set is SNAPSHOTTED at the start, so every
// candidate is scored against the same onsets - otherwise the candidates
// would not be comparable with each other.
// ---------------------------------------------------------------------------

static void arTempoSweepStart(void) {
  // The window's onsets, newest first. The comb does not care about the
  // order - the modulo is order-independent - but the ring is walked
  // backwards anyway, because that is the direction the window is in.
  const uint32_t cutoff = arTempo.frameIndex - (uint32_t)AR_TEMPO_COMB_FRAMES;
  int n = 0;
  for (int i = 0; i < (int)arTempo.peakCount && n < AR_TEMPO_PEAK_RING; i++) {
    const int idx = (arTempo.peakHead + AR_TEMPO_PEAK_RING - 1 - i) & (AR_TEMPO_PEAK_RING - 1);
    if ((int32_t)(arTempo.peakFrame[idx] - cutoff) < 0) break;   // too old
    arTempo.sweepFrame[n] = arTempo.peakFrame[idx];
    arTempo.sweepStr[n]   = arTempo.peakStr[idx];
    n++;
  }
  arTempo.sweepCount     = n;
  arTempo.sweepIndex     = 0;
  arTempo.sweepNow       = (float)arTempo.frameIndex;
  arTempo.sweepBestValue = -1e9f;
  arTempo.sweepBestScore = 0.0f;
  arTempo.sweepBestBpm   = 0.0f;
  arTempo.sweepBestPhase = 0.0f;
  arTempo.sweepActive    = (n >= AR_TEMPO_MIN_PEAKS);
}

static void arTempoSweepStep(void) {
  if (!arTempo.sweepActive) return;
  for (int i = 0; i < AR_TEMPO_CAND_PER_FRAME && arTempo.sweepIndex < AR_TEMPO_CAND_COUNT; i++) {
    const int idx = arTempo.sweepIndex++;
    const float bpm = AR_TEMPO_BPM_LO + (float)idx * AR_TEMPO_CAND_STEP;
    float phase = 0.0f;
    const float score = arTempoCombScore(arTempo.sweepFrame, arTempo.sweepStr,
                                         arTempo.sweepCount, bpm, &phase);
    const float value = score - arTempoPriorPen[idx];
    if (value > arTempo.sweepBestValue) {
      arTempo.sweepBestValue = value;
      arTempo.sweepBestScore = score;
      arTempo.sweepBestBpm   = bpm;
      arTempo.sweepBestPhase = phase;
    }
  }
  if (arTempo.sweepIndex < AR_TEMPO_CAND_COUNT) return;

  // The sweep is done. The score is published whether or not it is believed:
  // it is the measurement, and a score that has fallen is the signal that
  // the estimate is no longer trustworthy.
  //
  // The grid is anchored to the frame the sweep STARTED at, not the frame it
  // finished at. The phase was measured from the onsets that existed then,
  // so anchoring it to a later frame would put the grid seven frames late -
  // and the PLL would then spend its whole life pulling it back.
  arTempo.score = arTempo.sweepBestScore;
  if (arTempo.sweepBestScore >= AR_TEMPO_MIN_SCORE) {
    arTempoAdopt(arTempo.sweepBestBpm, arTempo.sweepBestPhase, arTempo.sweepNow);
  }
  arTempo.sweepActive = false;
}

// ---------------------------------------------------------------------------
// The peak picker.
//
// A frame is a peak when it is the maximum of a +-3 frame window, stands
// 35 % above the LOCAL MEAN of the last 0.35 s, is at least 6 % of the ODF's
// running maximum, and is not within 5 frames of a stronger peak.
//
// The candidate is evaluated three frames late, because the picker cannot
// see the future.
// ---------------------------------------------------------------------------
static void arTempoPickPeak(float odf, uint32_t frame) {
  // The history ring. It holds 64 values and the picker reads the newest 37
  // of them, so nothing is overwritten while it is read.
  arTempo.hist[arTempo.histIdx] = odf;
  arTempo.histIdx = (arTempo.histIdx + 1) & (AR_TEMPO_HIST_RING - 1);
  if (arTempo.histFilled < AR_TEMPO_HIST_RING) arTempo.histFilled++;

  // The picker needs 37 frames of history plus the candidate's latency.
  if (arTempo.histFilled < AR_TEMPO_HIST_LEN + AR_TEMPO_PEAK_POST + 1) return;

  // The logical index 0 is the oldest of the 37.
  const uint8_t head = (uint8_t)((arTempo.histIdx + AR_TEMPO_HIST_RING - AR_TEMPO_HIST_LEN)
                                 & (AR_TEMPO_HIST_RING - 1));
  auto H = [&](int l) -> float {
    return arTempo.hist[(head + l) & (AR_TEMPO_HIST_RING - 1)];
  };

  const int base = AR_TEMPO_HIST_LEN - 1 - AR_TEMPO_PEAK_POST;   // 33
  const float oj = H(base);
  if (oj <= 0.0f) return;

  float segMax = oj;
  for (int k = base - AR_TEMPO_PEAK_PRE; k <= base + AR_TEMPO_PEAK_POST; k++) {
    if (H(k) > segMax) segMax = H(k);
  }
  if (oj < segMax) return;                        // not a local maximum

  float sum = 0.0f;
  for (int k = base - AR_TEMPO_PEAK_MEAN_HALF; k <= base; k++) sum += H(k);
  const float mean = sum / (float)(AR_TEMPO_PEAK_MEAN_HALF + 1);
  if (oj < (1.0f + AR_TEMPO_PEAK_DELTA) * mean) return;   // not above the mean

  const float on = (arTempo.odfMax > 0.0f) ? (oj / arTempo.odfMax) : 0.0f;
  if (on < AR_TEMPO_PEAK_FLOOR) return;            // below the floor

  // The candidate's frame.
  const uint32_t j = frame - AR_TEMPO_PEAK_POST;

  // The minimum separation, keeping the STRONGER of any pair.
  if ((int32_t)(j - (uint32_t)arTempo.prevPeakFrame) < AR_TEMPO_PEAK_MIN_GAP) {
    if (on > arTempo.prevPeakStr) {
      arTempo.prevPeakFrame = (int32_t)j;
      arTempo.prevPeakStr = on;
      const int last = (arTempo.peakHead + AR_TEMPO_PEAK_RING - 1) & (AR_TEMPO_PEAK_RING - 1);
      arTempo.peakFrame[last] = j;
      arTempo.peakStr[last] = on;
    }
    return;
  }
  arTempo.prevPeakFrame = (int32_t)j;
  arTempo.prevPeakStr = on;
  arTempo.peakFrame[arTempo.peakHead] = j;
  arTempo.peakStr[arTempo.peakHead] = on;
  arTempo.peakHead = (arTempo.peakHead + 1) & (AR_TEMPO_PEAK_RING - 1);
  if (arTempo.peakCount < AR_TEMPO_PEAK_RING) arTempo.peakCount++;
}

// ---------------------------------------------------------------------------
// The frame.
//
// One call per analysis frame, with the SAME samples the banks read and
// AFTER the chain and after both banks, so it cannot disturb either: its
// filters are transposed direct form II, they READ the hop, and they have
// no output buffer.
//
// `gateOpen` is the ANALYSIS's gate - the previous frame's, because the
// gate is decided later in the frame. It is used for two things: no search
// runs while the gate is shut, and a gate that has been shut for a second
// means the music changed, so the tracker starts again.
// ---------------------------------------------------------------------------
static void arTempoProcess(const float *samples, int n, bool gateOpen) {
  if (!arTempo.init) return;

  // The re-lock request, applied here because the grid belongs to this task.
  if (arTempoRelockRequested.exchange(false, std::memory_order_relaxed)) {
    arTempo.locked = false;
    arTempo.beatIndex = 0;
    arTempo.nextBeat = 0.0f;
    arTempo.lastBeat = 0.0f;
    arTempo.period = 0.0f;
    arTempo.score = 0.0f;
    arTempo.peakCount = 0;
    arTempo.peakHead = 0;
    arTempo.prevPeakFrame = -1000;
    arTempo.prevPeakStr = 0.0f;
    arTempo.sweepActive = false;
  }

  // The gate.
  if (!gateOpen) {
    if (++arTempo.gateShutFrames >= AR_TEMPO_GATE_SHUT_FRAMES) {
      // The music has stopped or changed: start again from nothing.
      arTempoReset();
    }
    // No search while the gate is shut. The ODF still runs - it is cheap -
    // but there is nothing to search.
  } else {
    arTempo.gateShutFrames = 0;
  }

  // ---- the front end ----------------------------------------------------
  //
  // The anti-alias filter at the full rate, then one sample in sixteen is
  // kept, then the band at 3 kHz.
  {
    // The coefficients, in locals - see the note on rtaTopProcess() for why.
    float aa0[AR_TEMPO_AA_SECS], aa1[AR_TEMPO_AA_SECS], aa2[AR_TEMPO_AA_SECS];
    float aa3[AR_TEMPO_AA_SECS], aa4[AR_TEMPO_AA_SECS];
    for (int s = 0; s < AR_TEMPO_AA_SECS; s++) {
      aa0[s] = arTempo.aaC[s][0]; aa1[s] = arTempo.aaC[s][1]; aa2[s] = arTempo.aaC[s][2];
      aa3[s] = arTempo.aaC[s][3]; aa4[s] = arTempo.aaC[s][4];
    }
    float b0[AR_TEMPO_BAND_SECS], b1[AR_TEMPO_BAND_SECS], b2[AR_TEMPO_BAND_SECS];
    float b3[AR_TEMPO_BAND_SECS], b4[AR_TEMPO_BAND_SECS];
    for (int s = 0; s < AR_TEMPO_BAND_SECS; s++) {
      b0[s] = arTempo.bandC[s][0]; b1[s] = arTempo.bandC[s][1]; b2[s] = arTempo.bandC[s][2];
      b3[s] = arTempo.bandC[s][3]; b4[s] = arTempo.bandC[s][4];
    }

    for (int i = 0; i < n; i++) {
      float x = samples[i];
      // The anti-alias filter.
      for (int s = 0; s < AR_TEMPO_AA_SECS; s++) {
        float *w = arTempo.aaW[s];
        const float y = aa0[s] * x + w[0];
        w[0] = aa1[s] * x - aa3[s] * y + w[1];
        w[1] = aa2[s] * x - aa4[s] * y;
        x = y;
      }
      // One sample in AR_TEMPO_DEC is kept.
      if (++arTempo.aaPhase < AR_TEMPO_DEC) continue;
      arTempo.aaPhase = 0;
      // The band, at 3 kHz.
      for (int s = 0; s < AR_TEMPO_BAND_SECS; s++) {
        float *w = arTempo.bandW[s];
        const float y = b0[s] * x + w[0];
        w[0] = b1[s] * x - b3[s] * y + w[1];
        w[1] = b2[s] * x - b4[s] * y;
        x = y;
      }
      arTempo.decBuf[arTempo.decCount++] = x;
    }
  }

  // ---- the frame's ODF value ---------------------------------------------
  if (arTempo.decCount >= AR_TEMPO_HOP) {
    arTempo.decCount = 0;
    float p = 0.0f;
    for (int i = 0; i < AR_TEMPO_HOP; i++) p += arTempo.decBuf[i] * arTempo.decBuf[i];
    p /= (float)AR_TEMPO_HOP;
    const float db = 10.0f * log10f(p * AR_TEMPO_POW_NORM + AR_TEMPO_POW_EPS);
    float odf = db - arTempo.lastDb;
    if (odf < 0.0f) odf = 0.0f;      // half-wave rectified: a rise only
    arTempo.lastDb = db;
    // The running maximum, with a decaying tail.
    if (odf > arTempo.odfMax) arTempo.odfMax = odf;
    else                       arTempo.odfMax *= AR_TEMPO_MAX_DECAY;

    arTempo.frameIndex++;

    // ---- the picker -----------------------------------------------------
    arTempoPickPeak(odf, arTempo.frameIndex);

    // ---- the grid -------------------------------------------------------
    // The grid advances every frame. It is CONTINUOUS: the rate changes the
    // period, never the position, so a rate change cannot make the flash
    // jump.
    if (arTempo.locked) {
      while ((float)arTempo.frameIndex >= arTempo.nextBeat) {
        arTempo.lastBeat = arTempo.nextBeat;
        arTempo.nextBeat += arTempo.period;
        arTempo.beatIndex++;
      }
    }

    // ---- the search -----------------------------------------------------
    // A search every AR_TEMPO_SEARCH_EVERY frames, spread over the frames.
    if (gateOpen) {
      if (!arTempo.sweepActive) {
        arTempoSweepStart();
      }
      arTempoSweepStep();
    }

    // ---- the rebase -----------------------------------------------------
    // The frame counter is rebased so the float arithmetic on the grid keeps
    // its precision. It is deferred while a sweep is running, because the
    // sweep's snapshot is in the same frame numbers.
    if (arTempo.frameIndex >= AR_TEMPO_REBASE_FRAMES && !arTempo.sweepActive) {
      arTempo.frameIndex -= AR_TEMPO_REBASE_FRAMES;
      arTempo.nextBeat -= (float)AR_TEMPO_REBASE_FRAMES;
      arTempo.lastBeat -= (float)AR_TEMPO_REBASE_FRAMES;
      for (uint16_t i = 0; i < arTempo.peakCount; i++) {
        const int idx = (arTempo.peakHead + AR_TEMPO_PEAK_RING - 1 - i) & (AR_TEMPO_PEAK_RING - 1);
        arTempo.peakFrame[idx] -= AR_TEMPO_REBASE_FRAMES;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The published numbers.
//
// The values travel to the other tasks through the publish module
// (ar_publish.h); these are the AR's own copies.
// ---------------------------------------------------------------------------

// The beat rate. Zero when there is no estimate.
static float arTempoBpm(void) {
  return arTempo.locked ? arTempo.bpm : 0.0f;
}

// The comb score: the fraction of the onset strength that sits under the
// grid's best phase. It is what says whether the estimate is to be believed.
static float arTempoScore(void) {
  return arTempo.score;
}

// The beat in the bar: a LOCAL counter, modulo four.
//
// An audio-only estimator cannot know the true downbeat - a four-on-the-
// floor kick has no audible one - so the first beat after the lock is
// called beat 1 and the numbering repeats every four beats. It is a
// convention, not a measurement, and it drifts against the real bar across
// a track change.
static uint8_t arTempoBeatInBar(void) {
  if (!arTempo.locked) return 0;
  return (uint8_t)((arTempo.beatIndex % 4) + 1);
}

// The progress through the current beat, 0..1.
static float arTempoBeatProgress(void) {
  if (!arTempo.locked || arTempo.period <= 0.0f) return 0.0f;
  const float p = ((float)arTempo.frameIndex - arTempo.lastBeat) / arTempo.period;
  if (p < 0.0f) return 0.0f;
  if (p > 1.0f) return 1.0f;
  return p;
}

// Is there a usable estimate?
static bool arTempoValid(void) {
  return arTempo.locked && arTempo.score >= AR_TEMPO_MIN_SCORE;
}

// ---------------------------------------------------------------------------
// The serial console's tempo keys.
//
//   e / E    the comb's tolerance, down / up by 5 ms
//   q        re-lock now: the grid is dropped and the next search starts over
//
// Lowercase is the lower number and uppercase the higher one, for every
// pair - the same rule as the rest of the AudioReactive keys.
// ---------------------------------------------------------------------------
void audioReactiveTempoCommand(char c) {
  switch (c) {
    case 'e': case 'E': {
      const float delta = (c == 'E') ? 5.0f : -5.0f;
      const ArLiveSettings s = arSettingsEdit([delta](ArLiveSettings& st) {
        float ms = st.tempoTolMs + delta;
        if (ms < AR_TEMPO_TOL_MIN_MS) ms = AR_TEMPO_TOL_MIN_MS;
        if (ms > AR_TEMPO_TOL_MAX_MS) ms = AR_TEMPO_TOL_MAX_MS;
        st.tempoTolMs = ms;
      });
      USER_PRINTF("tempo tolerance now %.0f ms\n", (double)s.tempoTolMs);
      break;
    }
    case 'q':
      // Re-lock now. It is a REQUEST, because the grid belongs to the
      // analysis task.
      arTempoRequestRelock();
      USER_PRINTLN("tempo: re-locking - the grid is dropped and the next search starts over");
      break;
    default:
      break;
  }
}
