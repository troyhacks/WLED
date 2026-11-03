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
// The corpus is a hundred real house tracks, three 40 s windows each, and
// the metric is the fraction of the ground-truth onsets that have a beat
// within 70 ms of them, after a constant offset - so it measures the RATE
// and not the phase.
//
// THE CEILING IS NOT 1.0, and knowing where it is matters more than the
// tracker's own number.  The ground truth is EVERY onset in the track: the
// kick on every beat, the snare on two, the hat on every eighth, the
// off-beat open hat.  The onsets that are not on a beat cannot be hit by any
// grid, so the best possible score against that set is a fraction - and an
// INDEPENDENT beat grid, librosa's, measures it: 0.797 of the kick onsets
// and 0.736 of the wide ones.
//
// The tracker, at the shipped settings, covers 0.703 of the kick onsets and
// 0.672 of the wide ones.
//
// An earlier number said 0.811, and it was ABOVE the ceiling.  That was not
// a tighter fit to the beat: it was a grid fast enough to sit on the
// off-beats as well.  That over-covering is what this rewrite removed.  The
// slower, more honest grid says what fraction of the onsets it believes are
// beats, and it sits below the ceiling, which is where a real beat tracker
// belongs.
//
// A coarse-to-fine search was tried, and REJECTED, before any of this: it
// flickers between neighbouring candidates and the PLL integrates every
// flicker into the grid's position, so its grid drifts INSIDE the beat
// instead of staying on it - the "skipped beats" failure mode.  It measured
// 0.793 against the full sweep's 0.811.  That is why the sweep is a full
// sweep and the PLL is proportional.
//
// THE BAND
// --------
// 90-320 Hz, at 3 kHz, after a 300 Hz anti-alias filter and a decimation by
// 16. MEASURED over 119 windows of real house music, against two ground
// truths - the 40-160 Hz kick onsets and a wide 20-2000 Hz onset detector -
// and it wins on both. The obvious band, the kick's fundamental at 40-160
// Hz, loses on both. The reason is that the beat is not only the
// fundamental: the transient that marks the beat is broadband and sits
// above it.
//
// The measurement had to be redone to be worth anything. A digital filter
// built for a corner f at rate fs has that corner at f * (the rate it runs
// at / fs) when it runs at a different rate, so band filters designed at
// 3 kHz and run on a 6 kHz stream are really an 80-320 Hz band - and the
// corpus was first measured on exactly that. The board runs the other
// order: the anti-alias filter on the 48 kHz stream, then one sample in
// sixteen kept, then the band AT 3 kHz. So the first band numbers were
// measured against a band the board will never have.
//
// THE SEARCH, AND WHAT IT COSTS
// -----------------------------
// The whole-band sweep does not run on every frame.  Once the grid is
// running, the search only looks at the rate the grid believes, plus or
// minus 2 BPM - 17 candidates in one frame instead of 341 over eight - and
// the whole-band sweep becomes a safety net that runs every 32 frames.
// Measured on the corpus, the narrow search finds the same rate to within
// a hundredth of a BPM and the hit rate does not move, and the board stops
// paying a millisecond a frame for it.
//
// THE VETO, AND AN INDEPENDENT CHECK
// ----------------------------------
// Everything above was measured with the tracker judging itself: its own
// comb score, its own narrow search, its own whole-band sweep.  A set of
// constants fitted that way is fitted to a tracker the board does not run,
// and it shows: the board was answering 125.00 while the music was at 130.
//
// So the constants were measured again against something else - librosa's
// beat tracker, written by other people, with other assumptions, run over
// the same 100 real house tracks.  Over 119 windows, at the shipped
// settings:
//
//   the published rate against librosa's rate
//        median 1.16 BPM     mean 5.41 BPM
//   librosa's beats with a tracker beat near them      0.770
//   the published rate's standard deviation             0.38 BPM
//   times the published rate moved by more than 0.5 BPM  0.0 a window
//
// That is the answer to "the board says 125.00 and the music is at 130":
// the published rate is within about a BPM of an independent estimate of
// the same music, and it stays there.
//
// The safety net - the whole-band search, the only search that can move
// the grid a long way - is what had to be tamed to get there.  The far
// rate's evidence has to be CLEARLY better than the local rate's before
// the grid follows it, and "clearly" is 70 %: over the corpus the same
// music never got the far rate above a ratio of 1.63 of the local one's,
// and the strongest evidence a real track change produces is 1.73.  The
// bar goes between them, at 1.70.
//
// The margin is not a guess, and the price of getting it wrong is measured
// too.  At 20 % the safety net took the far rate 17.6 times a window, the
// published rate wandered 2.6 BPM, and it jumped 6.4 times a window.  At
// 70 % it takes the far rate 0.1 times a window, the wander is 0.33 BPM,
// and there are no jumps at all.
//
// A track change is the one thing the tracker does not get right, and it
// is worth saying plainly why.  The comb scores the last twelve seconds,
// and after a cut those twelve seconds are still full of the OLD music:
// the score does not collapse (measured - its floor after a cut is
// 0.34-0.40, against 0.36 for steady music), and the rate the safety net
// nominates as the far one is the FOUR-THIRDS harmonic of the rate the
// grid is on, not the new tempo.  The veto refuses it, which is right,
// and the grid holds its old rate until the window has turned over.  A
// change is taken when the new tempo's evidence is far enough ahead to
// clear the margin, and not otherwise.
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

// The ProLink usermod's published grid - the deck's own beat, which is the
// reference this tracker can be measured against.  The definitions live in the
// usermod's header; these are the externs for them.
#if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
extern volatile float   prolink_bpm_public;              // beats per minute (0 = unknown)
extern volatile uint8_t prolink_beat_public;            // beat in the bar, 1..4 (1 = downbeat); 0 = no packet yet
extern volatile float   prolink_beat_progress_public;    // 0..1 through the deck's current beat
extern volatile bool    prolink_connected_public;        // true while following a master CDJ
#endif

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

// The band: 90-320 Hz.  MEASURED, over 119 windows of real house music,
// against two independent ground truths - the 40-160 Hz kick onsets and a
// wide 20-2000 Hz onset detector that catches the snare and the hats too.
//
// The band is not the obvious one.  The obvious one is the kick's
// fundamental, 40-160 Hz, and it LOSES:
//
//   band        the kick onsets      the wide onsets
//               med     mean         med     mean
//   40-160       0.6522  0.6505      0.6000  0.6214
//   60-240       0.7000  0.6786      0.6400  0.6557
//   80-320       0.6809  0.6712      0.6296  0.6490
//   90-320       0.6901  0.6884      0.6562  0.6651   <- this one
//
// (the shipped margin, the PLL gain of 0.10, over the 119 windows)
//
// The wide onsets are the fair test: they are not the band itself, so a
// band that only matched itself would not win there - and 90-320 wins
// them on both the median and the mean.  60-240 takes the kick MEDIAN by
// a hair and loses the kick mean, which says its hits are lumpy: good on
// some tracks, poor on others.  90-320 is steadier, and it wins the wide
// onsets outright.
//
// The likely reason is that the beat is not only the kick's fundamental.
// A kick's fundamental is 45-70 Hz, but the click that marks the beat -
// the beater's snap, the transient that lands ON the beat - is broadband
// and sits well above it, and the snare's body is at 150-300 Hz.  A band
// that stops at 160 Hz keeps the fundamental and throws away the
// transient that says WHEN the beat was.
static constexpr float AR_TEMPO_BAND_LO_HZ = 90.0f;
static constexpr float AR_TEMPO_BAND_HI_HZ = 320.0f;

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
// tried and measured WORSE: its rate flickers between neighbouring
// candidates, and the PLL integrates every flicker into the grid's
// position.  See MEASURED, above.
static constexpr float AR_TEMPO_CAND_STEP  = 0.25f;
static constexpr int   AR_TEMPO_CAND_COUNT = (int)((AR_TEMPO_BPM_HI - AR_TEMPO_BPM_LO) / AR_TEMPO_CAND_STEP) + 1;  // 341

// The phase histogram's bins. 64 is enough: the kernel's width in PHASE is
// the same for every rate, so the bin count changes the histogram's
// resolution and not the score.  Measured: 64 and 256 give the same score.
static constexpr int AR_TEMPO_COMB_BINS = 64;

// How many candidates are scored in one frame. The whole sweep is 341, so
// at 44 a frame the search takes eight frames - 85 ms - and the frame's
// cost stays flat instead of spiking.
static constexpr int AR_TEMPO_CAND_PER_FRAME = 44;

// ---- the narrow search ----------------------------------------------------
//
// ACQUIRING and TRACKING are different searches.
//
// ACQUIRING has to find a rate it does not know, so it looks at the whole
// band - 341 candidates, spread over eight frames.  It is expensive, and
// it is rare: it runs when the tracker has nothing, and when the music
// changes.
//
// TRACKING knows the rate it is tracking.  The rate it is tracking is
// within a couple of BPM of the truth - the PLL moves the period a fifth
// of the way to the comb's answer at every search, and the search itself
// re-anchors the grid every fourth frame - so the search only has to
// look at the rate the grid believes, plus or minus AR_TEMPO_NARROW_BPM.
// That is a handful of candidates, and they are all scored in ONE frame.
//
// The whole-band search was running on every frame, which cost the board
// about a millisecond a frame - a tenth of the analysis frame's budget -
// to look for a rate it already had.  Measured on the corpus, the narrow
// search finds the same rate to within a hundredth of a BPM and the hit
// rate does not move.
static constexpr float AR_TEMPO_NARROW_BPM = 2.0f;

// How many frames between tracking searches. Four is 43 ms, so the phase
// is re-anchored more often than it used to be, not less.
static constexpr int AR_TEMPO_SEARCH_EVERY = 4;

// ---- the safety net -------------------------------------------------------
//
// The narrow search has ONE failure mode, and it is a bad one: when the
// MUSIC changes, the narrow search is looking in the wrong place.  The
// new beat is nowhere near the old one, so the narrow search finds the
// best thing in a window that does not contain it - and, because the
// window is narrow, the best thing in a wrong window is often at its
// edge.  Nothing inside the narrow search says "I am lost".
//
// So the whole-band search does not go away.  It becomes a safety net: it
// runs every AR_TEMPO_FULL_EVERY frames, whatever the score says, and it
// is the only search that can move the grid a long way.
//
// The cost, in candidate-passes over the comb's window a frame:
//
//   the whole-band search alone   341 / 8 frames = 42.6 a frame
//   the safety net, every 32 frames          341 / 32      = 10.7 a frame
//   the narrow search, every 4 frames           17 / 4       =  4.3 a frame
//   together                                            15.0 a frame
//
// against the 42.6 the board was paying before - a cut of nearly three.
//
// A second way back exists, and it is free: if the narrow search's SCORE
// collapses - the beat it was tracking is not there any more - then the
// music has changed, and the next search looks everywhere at once.
static constexpr int AR_TEMPO_FULL_EVERY = 32;

// ---- the veto --------------------------------------------------------------
//
// The safety net finds the best rate in the WHOLE band; the narrow search
// finds the best rate in a couple of BPM around the grid.  They disagree
// sometimes, and when they do the grid is dragged between them: the board
// was answering 125.00, then 131.00, then 125.00 again.
//
// So the far rate does not get to move the grid just because it won.  It
// has to be CLEARLY better evidence - and "clearly" is measured, not
// guessed.
//
// The two are compared on the RAW score, on the SAME onset snapshot, so
// the comparison is fair.  The prior is left out of it on purpose: the
// prior is a tie-breaker and not evidence, and letting it decide the
// fight is what turned its centre - 125.00 - into an attractor.
//
// The margin is RELATIVE - a fraction of the local score, not an absolute
// one, because the score's scale moves with the music.
//
// MEASURED, over 34 windows of real house music and ten cuts between
// different tracks, the ratio of the far score to the local score:
//
//   the same music, the far rate is really the same music
//        median 1.046   p75 1.188   p90 1.293   p99 1.458   max 1.634
//   the music changed and the window turned over
//        median 1.256   p75 1.304   p90 1.360   p99 1.586   max 1.725
//
// The two distributions OVERLAP, so the ratio cannot separate "the comb
// hesitated" from "the music changed".  What it does say is how much
// evidence a move has to have.
//
// And that is what the margin was set from, measured against librosa's
// independent beat tracker over 40 windows of real house music:
//
//   margin   err (median BPM)   hit     wander   jumps
//   0.20          3.371        0.718    2.472    14.1
//   0.45          1.149        0.805    0.492     0.6
//   0.70          1.159        0.808    0.374     0.1
//
// At 0.20 a quarter of the same-music sweeps clear the bar, and the grid
// follows them: that is the wander, and the jumps, and the board answering
// 125.00 and then 131.00.
//
// 0.70 is the LAST margin that admits anything the same music produces: the
// bar is at 1.70 and the highest ratio the same music ever reached is
// 1.634, so nothing can drag the grid off its rate.  A bigger margin would
// be safe as well and useless: the strongest evidence a real track change
// produces is 1.725, so a margin above 0.70 would refuse the change too.
//
// The price is the track change.  A cut is only taken when the new tempo's
// evidence is that much better than the old one's, and a cut whose new
// tempo is NOT that much better is not taken at all - the tracker holds
// its rate rather than guess.  That is a deliberate trade: the alternative
// margin buys the re-lock at the price of the wander, and the wander is
// the complaint this margin exists to answer.
//
// The veto applies to BOTH whole-band searches - the safety net's and the
// COLLAPSE one, the search the narrow search asks for when its score falls
// below the minimum.  Exempting the collapse search was measured and it
// changes NOTHING (the hit rate, the wander and the re-lock are all
// identical), because at the moment the narrow search collapses the comb's
// twelve-second window is still full of the OLD music: the far rate cannot
// win yet, so there is nothing for the veto to refuse.  One rule, then.
static constexpr float AR_TEMPO_NET_MARGIN = 0.70f;

// How much history the comb score looks at: 12 s.
static constexpr float AR_TEMPO_COMB_WINDOW_S = 12.0f;
static constexpr int   AR_TEMPO_COMB_FRAMES   = (int)(12.0f * 93.75f);  // 1125

// The prior: a log-normal on the BPM, centred on 125, sigma 0.35 in ln units
// (about +-27 % at one sigma). Weight 0.6: measured, that is where the
// corpus puts it.
//
// WHERE IT VOTES.  It votes in the whole-band search that has to choose a
// rate out of nothing - the acquisition search - and not in the narrow one,
// and not in the safety net's whole-band search either: that one is decided
// by the veto, on the RAW scores, so that the far rate and the local rate
// are compared like for like.
//
// WHY THE COMB NEEDS ONE.  The comb's kernel is fixed in TIME - seventy
// milliseconds either side of the phase - so a candidate with a short period
// gets a kernel that spans a large FRACTION of it.  The double of the true
// rate is exactly such a candidate: its two phase clusters are half a beat
// apart, and a hundred and forty milliseconds of kernel covers both, so it
// collects EVERY onset and scores as well as the true rate does.  The
// quadruple does the same.  So the comb does not prefer the true rate, it
// prefers the FASTEST one in the band - and the corpus agrees: with the
// prior's weight at zero the median error against librosa is 39 BPM, which
// is the four-thirds harmonic being taken.
//
// MEASURED, over 40 windows against librosa's independent beat tracker, at
// the shipped margin: with the prior voting in the narrow search the median
// error is 1.078 BPM and the wander 0.296; without it, 1.159 and 0.374.  So
// it is worth very little there - the prior's gradient across a four-BPM
// window is about three hundredths of a score, so it only ever breaks ties.
// But it breaks them TOWARDS 125.00, and the corpus is centred on 125, so
// even that measurement flatters it.  A tie-breaker that only ever pulls
// one way is a bias and not a tie-breaker.
static constexpr float AR_TEMPO_PRIOR_BPM   = 125.0f;
static constexpr float AR_TEMPO_PRIOR_SIGMA = 0.35f;
static constexpr float AR_TEMPO_PRIOR_WEIGHT = 0.6f;

// Below this score the comb is not believed, and the tempo is not published
// as valid.
//
// What it is NOT is a detector for a change of music, and that was MEASURED
// rather than assumed: two real tracks, cut against each other, and the
// score's floor after the cut is 0.340-0.395 (median 0.377) against a
// steady-music floor of 0.361.  The two overlap, so NO absolute threshold
// can separate "the music changed" from "the music is the same".  The comb
// scores the last twelve seconds, and after a cut those twelve seconds are
// still full of the OLD music, which still lines up with the OLD rate.
//
// What it IS: the floor under the belief.  The score is the fraction of the
// onset strength that sits under the beat, and when the onsets stop lining
// up with the grid the score falls - and a grid that is not being scored is
// not a grid.  Below this, the tracker stops believing it, the tempo stops
// being published as valid, and the next search looks everywhere at once.
//
// Silence is NOT detected here either: the GATE detects it, a gate shut for
// a second resets the tracker, and the gate is measured in decibels
// against the noise floor, which is the right instrument for that job.
static constexpr float AR_TEMPO_MIN_SCORE = 0.35f;

// How many CONSECUTIVE searches below the floor it takes to call the grid
// collapsed.
//
// One search under the floor is a DIP, not a collapse.  The score falls under
// the floor on a fraction of a percent of the frames of real house music -
// a hi-hat, a snare fill, a quiet bar - and a tracker that gives up its
// grid on one of those turns a good rate into a jump.  MEASURED with a
// single-search rule: the wander over the corpus went from 0.275 to 0.875
// BPM and the jumps from 0.00 to 2.48 a window, and a DJ-style mix spiked to
// 160 BPM.
//
// Four searches is 170 ms of the music NOT fitting the grid, which is a
// collapse and not a dip.  The grid survives a dip; it does not survive four
// of them.
static constexpr uint8_t AR_TEMPO_COLLAPSE_COUNT = 4;

// How much better the far rate has to be before a DEAD local rate gives up
// the grid to it.
//
// The rule used to be "the local rate's score is under the floor, so it is
// dead: take the far rate".  It took the far rate even when the far rate
// was WORSE than the local one, and that is what turned a good rate into a
// walk: MEASURED over sixteen windows of real house music, the wander went
// from 0.105 BPM to 0.835 and the jumps from 0.00 a window to 1.23.
//
// A dead local rate still has to be BEATEN.  How hard is a question the
// corpus answers: 5 % is enough.
static constexpr float AR_TEMPO_DEAD_MARGIN = 0.05f;

// ---- the comb's kernel -------------------------------------------------

// The box's half-width, as a FRACTION of the period.
//
// The box used to be a fixed width in TIME - the tolerance, 70 ms - and
// that is a bias, and it was MEASURED rather than argued: on twenty-five
// real house tracks the raw comb score's argmax sat at 169.5 BPM median
// (sixty per cent of the windows at or above 165) while the music was at
// 125, and on a twelve-track DJ-style mix the published rate started at
// 130, climbed to 175 within half a minute and STAYED there.  That is the
// board's live symptom: a set of lively house music at about 125 BPM
// reported as 167-175.
//
// The cause is the box's width.  A box of a fixed width in TIME covers a
// bigger FRACTION of a fast candidate's period - 0.21 of it at 90 BPM,
// 0.30 at 128, 0.41 at 175 - and the score's floor, which is the fraction
// a phase-uniform signal would capture, rises with it.  So the score
// itself rose with the rate, the whole-band search's winner was always the
// fastest candidate, and the grid was dragged to the top of the band.
//
// The cap is the box's half-width as a fraction of the period, and it
// binds above about 120 BPM, where the tolerance would otherwise be wider
// than a seventh of the beat.  Below that the tolerance still binds, which
// is right: the tolerance is a physical constraint and a slow beat's
// matches really are that close together.
//
// MEASURED, over sixty windows of real house music against librosa's
// independent beat tracker: the rate's error is no worse (median 1.159
// BPM, the same as before) and its MEAN is better (5.12 against 5.35), the
// rate's wander is better (0.275 against 0.407), and the twelve-track
// DJ-style mix holds 125-131 for its whole length instead of climbing to
// 175 and staying there.
static constexpr float AR_TEMPO_KERNEL_FRAC = 0.14f;

// ---- the downbeat ------------------------------------------------------

// The salience's window, in frames either side of the beat.  Three frames
// is 32 ms, and it covers the beat's transient wherever the grid sits
// inside the tolerance.
static constexpr int AR_TEMPO_DOWNBEAT_WIN = 3;

// The samples each position of the profile needs before it may be read.
// Sixteen samples is four bars of the bar's profile and one measure of
// the sixteen's.
static constexpr uint16_t AR_TEMPO_DOWNBEAT_MIN = 16;

// The profile's memory, in beats.  When it is full the accumulators are
// halved, so the profile keeps about this many beats and follows the
// music when it changes.
static constexpr uint16_t AR_TEMPO_DOWNBEAT_MAX = 512;

// The t-score the bar's profile has to reach before the downbeat is
// claimed.
//
// The score is how far the strongest position's mean stands above the
// bar's mean, in standard errors, and it is measured against a NULL: the
// same beats with the bar's alignment rolled by a random amount, which is
// the same statistic on data with no bar structure in it.
//
// MEASURED over sixty windows of real house music (tools/ar_sim/_fw_downnull.py):
// the null's median is 1.96, its 90th percentile 4.93 and its 95th 5.93,
// and the OBSERVED score sits on the null - it is above the null's mean in
// 50 % of the windows (chance) and it passes 5.0 in 11.7 % of the windows
// against the null's 10.0 %.
//
// That is the honest answer for four-on-the-floor music: the four beats of
// a bar are alike, and there is no downbeat to be found.  So the threshold
// is where the null is at its 90th percentile - the bar's alignment has to
// beat nine random alignments in ten - and it is NOT above the null's 95th
// percentile, so it is a threshold that the noise itself crosses one time
// in ten.  A track that really does mark its downbeat - a crash, a sub
// drop, a reversed cymbal - will sail past it.
static constexpr float AR_TEMPO_DOWNBEAT_T = 5.0f;

// The same, for the measure's profile - sixteen positions, so its null is
// a little lower (MEASURED: its median 2.33, its 90th percentile 3.88 and
// its 95th 4.43).
static constexpr float AR_TEMPO_MEASURE_T = 4.5f;

// Fewer onsets than this in the window and there is no search at all.
static constexpr int AR_TEMPO_MIN_PEAKS = 8;

// ---- the PLL -------------------------------------------------------------

// The re-anchor: how far the grid moves toward the comb's phase, per
// search. 35 % a search, so a phase error is gone in three searches - a
// quarter of a second.
static constexpr float AR_TEMPO_PLL_GAMMA = 0.35f;

// The rate's response to the comb: PROPORTIONAL, not a clamp.
//
// The clamp this replaces allowed the period to move at most 0.5 % of itself
// per search, and it applied the FULL step whenever the comb's winner was
// further away than that.  So a disagreement of one BPM - a comb flip between
// two nearby candidates - moved the rate by the whole 0.5 %, which at 128 BPM
// is 0.64 BPM of published ripple per search.  That was the rate's wander:
// the tracker was not being pulled to a different music, it was being
// *amplified* by the clamp whenever the comb hesitated.
//
// A proportional response moves a FRACTION of the way to the comb's winner,
// so a one-BPM disagreement moves the rate by a fifth of a BPM, and a real
// change of six BPM is still taken - geometrically, in about twenty
// searches.
//
// THE GAIN IS CHOSEN AGAINST SOMEBODY ELSE.  The table that first chose it
// was measured with the tracker's own instruments - its own hit rate against
// the raw onsets, its own wander, its own jump count - and that is the wrong
// way to choose it, because the raw onsets reward a tracker that FOLLOWS THE
// COMB: a fast PLL follows the comb's jitter, and the jitter is where the
// onsets are.  The rate is not supposed to follow the jitter.  The rate is
// supposed to be the MUSIC's rate.  So the gain is measured against
// librosa's beat tracker - somebody else's implementation, with other
// assumptions - and against nothing else.
//
// Over 119 windows of real house music, at the shipped band and the shipped
// margin:
//
//   gain | the rate vs librosa's   | librosa's beats | the kick onsets | wander  jumps
//         med    mean    p90        | med    mean      | med    mean    |
//   0.05 | 1.164  5.584  16.949     | 0.7284 0.6893   | 0.6574 0.6683 | 0.302    0.2
//   0.10 | 1.159  5.453  16.869     | 0.7500 0.7037   | 0.6901 0.6884 | 0.396    0.0
//   0.20 | 1.159  5.413  17.950     | 0.7701 0.7201   | 0.7027 0.6991 | 0.377    0.0
//   0.35 | 1.159  5.434  18.071     | 0.7907 0.7316   | 0.7059 0.7041 | 0.365    2.1
//
// "wander" is the standard deviation of the published rate over the second
// half of a window; "jumps" is the number of times the published rate moved
// by more than half a BPM, averaged over the windows.
//
// 0.05 is too timid - it damps the ripple but it also damps the response, and
// the agreement with an independent tracker falls by two points.  0.20 is the
// best of the three tame ones: the lowest mean error, the best agreement
// with an independent tracker, the best coverage of the raw onsets, and less
// wander than 0.10.
//
// 0.35 is a trap.  It scores BEST on both hit metrics - 0.791 and 0.706 -
// and it jumps 2.1 times a window, where 0.20 jumps 0.0 times.  A jump is
// the published rate moving by more than half a BPM, and the flash follows
// the rate, so a jump is a flash that lands off the beat and comes back.
// That is the "skipped beats" failure this tracker exists to avoid, and it
// is not worth two points of hit rate.
static constexpr float AR_TEMPO_PLL_GAIN = 0.20f;

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

// How long the grid holds on with no music before the tempo is called gone.
//
// The GRID is the cadence: it free-runs, and it does not stop when the music
// does.  A breakdown - the beat dropping out for effect - is a few seconds,
// and a grid that restarts on every gap STUTTERS, and a stutter is worse
// than a beat that is a little wrong.  So the grid holds through the gap.
//
// But a track that has ENDED is not a breakdown, and a strobe that flashes
// forever at a rate nobody is playing is worse than a strobe that stops.
// Sixteen seconds is longer than any breakdown and short enough that the
// end of a set is not strobing into the next one.
static constexpr uint16_t AR_TEMPO_HOLD_FRAMES = 1500;   // 16 s at 93.75 fps

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
  bool    haveLastDb;                    // ... and whether there IS one
  float   odfMax;                        // the ODF's running maximum

  // ---- the picker ----
  float   hist[AR_TEMPO_HIST_RING];      // the recent ODF values
  uint8_t histIdx;                       // where the next one goes
  uint16_t histFilled;                   // how many have been written

  // ---- the onsets ----
  //
  // The frames are SIGNED and RELATIVE to the frame base, and that is not a
  // detail: a rebase moves the base forward under the onsets that are
  // already in the ring, so an onset that happened before the rebase is
  // NEGATIVE afterwards.  They used to be unsigned, and the rebase
  // subtracted 65536 from every one of them - which, for a frame below
  // 65536, is not a number but a wrap.  The wrapped frames are all in the
  // past as far as the comb is concerned, so every onset landed in bin
  // zero, every candidate scored the same, and the prior picked 125.00 and
  // re-anchored the grid to phase zero.
  int32_t  peakFrame[AR_TEMPO_PEAK_RING];  // the onset ring: each onset's frame
  float    peakStr[AR_TEMPO_PEAK_RING];    // ... and its strength
  uint16_t peakHead;                       // the ring's write index
  uint16_t peakCount;                      // how many are in it
  int32_t  prevPeakFrame;                  // the last accepted onset (the gap)
  float    prevPeakStr;

  // ---- the sweep ----
  int32_t  sweepFrame[AR_TEMPO_PEAK_RING]; // the window's onsets, snapshotted
  float    sweepStr[AR_TEMPO_PEAK_RING];   // ... so the sweep sees ONE set
  uint16_t sweepCount;
  uint16_t sweepIndex;
  uint16_t sweepFirst;                    // the first candidate's index
  uint16_t sweepN;                        // how many candidates this search has
  uint16_t sweepPerFrame;                 // how many are scored in one frame
  uint32_t lastSweepFrame;                // when the last search started
  uint32_t lastFullFrame;                 // ... and the last WHOLE-BAND one
  float    sweepNow;                      // the frame the sweep STARTED at
  float    sweepTotal;                    // the window's onset strength, summed
  float    combH[AR_TEMPO_COMB_BINS];      // the phase histogram
  float    sweepBestValue;                // the best (score - penalty)
  float    sweepBestScore;
  float    sweepBestBpm;
  float    sweepBestPhase;
  uint16_t sweepBestIdx;                  // ... and which candidate that was
  // The narrow window's best, measured on the SAME snapshot as the whole
  // band so the two are comparable.  It is what the veto compares against.
  uint16_t sweepNarrowFirst;              // the narrow window's first candidate
  uint16_t sweepNarrowCount;              // ... and how many it has
  float    sweepNarrowValue;
  float    sweepNarrowScore;
  float    sweepNarrowBpm;
  float    sweepNarrowPhase;
  uint16_t sweepNarrowIdx;                // ... and which candidate that was
  // The FAR candidate: the best EVIDENCE outside the narrow window.  It is
  // nominated by the RAW score, and not by the value, because the prior is
  // a tie-breaker and it must not be allowed to choose which far rate gets
  // to argue with the local one.  When the prior chose it, a rate fifteen
  // BPM away never got to argue at all: the prior had already decided it
  // was not worth mentioning, and 125.00 became an attractor.
  float    sweepFarScore;
  float    sweepFarValue;
  float    sweepFarBpm;
  float    sweepFarPhase;
  uint16_t sweepFarIdx;                   // ... and which candidate that was
  bool     sweepActive;
  bool     sweepWasNarrow;                // this search looked around
  bool     sweepHasNarrow;                // ... and the veto's window is marked
  bool     needFullSearch;                // the next one looks everywhere
  bool     fullSearchFailed;              // ... and one that found nothing
  uint8_t  collapseCount;                 // searches below the floor, in a row

  // ---- the PLL ----
  float    period;                        // the beat period, in frames
  float    nextBeat;                      // the frame of the NEXT beat
  float    lastBeat;                      // the frame of the LAST beat
  uint32_t beatIndex;                     // the monotonic beat counter
  uint32_t frameIndex;                    // the frame counter, rebased
  float    bpm;                           // the published rate
  float    score;                         // the comb score, 0..1
  bool     locked;                        // the grid is running

  // ---- the downbeat ----
  //
  // The profile of the bar: the salience of the beats at each of the four
  // positions, and the same for the sixteen positions of the measure.
  // The strongest position is the downbeat - IF the profile stands out
  // from the noise, which is what the t-score decides.
  float    barSum[4];                     // the salience at each position
  float    barSq[4];                      // ... and its square, for the variance
  uint16_t barCnt[4];                     // ... and how many beats each has
  float    measSum[16];
  float    measSq[16];
  uint16_t measCnt[16];
  uint8_t  downbeatPos;                   // the measured downbeat, 0..3
  uint8_t  measurePos;                    // the measured measure's beat, 0..15
  float    downbeatT;                     // the profile's t-score
  float    measureT;
  bool     downbeatKnown;                 // the score is above the threshold
  bool     measureKnown;
  uint32_t downbeatEvalIndex;             // the beat the profiles were scored at
  uint32_t downbeatApplyAt;               // the beat the numbering shifts at
  uint32_t measureApplyAt;                // ... and the measure's, over sixteen

  // ---- the gate ----
  uint16_t gateShutFrames;                // how long the gate has been shut

  // ---- the ProLink reference ----
  //
  // The deck knows its own beat grid EXACTLY: it is the thing that made the
  // music.  When it is connected, the board can measure this tracker against
  // it - how far the beat is off in milliseconds, whether the rate agrees,
  // and whether the bar's numbering agrees.  That is the instrument that says
  // whether the audio-only tracker is right, and it is the only reference
  // that can say whether the downbeat is knowable from the sound at all.
  float    refBpm;                        // the deck's rate, 0 = no deck
  float    refErrMs;                      // our beat minus the deck's AUDIBLE grid, ms
  uint8_t  refBeat;                       // the deck's beat in the bar, 1..4
  uint8_t  refOurBeat;                    // our beat in the bar at that moment
  uint16_t refCount;                      // how many beats have been compared
  uint16_t refLocked;                     // ... and how many advanced TOGETHER
  uint16_t refSlips;                      // ... and how many times one of them skipped
  uint8_t  refOffset;                     // the deck's position minus ours, 0..3
  bool     refHaveErr;                    // the error's mean has a sample

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
//
// It used to be ROUNDED to a whole number of bins.  A bin is period/64
// wide - 7 ms at 128 BPM - so the tolerance the tracker actually applied
// moved by up to half a bin from one candidate to the next, and it was
// half a bin WIDER than the tolerance it advertised: the box covered bins
// i-w .. i+w, which is the continuous interval [i-w, i+w+1), and that
// interval's centre is i+1/2, not i.
//
// That mattered because a WIDER box catches more of the smear of a WRONG
// period.  A candidate that happened to round its half-width up was scored
// with a box up to a whole bin wider than its neighbour's, and it won on
// that alone.  The score profile across the band had a sawtooth on it, and
// the sawtooth's period was the candidate spacing.
//
// The rounding is gone.  The box is now the EXACT integral of the
// histogram over the requested half-width: the integer rolling sum is
// still there - it is the box's whole part - and the two fractional edge
// bins are added on.
//
// `wholeOut` gets the integer part of the half-width and `fracOut` the
// remainder, so the caller can build the box from both.
//
// The half-width is clamped BEFORE it is split, not after, so the
// remainder is always in [0,1).  Clamping the integer part instead would
// leave the remainder above 1 whenever the clamp bit, and the fractional
// edge rule - which assumes a remainder of less than one bin - would then
// add more than a whole bin's worth of the bins outside the box.
//
// With the allowed tolerance (30-150 ms) and the tracker's band (90-175
// BPM) the half-width reaches 28 bins at the tightest end, so the clamp
// does not bite; it is here so that a wider tolerance cannot break the
// rule.
static void arTempoKernelHalfWidth(float period, int *wholeOut, float *fracOut) {
  const float tolFrames = (arFrameSettings.tempoTolMs * 0.001f) * AR_TEMPO_RATE;
  float wf = tolFrames / period * (float)AR_TEMPO_COMB_BINS;
  // The CAP.  The box is never wider than AR_TEMPO_KERNEL_FRAC of the
  // period, so a fast candidate's box cannot cover a bigger share of its
  // beat than a slow one's.  Without it the score rises with the rate -
  // the score's floor is the box's width as a fraction of the circle -
  // and the whole-band search's winner is always the fastest candidate.
  // See the constant for the measurement.
  const float wfrac = AR_TEMPO_KERNEL_FRAC * (float)AR_TEMPO_COMB_BINS;
  if (wf > wfrac) wf = wfrac;
  // One bin's clearance is kept at each end, because the fractional case
  // can reach one bin past the integer box.
  const float wmax = (float)(AR_TEMPO_COMB_BINS / 2 - 2);
  if (wf < 1.0f) wf = 1.0f;
  if (wf > wmax) wf = wmax;
  const int w = (int)wf;              // wf is positive, so this is the floor
  *wholeOut = w;
  *fracOut  = wf - (float)w;
}

// The score at one candidate rate. `phaseOut` gets the best phase, in
// frames, relative to the tracker's frame base.
//
// `total` is the sum of the window's onset strengths. It does not depend on
// the candidate, so it is computed ONCE per sweep and passed in: it used to
// be summed inside this loop, which meant 256 extra adds for every one of
// the 44 candidates scored in a frame.
//
// `frames` are RELATIVE to the frame base, so they are small numbers and the
// float arithmetic on them is exact.  They can be NEGATIVE - see the note on
// the onset ring - and the phase arithmetic below has to cope with that.
static float arTempoCombScore(const int32_t *frames, const float *str, int n,
                               float total, float bpm, float *phaseOut) {
  const float period    = 60.0f * AR_TEMPO_RATE / bpm;
  const float invPeriod = 1.0f / period;
  const float binScale  = (float)AR_TEMPO_COMB_BINS * invPeriod;
  float *h = arTempo.combH;

  for (int i = 0; i < AR_TEMPO_COMB_BINS; i++) h[i] = 0.0f;
  for (int i = 0; i < n; i++) {
    const float f = (float)frames[i];
    // The phase offset.
    //
    // The quotient has to be a FLOOR and not a truncation toward zero: the
    // frames can be negative, and a truncation toward zero would put every
    // negative frame a whole period off - which is a whole bin's worth of
    // the histogram, for every onset, for every candidate.
    //
    // The two corrections catch the case where the multiply rounded across
    // an integer, which would otherwise put the onset a whole period out.
    const float k = floorf(f * invPeriod);
    float u = f - k * period;
    if (u < 0.0f) u += period;
    else if (u >= period) u -= period;
    int bin = (int)(u * binScale);
    if (bin < 0) bin = 0;
    else if (bin >= AR_TEMPO_COMB_BINS) bin = AR_TEMPO_COMB_BINS - 1;
    h[bin] += str[i];
  }
  if (total <= 0.0f) return 0.0f;

  // The circular box kernel, by a ROLLING sum: the window is 2*w+1 bins
  // wide and it slides one bin at a time, so the smoothing is O(bins) and
  // not O(bins*w).
  //
  // The box is centred on the BIN's CENTRE, (i + 1/2), because the
  // integer box of half-width w covers bins i-w .. i+w, and that is the
  // continuous interval [i-w, i+w+1), whose centre is i+1/2.  Write the
  // requested half-width as wf = w + f:
  //
  //   f < 0.5   the exact box is the integer one MINUS (0.5-f) of each
  //             of its two edge bins - it is NARROWER than the integer
  //             box, by half a bin at each end
  //   f > 0.5   the exact box is the integer one PLUS (f-0.5) of each of
  //             the bins just OUTSIDE it - it is WIDER
  //
  // At f = 0.5 the two agree, and that is the point where the integer box
  // IS the exact box.  (The old code always used the integer box, so it
  // was up to a whole bin wider than the tolerance it advertised.)
  int w = 1;
  float f = 0.0f;
  arTempoKernelHalfWidth(period, &w, &f);
  float win = 0.0f;
  for (int j = -w; j <= w; j++) win += h[(j + AR_TEMPO_COMB_BINS) & (AR_TEMPO_COMB_BINS - 1)];
  float best = -1.0e30f;
  int bestBin = 0;
  // The box's value at every bin, so the plateau's extent can be found.
  // 64 floats on the stack.
  float box[AR_TEMPO_COMB_BINS];
  for (int i = 0; i < AR_TEMPO_COMB_BINS; i++) {
    if (i > 0) {
      win += h[(i + w) & (AR_TEMPO_COMB_BINS - 1)]
           - h[(i - w - 1 + AR_TEMPO_COMB_BINS) & (AR_TEMPO_COMB_BINS - 1)];
    }
    float v = win;
    if (f < 0.5f) {
      v -= (0.5f - f) * (h[(i - w + AR_TEMPO_COMB_BINS) & (AR_TEMPO_COMB_BINS - 1)]
                       + h[(i + w) & (AR_TEMPO_COMB_BINS - 1)]);
    } else {
      v += (f - 0.5f) * (h[(i - w - 1 + AR_TEMPO_COMB_BINS) & (AR_TEMPO_COMB_BINS - 1)]
                       + h[(i + w + 1) & (AR_TEMPO_COMB_BINS - 1)]);
    }
    box[i] = v;
    if (v > best) { best = v; bestBin = i; }
  }
  // THE PLATEAU'S CENTRE, NOT ITS LEADING EDGE.
  //
  // The box is 2*w+1 bins wide, so an onset cluster NARROWER than the box
  // produces a plateau of equal values 2*w+1 bins across - up to 27 of the
  // 64 bins at 175 BPM.  The argmax above returns the FIRST of them, and
  // the first is the plateau's LEADING edge: up to a whole tolerance
  // BEFORE the cluster's centre.
  //
  // That phase is what the grid is anchored on - at the first lock and at
  // every re-anchor - so the whole beat grid, and the published
  // beat_progress, inherit the bias: every beat up to 70 ms early.  A
  // constant offset is invisible to the corpus metric, which removes the
  // offset before it scores, and very visible to a strobe, which flashes
  // where the grid says the beat is.
  //
  // So the phase is the MIDDLE of the plateau.  A plateau of bins a..b has
  // its centre at (a+b)/2 + 1/2, because the bins' positions are their
  // LEFT edges and the box is centred on a bin's CENTRE.
  if (phaseOut) {
    // The plateau's extent, counted in steps from the argmax.  The
    // comparison is relative: the rolling sum is float32 and the plateau's
    // values drift in the last bits.
    const float eps = best * 1e-5f;
    int left = 0, right = 0;
    while (left < AR_TEMPO_COMB_BINS
           && box[(bestBin - 1 - left + AR_TEMPO_COMB_BINS) & (AR_TEMPO_COMB_BINS - 1)]
              >= best - eps) left++;
    while (right < AR_TEMPO_COMB_BINS
           && box[(bestBin + 1 + right) & (AR_TEMPO_COMB_BINS - 1)]
              >= best - eps) right++;
    // The centre, in bins.  The bins' positions are their LEFT edges and
    // the box is centred on a bin's CENTRE, so a plateau that runs from
    // bestBin-left to bestBin+right has its centre at
    // bestBin + (right-left)/2 + 1/2.  The arithmetic is done on the
    // unwrapped steps, so a plateau that wraps round the circle comes out
    // right.
    const float centre = (float)bestBin + 0.5f * (float)(right - left) + 0.5f;
    *phaseOut = centre / (float)AR_TEMPO_COMB_BINS * period;
  }
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
  arTempo.haveLastDb  = false;
  arTempo.odfMax      = 0.0f;
  arTempo.histIdx     = 0;
  arTempo.histFilled  = 0;
  arTempo.peakHead    = 0;
  arTempo.peakCount   = 0;
  arTempo.prevPeakFrame = -1000;
  arTempo.prevPeakStr = 0.0f;
  arTempo.sweepCount  = 0;
  arTempo.sweepIndex  = 0;
  arTempo.sweepFirst  = 0;
  arTempo.sweepN      = AR_TEMPO_CAND_COUNT;
  arTempo.sweepPerFrame = AR_TEMPO_CAND_PER_FRAME;
  arTempo.lastSweepFrame = 0;
  arTempo.lastFullFrame = 0;
  arTempo.sweepActive = false;
  arTempo.sweepWasNarrow = false;
  arTempo.needFullSearch = false;
  arTempo.sweepBestValue = -1e9f;
  arTempo.sweepBestScore = 0.0f;
  arTempo.sweepBestBpm   = 0.0f;
  arTempo.sweepBestPhase = 0.0f;
  arTempo.sweepBestIdx   = 0;
  arTempo.sweepHasNarrow = false;
  arTempo.sweepNarrowFirst = 0;
  arTempo.sweepNarrowCount = 0;
  arTempo.sweepNarrowValue = -1e9f;
  arTempo.sweepNarrowScore = -1e9f;
  arTempo.sweepNarrowBpm   = 0.0f;
  arTempo.sweepNarrowPhase = 0.0f;
  arTempo.sweepNarrowIdx   = 0;
  arTempo.sweepFarScore = -1e9f;
  arTempo.sweepFarValue = -1e9f;
  arTempo.sweepFarBpm   = 0.0f;
  arTempo.sweepFarPhase = 0.0f;
  arTempo.sweepFarIdx   = 0;
  arTempo.fullSearchFailed = false;
  arTempo.collapseCount = 0;
  arTempo.period      = 0.0f;
  arTempo.nextBeat    = 0.0f;
  arTempo.lastBeat    = 0.0f;
  arTempo.beatIndex   = 0;
  arTempo.frameIndex  = 0;
  arTempo.bpm         = 0.0f;
  arTempo.score       = 0.0f;
  // The downbeat's profile goes with the grid: its positions are the
  // beat counter's, and the beat counter restarts with the lock.
  for (int i = 0; i < 4; i++) {
    arTempo.barSum[i] = 0.0f;
    arTempo.barSq[i]  = 0.0f;
    arTempo.barCnt[i] = 0;
  }
  for (int i = 0; i < 16; i++) {
    arTempo.measSum[i] = 0.0f;
    arTempo.measSq[i]  = 0.0f;
    arTempo.measCnt[i] = 0;
  }
  arTempo.downbeatPos  = 0;
  arTempo.measurePos   = 0;
  arTempo.downbeatT    = 0.0f;
  arTempo.measureT     = 0.0f;
  arTempo.downbeatKnown = false;
  arTempo.measureKnown  = false;
  arTempo.downbeatEvalIndex = 0;
  arTempo.downbeatApplyAt = 0;
  arTempo.measureApplyAt = 0;
  arTempo.locked      = false;
  arTempo.gateShutFrames = 0;
  arTempo.refBpm      = 0.0f;
  arTempo.refErrMs    = 0.0f;
  arTempo.refBeat     = 0;
  arTempo.refOurBeat  = 0;
  arTempo.refCount    = 0;
  arTempo.refLocked   = 0;
  arTempo.refSlips    = 0;
  arTempo.refOffset   = 0;
  arTempo.refHaveErr  = false;
}

// Throw the EVIDENCE away and keep the grid.
//
// The grid is the CADENCE: the period, the phase and the beat counter.  It
// free-runs, and it does not stop when the music does - a breakdown drops
// the beat out for a few seconds, and a grid that restarts on every gap
// STUTTERS.  A stutter is worse than a beat that is a little wrong, because
// the eye reads a stutter as broken and reads a wrong beat as a wrong beat.
//
// What is thrown away is the evidence: the onsets, the sweep and the score,
// so the music that comes back is measured afresh.  The grid keeps running
// meanwhile, and the search that runs when the music returns re-anchors it.
static void arTempoHold(void) {
  arTempo.peakCount = 0;
  arTempo.peakHead = 0;
  arTempo.prevPeakFrame = -1000;
  arTempo.prevPeakStr = 0.0f;
  arTempo.sweepActive = false;
  arTempo.needFullSearch = false;
  arTempo.fullSearchFailed = false;
  arTempo.collapseCount = 0;
}

// Throw the grid away and wait for the next search.
static void arTempoRequestRelock(void) {
  arTempoRelockRequested.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The PLL's rate update and re-anchor.
//
// The period moves a FRACTION of the way to the comb's winner - PLL_GAIN of
// the distance - so the rate's step is proportional to how much the comb
// disagrees with it. The re-anchor moves the grid's POSITION by PLL_GAMMA of
// the distance to the new comb phase - 35 % per search, so a phase error is
// gone in three searches.
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
    // The downbeat's profile goes with it: its positions ARE the beat
    // counter's, so a profile left over from the last grid would be
    // counted against the wrong bar.
    for (int i = 0; i < 4; i++) {
      arTempo.barSum[i] = 0.0f;
      arTempo.barSq[i]  = 0.0f;
      arTempo.barCnt[i] = 0;
    }
    for (int i = 0; i < 16; i++) {
      arTempo.measSum[i] = 0.0f;
      arTempo.measSq[i]  = 0.0f;
      arTempo.measCnt[i] = 0;
    }
    arTempo.downbeatKnown = false;
    arTempo.measureKnown  = false;
    arTempo.downbeatEvalIndex = 0;
    arTempo.downbeatApplyAt = 0;
    arTempo.measureApplyAt = 0;
  } else {
    arTempo.period += AR_TEMPO_PLL_GAIN * (P - arTempo.period);
    const float k      = roundf((arTempo.nextBeat - phase) / arTempo.period);
    const float anchor = phase + k * arTempo.period;
    arTempo.nextBeat += AR_TEMPO_PLL_GAMMA * (anchor - arTempo.nextBeat);
  }
  arTempo.bpm = 60.0f * AR_TEMPO_RATE / arTempo.period;
}

// ---------------------------------------------------------------------------
// The narrow window, in candidate indices.
//
// The window is ARITHMETIC in the candidate grid, not in the BPM, and that
// matters more than it looks.
//
// The published rate is a float and it is rarely exactly on a candidate - the
// proportional PLL leaves it a fraction UNDER the candidate it is converging
// to - so a window computed by rounding the two BPM edges loses a candidate
// at one end and comes out asymmetric: 124.9999 BPM gave a window of 123.00
// to 126.75, sixteen candidates, when it should have been 123.00 to 127.00,
// seventeen.  Worse, the window's MEMBERSHIP then changes every time the
// rate crosses a candidate boundary, so the local reference the veto
// compares against can change for no reason but a rounding.
//
// Snapping the CENTRE to the candidate grid and counting outwards gives the
// same window every time: seventeen candidates, symmetric, and it only moves
// when the rate crosses a half-step.
// ---------------------------------------------------------------------------
static void arTempoNarrowWindow(float bpm, int *first, int *count) {
  const int half = (int)lroundf(AR_TEMPO_NARROW_BPM / AR_TEMPO_CAND_STEP);
  int centre = (int)lroundf((bpm - AR_TEMPO_BPM_LO) / AR_TEMPO_CAND_STEP);
  int f = centre - half;
  int c = 2 * half + 1;
  if (f < 0) { c += f; f = 0; }                    // the window ran off the bottom
  if (f + c > AR_TEMPO_CAND_COUNT) c = AR_TEMPO_CAND_COUNT - f;
  if (f < 0) f = 0;
  if (c < 1) c = 1;
  *first = f;
  *count = c;
}

// ---------------------------------------------------------------------------
// The sweep.
//
// Two searches, and they cost very different amounts.
//
// The WHOLE-BAND search looks at all 341 candidates.  It does not fit in
// one frame comfortably, so it is SPREAD: it starts, scores a fixed number
// of candidates a frame, and finishes eight frames later.  It runs when
// the tracker has no rate - at the start, and after the music changes.
//
// The NARROW search looks at the rate the grid believes, plus or minus
// AR_TEMPO_NARROW_BPM.  That is a handful of candidates and they are all
// scored in ONE frame, every AR_TEMPO_SEARCH_EVERY frames.
//
// The peak set is SNAPSHOTTED at the start, so every candidate is scored
// against the same onsets - otherwise the candidates would not be
// comparable with each other.
// ---------------------------------------------------------------------------

static void arTempoSweepStart(void) {
  // The window's onsets, newest first. The comb does not care about the
  // order - the modulo is order-independent - but the ring is walked
  // backwards anyway, because that is the direction the window is in.
  const int32_t cutoff = (int32_t)arTempo.frameIndex - (int32_t)AR_TEMPO_COMB_FRAMES;
  int n = 0;
  float total = 0.0f;
  for (int i = 0; i < (int)arTempo.peakCount && n < AR_TEMPO_PEAK_RING; i++) {
    const int idx = (arTempo.peakHead + AR_TEMPO_PEAK_RING - 1 - i) & (AR_TEMPO_PEAK_RING - 1);
    if (arTempo.peakFrame[idx] < cutoff) break;   // too old
    arTempo.sweepFrame[n] = arTempo.peakFrame[idx];
    arTempo.sweepStr[n]   = arTempo.peakStr[idx];
    total += arTempo.peakStr[idx];
    n++;
  }

  // The candidate set.  Locked, the search looks around the rate the grid
  // believes; unlocked, it looks at the whole band, because that is the
  // search that FINDS a rate in the first place.
  int first = 0;
  int count = AR_TEMPO_CAND_COUNT;
  bool narrow = false;
  if (arTempo.locked && !arTempo.needFullSearch && arTempo.bpm > 0.0f) {
    arTempoNarrowWindow(arTempo.bpm, &first, &count);
    narrow = true;
  }
  arTempo.needFullSearch = false;
  arTempo.sweepWasNarrow = narrow;

  // The veto's window.  When the search is the WHOLE BAND and the tracker
  // is locked, the veto needs the best answer NEAR the grid as well as the
  // best answer anywhere - and it needs both from the SAME snapshot, or the
  // comparison is not fair.  So the narrow window's candidates are marked
  // here, and the scoring loop remembers which of them won.
  //
  // A narrow search does not need this: everything it scored was inside the
  // window already.
  arTempo.sweepHasNarrow   = false;
  arTempo.sweepNarrowFirst = 0;
  arTempo.sweepNarrowCount = 0;
  arTempo.sweepNarrowValue = -1e9f;
  arTempo.sweepNarrowScore = -1e9f;
  arTempo.sweepNarrowBpm   = 0.0f;
  arTempo.sweepNarrowPhase = 0.0f;
  arTempo.sweepNarrowIdx   = 0;
  arTempo.sweepFarScore = -1e9f;
  arTempo.sweepFarValue = -1e9f;
  arTempo.sweepFarBpm   = 0.0f;
  arTempo.sweepFarPhase = 0.0f;
  arTempo.sweepFarIdx   = 0;
  if (!narrow && arTempo.locked && arTempo.bpm > 0.0f) {
    int f = 0, c = 0;
    arTempoNarrowWindow(arTempo.bpm, &f, &c);
    arTempo.sweepNarrowFirst = (uint16_t)f;
    arTempo.sweepNarrowCount = (uint16_t)c;
    arTempo.sweepHasNarrow   = true;
  }

  arTempo.sweepCount     = n;
  arTempo.sweepIndex     = 0;
  arTempo.sweepFirst     = (uint16_t)first;
  arTempo.sweepN         = (uint16_t)count;
  arTempo.sweepPerFrame  = (count > AR_TEMPO_CAND_PER_FRAME)
                         ? (uint16_t)AR_TEMPO_CAND_PER_FRAME : (uint16_t)count;
  arTempo.lastSweepFrame = arTempo.frameIndex;
  if (!narrow) arTempo.lastFullFrame = arTempo.frameIndex;
  arTempo.sweepNow       = (float)arTempo.frameIndex;
  arTempo.sweepTotal     = total;
  arTempo.sweepBestValue = -1e9f;
  arTempo.sweepBestScore = 0.0f;
  arTempo.sweepBestBpm   = 0.0f;
  arTempo.sweepBestPhase = 0.0f;
  arTempo.sweepBestIdx   = 0;
  arTempo.sweepActive    = (n >= AR_TEMPO_MIN_PEAKS);
}

static void arTempoSweepStep(void) {
  if (!arTempo.sweepActive) return;
  const int first = arTempo.sweepFirst;
  const int total = arTempo.sweepN;
  for (int i = 0; i < arTempo.sweepPerFrame && arTempo.sweepIndex < total; i++) {
    const int idx = first + arTempo.sweepIndex++;
    const float bpm = AR_TEMPO_BPM_LO + (float)idx * AR_TEMPO_CAND_STEP;
    float phase = 0.0f;
    const float score = arTempoCombScore(arTempo.sweepFrame, arTempo.sweepStr,
                                         arTempo.sweepCount, arTempo.sweepTotal,
                                         bpm, &phase);
    // The prior votes in the WHOLE-BAND search and NOT in the narrow one.
    //
    // The narrow search looks at the rate the grid already believes, plus or
    // minus two BPM - seventeen candidates, and on a 0.25 BPM grid that is a
    // four BPM span.  The prior's gradient across that span is small: about
    // a thousandth of a score at 125 BPM and about three hundredths at 140.
    // So a prior that votes there does not decide the search - it breaks
    // ties, and it breaks them TOWARDS ITS CENTRE.
    //
    // That is the whole reason it does not vote there.  The grid was not
    // being pulled to a different music; it was being pulled to 125.00 by a
    // tie-breaker that was allowed to outvote the evidence, and 125.00 is
    // where the board said the music was when the music was not there.
    //
    // MEASURED, over 40 windows against librosa's independent beat tracker,
    // at the shipped margin: with the prior voting here the median error
    // is 1.078 BPM and the wander 0.296; without it, 1.159 and 0.374.  So
    // it is worth very little - and the corpus is centred on 125, so even
    // that flatters it.
    //
    // The whole-band search is where the prior belongs: that is the search
    // that has to choose a rate out of nothing, and the prior is what keeps
    // it off the four-thirds harmonic, whose score is only a little better.
    const float value = arTempo.sweepWasNarrow ? score
                                               : (score - arTempoPriorPen[idx]);
    if (value > arTempo.sweepBestValue) {
      arTempo.sweepBestValue = value;
      arTempo.sweepBestScore = score;
      arTempo.sweepBestBpm   = bpm;
      arTempo.sweepBestPhase = phase;
      arTempo.sweepBestIdx   = (uint16_t)idx;
    }
    // The veto's local answer: the best candidate inside the window around
    // the rate the grid believes.  Scored on the same snapshot, with the
    // same kernel, so the two are comparable.
    //
    // It is picked by the RAW score and not by the value, so the prior does
    // not get a vote in the fight it is supposed to stay out of.  The
    // difference is small - inside a two-BPM window the prior moves by less
    // than a thousandth - but it is the difference between "the local rate's
    // best EVIDENCE" and "the local rate the prior likes best", and only
    // the first of those is evidence.
    const bool inWindow = arTempo.sweepHasNarrow
        && idx >= (int)arTempo.sweepNarrowFirst
        && idx < (int)arTempo.sweepNarrowFirst + (int)arTempo.sweepNarrowCount;
    if (inWindow) {
      if (score > arTempo.sweepNarrowScore) {
        arTempo.sweepNarrowScore = score;
        arTempo.sweepNarrowValue = value;
        arTempo.sweepNarrowBpm   = bpm;
        arTempo.sweepNarrowPhase = phase;
        arTempo.sweepNarrowIdx   = (uint16_t)idx;
      }
    } else if (arTempo.sweepHasNarrow) {
      // The FAR candidate: the best EVIDENCE outside the window.
      //
      // It is nominated by the RAW score.  The prior is a tie-breaker, and
      // a tie-breaker that also gets to choose the opponent is not a
      // tie-breaker: it chose 125.00's opponent every time, and the grid
      // could never be argued out of 125.00.
      if (score > arTempo.sweepFarScore) {
        arTempo.sweepFarScore = score;
        arTempo.sweepFarValue = value;
        arTempo.sweepFarBpm   = bpm;
        arTempo.sweepFarPhase = phase;
        arTempo.sweepFarIdx   = (uint16_t)idx;
      }
    }
  }
  if (arTempo.sweepIndex < total) return;

  // The veto.
  //
  // The safety net has looked everywhere, and it has two answers: the best
  // EVIDENCE near the grid, and the best evidence far from it.  The far one
  // is asking the grid to move a long way, so it has to be CLEARLY better
  // evidence than the rate the grid already believes.  Otherwise the
  // near-tie is refused and the local rate keeps the grid.
  //
  // Both sides of the fight are the RAW score, measured on the SAME
  // snapshot with the same kernel, and the prior is left out of it
  // altogether: the prior is a tie-breaker, not evidence, and letting it
  // decide this fight is what turned its centre - 125.00 - into an
  // attractor.
  //
  // And the local rate has to be ALIVE to defend itself.  A rate the
  // tracker has already declared dead - one whose score has been below the
  // minimum for AR_TEMPO_COLLAPSE_COUNT searches in a row, which is exactly
  // what the COLLAPSE search means - has no evidence to defend, so it does
  // not get to veto anything.  A dead local rate cannot keep the grid while
  // the music has moved on.
  //
  // The death has to be SUSTAINED, because one search under the floor is a
  // dip and not a collapse: the score goes under the floor on a fraction of
  // a percent of the frames of real music, and a grid that surrenders on one
  // of them is a grid that jumps.
  if (arTempo.sweepHasNarrow) {
    const bool localAlive = arTempo.collapseCount < AR_TEMPO_COLLAPSE_COUNT;
    const bool farAlive   = arTempo.sweepFarScore   >= AR_TEMPO_MIN_SCORE;
    bool takeFar = false;
    if (!localAlive) {
      // The local rate is dead, but it still has to be BEATEN.  Taking the
      // far rate because the local one is under the floor - without asking
      // whether the far one is any better - is what walked a good rate into
      // a false one.
      takeFar = farAlive
             && arTempo.sweepFarScore
             >= arTempo.sweepNarrowScore * (1.0f + AR_TEMPO_DEAD_MARGIN);
    } else {
      takeFar = arTempo.sweepFarScore
             >= arTempo.sweepNarrowScore * (1.0f + AR_TEMPO_NET_MARGIN);
    }
    if (takeFar) {
      arTempo.sweepBestValue = arTempo.sweepFarValue;
      arTempo.sweepBestScore = arTempo.sweepFarScore;
      arTempo.sweepBestBpm   = arTempo.sweepFarBpm;
      arTempo.sweepBestPhase = arTempo.sweepFarPhase;
      arTempo.sweepBestIdx   = arTempo.sweepFarIdx;
    } else {
      // The local rate keeps the grid.  Its score is the one that is
      // published, because it is the score of the rate being published.
      arTempo.sweepBestValue = arTempo.sweepNarrowValue;
      arTempo.sweepBestScore = arTempo.sweepNarrowScore;
      arTempo.sweepBestBpm   = arTempo.sweepNarrowBpm;
      arTempo.sweepBestPhase = arTempo.sweepNarrowPhase;
      arTempo.sweepBestIdx   = arTempo.sweepNarrowIdx;
    }
  }

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
    arTempo.fullSearchFailed = false;
    arTempo.collapseCount = 0;
    arTempoAdopt(arTempo.sweepBestBpm, arTempo.sweepBestPhase, arTempo.sweepNow);
  } else if (arTempo.sweepWasNarrow) {
    // The narrow search cannot find the beat any more.  Its score fell
    // below the minimum, so the music it was tracking is not there: the
    // next search looks everywhere, at once.
    //
    // Unless a WHOLE-BAND search has already looked everywhere and found
    // nothing.  Then looking everywhere again will not make a beat appear,
    // and the sweep would run every ninth frame for the price of 341
    // candidates.  The safety net still looks, once every
    // AR_TEMPO_FULL_EVERY frames - that is what it is for.
    //
    // But it takes AR_TEMPO_COLLAPSE_COUNT searches in a row below the
    // floor before the grid is called dead.  One search under the floor is
    // a dip: the score goes under it on a fraction of a percent of the
    // frames of real music, and looking everywhere on one of them is how a
    // good rate turns into a jump.
    if (arTempo.collapseCount < 255) arTempo.collapseCount++;
    if (!arTempo.fullSearchFailed
        && arTempo.collapseCount >= AR_TEMPO_COLLAPSE_COUNT) {
      arTempo.needFullSearch = true;
    }
  } else {
    // A WHOLE-BAND search that found nothing.  It has already looked
    // everywhere, so the narrow search must not ask it to.
    arTempo.fullSearchFailed = true;
    if (arTempo.collapseCount < 255) arTempo.collapseCount++;
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

  // The candidate's frame.  It is relative to the base and it can be
  // negative - the base moves on at a rebase, and the onset that was three
  // frames back is now three frames before it.
  const int32_t j = (int32_t)frame - (int32_t)AR_TEMPO_PEAK_POST;

  // The minimum separation, keeping the STRONGER of any pair.
  //
  // The difference is taken in the unsigned domain and read back as signed,
  // which is the two's-complement difference and is exact for any pair
  // inside the base's range.
  if ((int32_t)((uint32_t)j - (uint32_t)arTempo.prevPeakFrame) < AR_TEMPO_PEAK_MIN_GAP) {
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
// THE DOWNBEAT - is one beat in the bar different from the others?
//
// The bar's profile is the salience of the beats at each of the four
// positions, and the measure's is the same for sixteen.  The strongest
// position is the downbeat - IF the profile stands out from the noise.
//
// And that "if" is the whole of it, and it was MEASURED rather than
// assumed.  The score is a t-score - how far the strongest position's
// mean stands above the profile's mean, in standard errors - and it is
// measured against a NULL: the same beats with the bar's alignment rolled
// by a random amount, which is the same statistic on data with no bar
// structure in it.  Over sixty windows of real house music:
//
//   the null's median                 1.95   (the bar)   2.41 (the measure)
//   the null's 95th percentile        5.86                  4.36
//   the observed median               2.03                  2.29
//   the observed above the null       43 % of the windows    55 %
//
// Chance.  Four-on-the-floor house music does not mark its downbeat - the
// four beats of a bar are alike - so there is nothing there to find, and a
// tracker that claims a downbeat out of that is inventing one.  So the
// threshold is above the null's 95th percentile, and a track that really
// does mark its downbeat will sail past it.
//
// The consequence for the user is the same either way: the tempo code
// emits four beats to the bar, like the pioneer kit, and the first one is
// the best guess available.  The difference is that the guess is now a
// MEASUREMENT when the music supports one, and it SAYS which it is.
// ---------------------------------------------------------------------------

// The ODF's history, by logical index: 0 is the oldest of the last
// AR_TEMPO_HIST_LEN frames, and AR_TEMPO_HIST_LEN-1 is the newest.
static float arTempoOdfAt(int logical) {
  const uint8_t head = (uint8_t)((arTempo.histIdx + AR_TEMPO_HIST_RING - AR_TEMPO_HIST_LEN)
                                 & (AR_TEMPO_HIST_RING - 1));
  return arTempo.hist[(head + logical) & (AR_TEMPO_HIST_RING - 1)];
}

// The ODF's peak within +/- win frames of the frame `lag` frames back.
//
// This is the beat's SALIENCE: how hard the beat hit.  The ODF is the
// band's rise in dB, so its peak is the transient's size, and the window
// covers the beat wherever the grid sits inside the tolerance.
static float arTempoOdfPeakAround(int lag, int win) {
  const int c = AR_TEMPO_HIST_LEN - 1 - lag;
  int lo = c - win;
  int hi = c + win;
  if (lo < 0) lo = 0;
  if (hi > AR_TEMPO_HIST_LEN - 1) hi = AR_TEMPO_HIST_LEN - 1;
  float m = 0.0f;
  for (int k = lo; k <= hi; k++) {
    const float v = arTempoOdfAt(k);
    if (v > m) m = v;
  }
  return m;
}

// One beat's salience into the bar's profile and into the measure's.
static void arTempoAccumulateBeat(uint32_t beatIndex, float sal) {
  const uint8_t p4  = (uint8_t)(beatIndex % 4);
  const uint8_t p16 = (uint8_t)(beatIndex % 16);
  arTempo.barSum[p4] += sal;
  arTempo.barSq[p4]  += sal * sal;
  arTempo.barCnt[p4]++;
  arTempo.measSum[p16] += sal;
  arTempo.measSq[p16]  += sal * sal;
  arTempo.measCnt[p16]++;
  // The profile's memory.  Halving the accumulators keeps their means and
  // their variance and lets the profile follow the music when it changes;
  // a profile that never forgot would carry the last track's downbeat
  // into the next one.
  if (arTempo.barCnt[p4] >= AR_TEMPO_DOWNBEAT_MAX) {
    for (int i = 0; i < 4; i++) {
      arTempo.barSum[i] *= 0.5f;
      arTempo.barSq[i]  *= 0.5f;
      arTempo.barCnt[i]  = (uint16_t)(arTempo.barCnt[i] / 2);
    }
    for (int i = 0; i < 16; i++) {
      arTempo.measSum[i] *= 0.5f;
      arTempo.measSq[i]  *= 0.5f;
      arTempo.measCnt[i]  = (uint16_t)(arTempo.measCnt[i] / 2);
    }
  }
}

// The profile's t-score: how far the strongest position's mean stands above
// the profile's mean, in standard errors.  `bestOut` gets the position.
//
// The variance is POOLED over the positions - the beats are the same
// beats, so the spread of the salience is one spread - and the standard
// error is that spread over the square root of the samples a position
// holds.
static float arTempoProfileT(const float *sum, const float *sq,
                              const uint16_t *cnt, int n, int *bestOut) {
  float meanSum = 0.0f;
  uint32_t total = 0;
  for (int p = 0; p < n; p++) { meanSum += sum[p]; total += cnt[p]; }
  if (total < (uint32_t)n * (uint32_t)AR_TEMPO_DOWNBEAT_MIN) return 0.0f;
  const float grand = meanSum / (float)total;
  float var = 0.0f;
  for (int p = 0; p < n; p++) {
    if (cnt[p] == 0) continue;
    const float m = sum[p] / (float)cnt[p];
    var += sq[p] - (float)cnt[p] * m * m;
  }
  const float sd = sqrtf(var / (float)(total - (uint32_t)n));
  const float se = sd / sqrtf((float)total / (float)n);
  int bestP = 0;
  float best = -1e30f;
  for (int p = 0; p < n; p++) {
    if (cnt[p] == 0) continue;
    const float m = sum[p] / (float)cnt[p];
    if (m > best) { best = m; bestP = p; }
  }
  if (bestOut) *bestOut = bestP;
  if (se <= 0.0f) return 0.0f;
  return (best - grand) / se;
}

// Score the profiles, once a measure.  A downbeat that has been found is
// KEPT - it is not re-decided every measure, because a decision that
// flickers would move the bar's numbering under the effect's feet.
//
// The first time a downbeat is found, the numbering does NOT shift there and
// then: it shifts at the next beat the measurement says is the downbeat.
// A shift in the middle of the bar moves the bar's position by up to three
// beats, and the beat strobe's flash jumps with it - a long flash in the
// middle of a bar, which reads as a stutter.
static void arTempoScoreDownbeat(void) {
  int p4 = 0, p16 = 0;
  const float t4  = arTempoProfileT(arTempo.barSum, arTempo.barSq, arTempo.barCnt, 4, &p4);
  const float t16 = arTempoProfileT(arTempo.measSum, arTempo.measSq, arTempo.measCnt, 16, &p16);
  arTempo.downbeatT = t4;
  arTempo.measureT  = t16;
  if (t4 >= AR_TEMPO_DOWNBEAT_T) {
    if (!arTempo.downbeatKnown) {
      arTempo.downbeatPos    = (uint8_t)p4;
      arTempo.downbeatKnown  = true;
      // The next beat whose position in the bar is p4 - that is where the
      // numbering shifts to, so the shift lands on a bar boundary.
      uint32_t at = (arTempo.beatIndex - (arTempo.beatIndex % 4u)) + (uint32_t)p4;
      if (at <= arTempo.beatIndex) at += 4u;
      arTempo.downbeatApplyAt = at;
    }
  }
  if (t16 >= AR_TEMPO_MEASURE_T) {
    if (!arTempo.measureKnown) {
      arTempo.measurePos   = (uint8_t)p16;
      arTempo.measureKnown = true;
      uint32_t at = (arTempo.beatIndex - (arTempo.beatIndex % 16u)) + (uint32_t)p16;
      if (at <= arTempo.beatIndex) at += 16u;
      arTempo.measureApplyAt = at;
    }
  }
}

// ---------------------------------------------------------------------------
// THE PROLINK REFERENCE
//
// (arTempoBeatInBar is defined with the other published numbers, below; it
// is declared here because the reference needs it.)
//
// The deck's beat grid is EXACT: it is the thing that made the music.  When
// it is connected, the board can measure this tracker against it, and that
// is the only reference that says whether the audio-only estimate is right.
//
// The deck publishes the fraction of a beat since the last boundary of ITS
// grid - and its grid is the SHIFTED one: the ProLink usermod moves the
// deck's grid EARLIER by its phase offset so that the boundary lands on the
// beat the board HEARS, not on the beat the deck announces (the deck's
// packet arrives after the beat it announces).
//
// So the reading here is a RESIDUAL, not the tracker's raw error: it is
// what is left of the difference between our detected beat and the audible
// grid once the offset has been applied.  At zero the two grids coincide;
// the phase offset setting moves this number one-for-one, which makes it
// the tuning readout - the number to watch while the offset is being set.
//
// The sign is unchanged: positive means our beat is LATE with respect to
// the grid.  It is wrapped into half a beat, so a beat that is early is not
// read as a beat that is very late, and it is averaged, so one late packet
// does not move the number.
//
// The bar's numbering is compared only when the two positions can be
// compared at all, and that is a ONE-SIDED window.
//
// What the comparison then measures is not "do the positions match" - they
// are not expected to, because our numbering is a convention and the deck's
// is the truth - but "did the two counters advance together".  The offset
// between them is expected to be a CONSTANT; a constant offset is a
// constant non-zero number, and it is the SLIPS that mean something has
// come apart.
//
// The deck's published position is the position of the grid cell that
// CONTAINS the reading instant.  Our beat is the beat we just detected.
// They are the same beat only if the deck's shifted boundary for OUR beat
// has already been crossed - that is, if err >= 0.  A negative err means the
// boundary has NOT been crossed: the deck is still on the PREVIOUS beat, and
// comparing the two positions would be comparing two different beats.
//
// So a comparison that would be wrong is SKIPPED rather than scored as a
// mismatch.  Skipping matters: a scored mismatch says the two grids
// disagree, and a grid that is right must not be reported as wrong.
//
// The upper bound (120 ms) is the other half of the same idea: past a
// quarter of a beat the two grids are too far apart for their bar positions
// to mean anything to each other.  A phase offset set too far therefore
// stops the comparison, and that is right too.
//
// It is called AT OUR BEAT, and not every frame: the deck's position in the
// bar belongs to its last beat, and it only means anything next to ours.
// ---------------------------------------------------------------------------
static uint8_t arTempoBeatInBar(void);

static void arTempoProLinkReference(void) {
  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  if (!prolink_connected_public || prolink_bpm_public <= 0.0f) {
    arTempo.refBpm = 0.0f;
    return;
  }
  arTempo.refBpm = prolink_bpm_public;
  const float beatMs = 60000.0f / prolink_bpm_public;

  // The residual is only meaningful while the deck's GRID is running.  When
  // it is not - no deck, or the beat packets stopped - the usermod publishes
  // a phase of zero, and a residual of zero would read as "in sync", which
  // is a lie: there is nothing to be in sync WITH.  So the readout is
  // marked invalid instead, and the tuning number is not published.
  if (prolink_beat_public < 1 || prolink_beat_public > 4) {
    arTempo.refHaveErr = false;
    return;
  }

  float err = prolink_beat_progress_public * beatMs;
  if (err > beatMs * 0.5f) err -= beatMs;
  if (!arTempo.locked) return;
  if (!arTempo.refHaveErr) {
    arTempo.refErrMs   = err;
    arTempo.refHaveErr = true;
  } else {
    arTempo.refErrMs += 0.1f * (err - arTempo.refErrMs);
  }
  const uint8_t deckBeat = prolink_beat_public;
  const uint8_t ourBeat  = arTempoBeatInBar();
  // The bar's position is compared only when the two positions can mean the
  // same thing - and that is a ONE-SIDED window, because of what the deck's
  // position actually IS.
  //
  // The deck's published position is the position of the grid cell that
  // contains the reading, not "the deck's beat".  The deck's grid is the
  // SHIFTED one - the usermod moves it by its phase offset so that it lands
  // on the beat the board hears - so its boundaries fall on the AUDIBLE
  // beats.  Our beat is the beat we just detected.
  //
  // They are the same beat only if the deck's boundary for our beat has
  // already been crossed, which is err >= 0.  A negative err means it has
  // NOT: the deck is still on the previous beat, and the two positions are
  // two different beats.  Those comparisons are SKIPPED, not counted as
  // mismatches - a grid that is right must not be reported as wrong.
  //
  // The upper bound is the other half: past 120 ms - a quarter of a beat at
  // 120 BPM - the grids are too far apart for their bar positions to mean
  // anything to each other.
  //
  // (Before the shift the comparison was meaningless for a different reason:
  // the deck's position arrived with the packet, ~40 ms after the audible
  // beat, while our beat was placed at the audible beat - so the two were a
  // beat apart and the agreement sat BELOW the 25 % that chance gives.)
  //
  // The AGE of the published grid is covered by the same one-sided window.
  // The grid is published from the usermod's main loop, so a reading can be
  // up to one loop period old; a reading taken just BEFORE a boundary
  // carries a phase of ~1.0, which wraps to an err of ~0 MINUS - so the cell
  // that has already turned over is skipped instead of compared.  The
  // ProLink info page's "grid N Hz" row says how old a reading can be.
  if (deckBeat >= 1 && deckBeat <= 4 && ourBeat >= 1 && ourBeat <= 4
      && err >= 0.0f && err < 120.0f) {
    // The OFFSET: the deck's position in the bar minus ours, modulo four.
    //
    // It is NOT expected to be zero, and that is the whole point of reading
    // it.  Our numbering is a CONVENTION - the first beat after the lock is
    // beat 1 - because an audio-only tracker cannot know the downbeat, and
    // the deck's numbering is the real one.  So the offset's VALUE says
    // nothing about the sync; what matters is that it does not MOVE.
    //
    // A constant offset means the two counters advance together, beat for
    // beat: the deck's grid and ours are the same grid, and our bar
    // numbering is simply rotated against the deck's.  A CHANGING offset
    // means one of the two skipped a beat - the deck dropped a packet, or
    // our grid double-advanced - and the two grids have come apart.
    //
    // (The old counter asked whether the two positions were EQUAL, which is
    // a coin that lands the same way for every beat: with a constant offset
    // it reads 0 % or 100 % and with a slipping one it reads about a
    // quarter, and neither number says anything about the sync.)
    const uint8_t offset = (uint8_t)((deckBeat + 4u - ourBeat) & 3u);
    if (arTempo.refCount > 0 && offset != arTempo.refOffset) {
      if (arTempo.refSlips < 65535) arTempo.refSlips++;
    } else {
      if (arTempo.refLocked < 65535) arTempo.refLocked++;
    }
    arTempo.refOffset   = offset;
    arTempo.refBeat     = deckBeat;
    arTempo.refOurBeat  = ourBeat;
    if (arTempo.refCount < 65535) arTempo.refCount++;
  }
  #else
  (void)0;
  #endif
}

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
    arTempo.needFullSearch = false;
    arTempo.fullSearchFailed = false;
  }

  // The gate.
  if (!gateOpen) {
    // The counter saturates: it is compared against a frame count, and it
    // must not wrap into a short value and look like a new episode.
    if (arTempo.gateShutFrames < 60000) arTempo.gateShutFrames++;
    // Once per episode - the exact test, not ">=", so the hold happens when
    // the gate shuts and not on every frame after it.
    if (arTempo.gateShutFrames == AR_TEMPO_GATE_SHUT_FRAMES) {
      // The music has stopped or changed: the EVIDENCE is thrown away, but
      // the GRID is not.  The grid is the cadence - it free-runs - and a
      // breakdown drops the beat out for a few seconds.  A grid that starts
      // again from nothing on every gap STUTTERS, and a stutter is worse
      // than a beat that is a little wrong.
      //
      // The tempo itself goes invalid if the music stays gone for
      // AR_TEMPO_HOLD_FRAMES: a track that has ENDED is not a breakdown, and
      // a strobe that flashes forever at a rate nobody is playing is worse
      // than a strobe that stops.
      arTempoHold();
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
    // The first frame has no previous level, so it has no RISE to report.
    //
    // It used to subtract a -120 dB sentinel, which turned the first frame
    // into a 100 dB spike: the ODF's running maximum jumped to it, and the
    // picker's threshold - 6 % of that maximum - then sat at 6 dB and
    // rejected every real onset for the five seconds the maximum took to
    // decay.  There is no onset in the first frame; the frame after it has
    // a level to compare against.
    //
    // The flag is its own, and not the frame counter: a REBASE sets the
    // frame counter back to zero in the middle of a run, and the frame
    // after it has a perfectly good level to compare against.
    float odf = 0.0f;
    if (arTempo.haveLastDb) {
      odf = db - arTempo.lastDb;
      if (odf < 0.0f) odf = 0.0f;    // half-wave rectified: a rise only
    }
    arTempo.lastDb = db;
    arTempo.haveLastDb = true;
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
        const float beatFrame = arTempo.nextBeat;
        arTempo.nextBeat += arTempo.period;
        arTempo.beatIndex++;
        // The beat's salience goes into the downbeat's profile: how hard
        // this beat hit, against the beats either side of it in the bar.
        //
        // `lag` is how many frames late the beat was noticed, and it is
        // normally zero or one.  The window is the ODF's peak around the
        // beat's own frame, so the salience is the transient's size and
        // not the level's.
        const int lag = (int)((float)arTempo.frameIndex - beatFrame + 0.5f);
        arTempoAccumulateBeat(arTempo.beatIndex,
                              arTempoOdfPeakAround(lag, AR_TEMPO_DOWNBEAT_WIN));
        // The deck's grid, if there is one: our beat against the deck's.
        arTempoProLinkReference();
      }
      // The profiles are scored once a measure.  A downbeat that has been
      // found is kept, so the bar's numbering cannot flicker.
      if ((arTempo.beatIndex & 15u) == 0u
          && arTempo.beatIndex != arTempo.downbeatEvalIndex) {
        arTempo.downbeatEvalIndex = arTempo.beatIndex;
        arTempoScoreDownbeat();
      }
    }

    // ---- the search -----------------------------------------------------
    // Unlocked, the search runs as soon as the last one finished: it is
    // looking for a rate and there is no rate to look around.  Locked, it
    // runs every AR_TEMPO_SEARCH_EVERY frames, and it is the narrow one -
    // unless the safety net is due, or the narrow search lost the beat.
    if (gateOpen) {
      if (!arTempo.sweepActive) {
        // The safety net: a whole-band search every AR_TEMPO_FULL_EVERY
        // frames, whatever the score says.  This is the only search that
        // can move the grid a long way, so it is the one that notices the
        // music changed.
        if (arTempo.locked
            && (int32_t)(arTempo.frameIndex - arTempo.lastFullFrame)
               >= AR_TEMPO_FULL_EVERY) {
          arTempo.needFullSearch = true;
        }
        const bool due = !arTempo.locked
                      || arTempo.needFullSearch
                      || ((int32_t)(arTempo.frameIndex - arTempo.lastSweepFrame)
                          >= AR_TEMPO_SEARCH_EVERY);
        if (due) arTempoSweepStart();
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
      // The search's clock moves with the frame counter.  It has to: leave
      // it behind and the next search is not due for another 65536 frames
      // - eleven minutes of a frozen grid.
      if (arTempo.lastSweepFrame >= AR_TEMPO_REBASE_FRAMES) {
        arTempo.lastSweepFrame -= AR_TEMPO_REBASE_FRAMES;
      } else {
        arTempo.lastSweepFrame = 0;
      }
      if (arTempo.lastFullFrame >= AR_TEMPO_REBASE_FRAMES) {
        arTempo.lastFullFrame -= AR_TEMPO_REBASE_FRAMES;
      } else {
        arTempo.lastFullFrame = 0;
      }
      // The onset ring moves with the base.  The frames are SIGNED, so the
      // subtraction is exact and an onset that happened before the rebase
      // lands at a negative frame - which is what it is: it is before the
      // new base.
      //
      // This used to be an unsigned subtraction, and it wrapped: every
      // frame in the ring is below 65536 at this point, so every one of
      // them became a frame near 4294967296.  The comb then saw all the
      // onsets at the same instant, every candidate scored the same, and
      // the prior chose 125.00 and re-anchored the grid to phase zero.
      for (uint16_t i = 0; i < arTempo.peakCount; i++) {
        const int idx = (arTempo.peakHead + AR_TEMPO_PEAK_RING - 1 - i) & (AR_TEMPO_PEAK_RING - 1);
        arTempo.peakFrame[idx] -= (int32_t)AR_TEMPO_REBASE_FRAMES;
      }
      // The gap test's reference moves with the ring.  It is left alone
      // when it is the sentinel - a negative one means "no onset yet".
      if (arTempo.prevPeakFrame >= 0) {
        arTempo.prevPeakFrame -= (int32_t)AR_TEMPO_REBASE_FRAMES;
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

// The beat in the bar.
//
// When the profile has found a downbeat, the numbering is put on it: the
// measured position is beat 1.  When it has not - which is what four-on-
// the-floor music gives, and it was MEASURED over sixty windows of real
// house music - the numbering is the CONVENTION: the first beat after the
// lock is beat 1.  The confidence is published either way, so a reader can
// tell a measurement from a convention.
//
// The numbering shifts at a BAR BOUNDARY, not the moment the downbeat is
// found.  Shifting it the moment it is found moves the bar's position by up
// to three beats, and the beat strobe's flash jumps with it - a long flash
// in the middle of the bar, which reads as a stutter.  So the shift waits
// for the next beat that the measurement says is the downbeat.
static uint8_t arTempoBeatInBar(void) {
  if (!arTempo.locked) return 0;
  const bool applied = arTempo.downbeatKnown
                    && arTempo.beatIndex >= arTempo.downbeatApplyAt;
  const uint32_t base = applied
                      ? (arTempo.beatIndex - (uint32_t)arTempo.downbeatPos)
                      : arTempo.beatIndex;
  return (uint8_t)((base % 4) + 1);
}

// The beat in the measure: the same, over sixteen beats.  One beat in
// sixteen that is different marks the measure.
static uint8_t arTempoBeatInMeasure(void) {
  if (!arTempo.locked) return 0;
  const bool applied = arTempo.measureKnown
                    && arTempo.beatIndex >= arTempo.measureApplyAt;
  const uint32_t base = applied
                      ? (arTempo.beatIndex - (uint32_t)arTempo.measurePos)
                      : arTempo.beatIndex;
  return (uint8_t)((base % 16) + 1);
}

// How sure is the downbeat?  The t-score of the bar's profile: zero when
// there is no profile yet, and the null's median is about two, so a reader
// can tell "no idea" from "a little" from "a lot".
static float arTempoDownbeatT(void) {
  return arTempo.locked ? arTempo.downbeatT : 0.0f;
}

// Is the downbeat a MEASUREMENT rather than a convention?
static bool arTempoDownbeatKnown(void) {
  return arTempo.locked && arTempo.downbeatKnown;
}

// The progress through the current beat, 0..1.
static float arTempoBeatProgress(void) {
  if (!arTempo.locked || arTempo.period <= 0.0f) return 0.0f;
  const float p = ((float)arTempo.frameIndex - arTempo.lastBeat) / arTempo.period;
  if (p < 0.0f) return 0.0f;
  if (p > 1.0f) return 1.0f;
  return p;
}

// The reference, for the publisher.
static float arTempoRefBpm(void)      { return arTempo.refBpm; }
static float arTempoRefErrMs(void)    { return arTempo.refErrMs; }
static uint8_t arTempoRefBeat(void)   { return arTempo.refBeat; }
static uint8_t arTempoRefOurBeat(void){ return arTempo.refOurBeat; }
static uint16_t arTempoRefCount(void) { return arTempo.refCount; }
static bool     arTempoRefValid(void) { return arTempo.refHaveErr; }
static uint16_t arTempoRefLocked(void){ return arTempo.refLocked; }
static uint16_t arTempoRefSlips(void) { return arTempo.refSlips; }
static uint8_t  arTempoRefOffset(void){ return arTempo.refOffset; }

// Is there a usable estimate?
//
// The grid is usable while it is RUNNING, and it stays running through a
// dip: the score goes under the floor on a fraction of a percent of the
// frames of real music, and an effect that drops out of the beat on one of
// them is worse than an effect that strobes on a grid that is about to move.
//
// The score itself is published as it is measured - the dip is visible there
// - and the search is judged the same way it is acted on: four searches in a
// row under the floor, and then it looks everywhere.
static bool arTempoValid(void) {
  // The GRID is what is valid - not the score.
  //
  // The score dips under the floor on 1.3 % of the frames of real music,
  // and it stays under it for whole BREAKDOWNS, where the beat drops out
  // for effect.  A grid that calls itself invalid on those frames STUTTERS:
  // the effect goes dark and comes back, and the flash lands in the wrong
  // place.  A stutter is worse than a beat that is a little wrong, so the
  // grid keeps running and the rate keeps being published; the SCORE is
  // published next to it, so the confidence is visible to whoever is
  // reading.
  //
  // The one thing that does stop the tempo is the music being gone for
  // AR_TEMPO_HOLD_FRAMES: a track that has ended is not a breakdown.
  return arTempo.locked && arTempo.gateShutFrames < AR_TEMPO_HOLD_FRAMES;
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
