/*
 * The AudioReactive publish path - see ar_publish.h.
 *
 * The lock lives in its own translation unit because it has to be ONE lock for
 * the whole path: the usermod publishes from its analysis task, the sound
 * simulator publishes from the main loop, and the effect that draws an
 * analyser reads from the LED loop. Three tasks, two cores, one lock.
 */

#include "ar_publish.h"

#include <atomic>
#include <esp_timer.h>     // the native timer, for the handoff's decay
#include <math.h>
#include <string.h>

// The one publish lock.
static std::atomic<uint8_t> arPublishLock{0};

ArPublishGuard::ArPublishGuard() {
  while (arPublishLock.exchange(1, std::memory_order_acquire)) { /* spin */ }
}

ArPublishGuard::~ArPublishGuard() {
  arPublishLock.store(0, std::memory_order_release);
}

// The published RTA frame: double buffered, the index names the published
// one. Both the buffer and the index are only touched under the lock.
static uint8_t arRtaBuf[2][NUM_RTA_CHANNELS] = {{0}};
static uint8_t  arRtaIdx = 0;                 // which buffer is published
static bool     arRtaValid = false;           // false until the first frame
// True while the published frame is a live measurement - see
// arSetRtaLive(). The frame is then stale, and may not be used.
static bool     arRtaLive = false;

void arPublishRtaBands(const uint8_t* bands) {
  ArPublishGuard guard;
  memcpy(arRtaBuf[arRtaIdx ^ 1], bands, NUM_RTA_CHANNELS);
  arRtaIdx ^= 1;
  arRtaValid = true;
}

void arCopyRtaBandsLocked(uint8_t* dest) {
  if (!arRtaValid) {
    memset(dest, 0, NUM_RTA_CHANNELS);
    return;
  }
  memcpy(dest, arRtaBuf[arRtaIdx], NUM_RTA_CHANNELS);
}

bool arRtaUsable(void) {
  ArPublishGuard guard;
  return arRtaValid && arRtaLive;
}

bool arCopyRtaBands(uint8_t* dest) {
  ArPublishGuard guard;
  if (!arRtaValid || !arRtaLive) {
    // Not a live measurement: the caller may not use it, so it gets nothing
    // to misuse. The copy is not made at all.
    memset(dest, 0, NUM_RTA_CHANNELS);
    return false;
  }
  memcpy(dest, arRtaBuf[arRtaIdx], NUM_RTA_CHANNELS);
  return true;
}

void arSetRtaLive(bool live) {
  ArPublishGuard guard;
  arRtaLive = live;
}

void arResetRtaBands() {
  ArPublishGuard guard;
  memset(arRtaBuf, 0, sizeof(arRtaBuf));
  arRtaIdx = 0;
  arRtaValid = false;
}

// The published ABSOLUTE band levels.
//
// Double buffered like the bands themselves, and under the same lock: a
// reader must never see half of one frame's levels and half of another's.
//
// The window they are read against travels with them, for the same reason:
// the auto-range moves the window, so the levels and the window have to come
// from the same frame or the axis is a lie.
static float arRtaDbBuf[2][NUM_RTA_CHANNELS] = {{0}};
static ArRtaScale arRtaScale = {};
static uint8_t arRtaDbIdx = 0;
static bool    arRtaDbValid = false;

void arPublishRtaDb(const float* db, const ArRtaScale& scale) {
  ArPublishGuard guard;
  memcpy(arRtaDbBuf[arRtaDbIdx ^ 1], db, NUM_RTA_CHANNELS * sizeof(float));
  arRtaScale = scale;
  arRtaDbIdx ^= 1;
  arRtaDbValid = true;
}

// The window that comes with the levels is the auto-range's, for the readers
// that want it. A reader that sets its own window - the RTA effect does,
// with its two sliders - passes nullptr and does not get one.
bool arCopyRtaDb(float* dest, ArRtaScale* scale) {
  ArPublishGuard guard;
  if (!arRtaDbValid || !arRtaLive) {
    for (int k = 0; k < NUM_RTA_CHANNELS; k++) dest[k] = -120.0f;
    if (scale) {
      scale->topDb = -120.0f;
      scale->rangeDb = 0.0f;
      scale->valid = 0;
    }
    return false;
  }
  memcpy(dest, arRtaDbBuf[arRtaDbIdx], NUM_RTA_CHANNELS * sizeof(float));
  if (scale) *scale = arRtaScale;
  return true;
}

// The published loudness frame.
//
// One struct and not a double buffer, like the tempo: the copy is made under
// the lock, so a reader cannot see half of it.
static ArLoudnessFrame arLoudFrame = {};
static bool arLoudValid = false;
static bool arLoudLive  = false;

void arPublishLoudness(const ArLoudnessFrame& l) {
  ArPublishGuard guard;
  arLoudFrame = l;
  arLoudValid = true;
}

bool arCopyLoudness(ArLoudnessFrame* dest) {
  ArPublishGuard guard;
  if (!arLoudValid || !arLoudLive) {
    dest->programme = -120.0f; dest->momentary = -120.0f;
    dest->momentaryMax = -120.0f; dest->shortTerm = -120.0f;
    dest->shortTermMax = -120.0f; dest->sliding10 = -120.0f;
    dest->lra = 0.0f; dest->truePeak = -120.0f; dest->truePeakNow = -120.0f;
    dest->valid = 0;
    return false;
  }
  *dest = arLoudFrame;
  return true;
}

void arSetLoudnessLive(bool live) {
  ArPublishGuard guard;
  arLoudLive = live;
}

// The published tempo frame.
//
// It is ONE struct and not a double buffer, because the payload is twelve
// bytes and the copy is made under the lock: a reader cannot see half of it.
static ArTempoFrame arTempoFrame = {};
static bool arTempoValid = false;   // false until the first frame
static bool arTempoLive = false;     // true while the frame is a live measurement

void arPublishTempo(const ArTempoFrame& t) {
  ArPublishGuard guard;
  arTempoFrame = t;
  arTempoValid = true;
}

void arCopyTempoLocked(ArTempoFrame* dest) {
  if (!arTempoValid) {
    dest->bpm = 0.0f; dest->beatProgress = 0.0f; dest->score = 0.0f;
    dest->quality = 0.0f; dest->qualityPeak = 0.0f;
    dest->driftMs = 0.0f; dest->driftBpm = 0.0f;
    dest->hold = 0;
    dest->beatInBar = 0; dest->valid = 0;
    dest->beatInMeasure = 0; dest->beatInMeasure32 = 0;
    dest->downbeatKnown = 0; dest->downbeatSet = 0;
    dest->downbeatT = 0.0f; dest->beatCount = 0;
    dest->refBpm = 0.0f; dest->refErrMs = 0.0f;
    dest->refBeat = 0; dest->refOurBeat = 0;
    dest->refCount = 0; dest->refLocked = 0; dest->refSlips = 0;
    dest->refOffset = 0;
    return;
  }
  *dest = arTempoFrame;
}

bool arTempoUsable(void) {
  ArPublishGuard guard;
  return arTempoValid && arTempoLive;
}

bool arCopyTempo(ArTempoFrame* dest) {
  ArPublishGuard guard;
  if (!arTempoValid || !arTempoLive) {
    // Not a live measurement: the caller may not use it, so it gets nothing
    // to misuse. The copy is not made at all.
    memset(dest, 0, sizeof(*dest));
    return false;
  }
  *dest = arTempoFrame;
  return true;
}

void arSetTempoLive(bool live) {
  ArPublishGuard guard;
  arTempoLive = live;
}

// The HANDOFF: the correction that keeps the light from teleporting when
// the grid moves.
//
// The grid is either the deck's or the estimator's, and when it changes,
// the BAR's phase changes with it - because the two number the beats
// differently.  The deck's numbering is the track's real one (the downbeat
// is where the downbeat is); the estimator's is a convention (an
// audio-only tracker cannot know where the bar starts, so its beat one is
// the first beat after the lock).  The two are therefore normally a beat
// or two apart, and a light that shows the bar's position would JUMP a
// quarter of the bar when the deck takes over.
//
// It does not jump: the phase is CORRECTED, and the correction is let go
// over about a bar.  So the light travels to the new grid instead of
// teleporting to it - and because the correction is let go over four
// beats while the bar turns over in four, the light never goes BACKWARDS.
// It just runs a little slower for a bar and then carries on as if it had
// always been there.
//
// The same goes for a grid that moves while it stays the same grid: a
// deck that re-anchors, a tracker that skips a beat, a packet that
// arrives late.  Any STEP in the phase is taken back the same way.  So
// the rule is not "the deck arrived" but "the phase moved further than
// the music can explain" - and the source changing hands is just the
// biggest case of that.
//
// The beat NUMBER is not corrected: it may change when the deck takes
// over, and that is information - the deck's downbeat is the real one -
// not a glitch.
static float    arHandoffCorr   = 0.0f;   // the correction, in beats
static uint32_t arHandoffAt     = 0;      // when it was set, ms
static uint8_t  arHandoffSrc    = 0;      // 0 none, 1 the estimator, 2 the deck
static float    arHandoffAcc    = 0.0f;   // the source's phase, unwrapped, in beats
static uint32_t arHandoffAccAt  = 0;      // when that was, ms
static float    arHandoffBpm    = 0.0f;   // the tempo of that source

// How fast the correction is let go, in BEATS.  Four beats is a bar: the
// light slows for a bar and is back on the grid by the next one.
#define AR_HANDOFF_TAU_BEATS 4.0f

// How far the phase may step before it counts as a jump rather than the
// music moving.  A tenth of a beat is about 45 ms - far more than the
// phase of a beat detector wavers, and far less than the quarter of a
// bar that the change of grid is worth.
#define AR_HANDOFF_JUMP_BEATS 0.10f

static void arApplyHandoff(ArBeatSource* b) {
  const uint8_t src = b->live ? (b->fromProLink ? 2 : 1) : 0;
  const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

  if (src == 0) {
    // No beat at all: the effects are on their own clocks, so there is
    // nothing to correct, and the next beat starts from scratch.
    arHandoffSrc = 0;
    arHandoffCorr = 0.0f;
    b->phaseBeats = 0.0f;
    b->barCount = 0;
    return;
  }

  const float phase  = (float)(b->beatInBar - 1u) + b->beatProgress;
  const float beatMs = (b->bpm > 1.0f) ? (60000.0f / b->bpm) : 500.0f;

  // Where the phase SHOULD be by now: the last one, plus the time that
  // has passed, at the source's own tempo.
  //
  // The old phase is caught up to the moment of the comparison first.
  // This function is called from more than one place - the effects, and
  // the JSON - and they call it at very different rates, so the old
  // phase can be a long way behind.  Without the catch-up the comparison
  // would be of two moments seconds apart, and the correction would be a
  // whole number of beats wrong.
  const float oldBeatMs = (arHandoffBpm > 1.0f) ? (60000.0f / arHandoffBpm) : beatMs;
  const float elapsed   = (float)(now - arHandoffAccAt) / oldBeatMs;
  const float expected  = arHandoffAcc + elapsed;

  // The step: how much further the phase moved than the music explains,
  // the short way round the bar.
  float step = phase - expected;
  while (step >  2.0f) step -= 4.0f;
  while (step < -2.0f) step += 4.0f;

  if (src != arHandoffSrc) {
    if (arHandoffSrc == 0) {
      // The first beat of a run: nothing to correct.
      arHandoffCorr = 0.0f;
    } else {
      // The grid changed hands: the whole difference is a step, and it is
      // taken back, so the phase on show does not move at all.
      arHandoffCorr -= step;
      arHandoffAt = now;
    }
    arHandoffSrc = src;
    arHandoffAcc = phase;      // the new grid's phase starts here
  } else if (fabsf(step) > AR_HANDOFF_JUMP_BEATS) {
    // The same grid, but it STEPPED.
    //
    // A step of a WHOLE BEAT or more from the DECK is the deck's own
    // numbering moving - the song changed, and the bar with it - and
    // that is INFORMATION, not a glitch.  The deck's position in the
    // song is transmitted in beats, so a new song restarts it, and the
    // beat in the bar jumps with it.
    //
    // Taking that step back would be wrong: the light would run slowly
    // for a bar, appear to reverse, and then drift to the place it was
    // going to anyway.  A grid that loops must take the light with it -
    // the light goes to the new bar and STAYS there.
    //
    // The deck's PHASE stepping (a packet that arrived late, an anchor
    // that moved) is a different thing: that is a glitch, and it is
    // taken back the way it always was.  A dropped beat packet does not
    // step the phase at all - the anchor holds and the phase keeps
    // running - so a whole-beat step from the deck is its numbering
    // moving and nothing else.
    if (src == 2 && fabsf(step) >= 0.5f) {
      // The deck moved its grid on purpose: re-anchor on it, with no
      // correction to let go.
      arHandoffCorr = 0.0f;
      arHandoffAcc = phase;
    } else {
      arHandoffCorr -= step;
      arHandoffAt = now;
      arHandoffAcc = expected + step;
    }
  } else {
    // The phase is going where it should.  The correction is let go, so
    // the phase glides onto the grid instead of sitting beside it.
    arHandoffAcc = expected;
    const float since = (float)(now - arHandoffAt) / beatMs;
    arHandoffCorr *= expf(-since / AR_HANDOFF_TAU_BEATS);
  }

  float p = phase + arHandoffCorr;
  while (p >= 4.0f) p -= 4.0f;
  while (p <  0.0f) p += 4.0f;
  b->phaseBeats = p;

  // The bar number, on the SAME grid as the phase: the unwrapped phase
  // divided by the beats in a bar.
  //
  // The master's COUNT cannot answer this.  With a deck driving, that
  // count is the deck's position in the song, and it rides the status
  // stream - which is not the beat rate - so its bar boundary can be a
  // beat away from the phase's.  A light that turns round when the COUNT
  // says the bar turned round reverses in the middle of the bar, and a
  // sub-effect that changes when the COUNT says the bar turned round
  // changes in the middle of the bar too.
  //
  // The unwrapped phase is the exact position in beats, and it is what the
  // light follows, so the bar it gives is the bar the light is in.
  const float accBar = floorf(arHandoffAcc / 4.0f);
  b->barCount = (accBar > 0.0f) ? (uint32_t)accBar : 0u;

  arHandoffAccAt = now;
  arHandoffBpm   = b->bpm;
}

// The beat source: ProLink first, the estimator second.
//
// The two rules are the ones the beat-strobe effect has always used, moved
// here so that there is only ONE of them. The effect and the web GUI both
// ask this function, so the wall and the screen cannot disagree about
// where the beat is.
ArBeatSource arGetBeatSource(bool ignoreProLink) {
  ArBeatSource b = {};

  // The estimator's frame, read first: even when the DECK is the clock,
  // the user's own mark of the downbeat lives in it.
  ArTempoFrame tf;
  const bool haveTf = arCopyTempo(&tf);

  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  // ProLink globals - file-scope volatile in usermods_list.cpp. The
  // beatInMeasure sentinel 0 ("no packet yet") falls through, so there is
  // no beat before the first CDJ hello.
  extern volatile float   prolink_bpm_public;            // beats per minute (0 = unknown)
  extern volatile uint8_t prolink_beat_public;           // beat-in-measure 1..4, in the SHIFTED grid
  extern volatile float   prolink_beat_progress_public;  // 0..1 within the current beat
  extern volatile bool    prolink_connected_public;      // true while following a master CDJ
  extern volatile uint32_t prolink_beats_elapsed_public;   // the deck's beat in the song

  if (!ignoreProLink && prolink_connected_public &&
      prolink_bpm_public > 0.0f && prolink_beat_public != 0u) {
    b.bpm           = prolink_bpm_public;
    b.beatInBar     = prolink_beat_public;
    b.beatProgress  = prolink_beat_progress_public;
    b.live          = 1;
    b.downbeatKnown = 1;      // a CDJ knows its own downbeat
    b.fromProLink   = 1;

    // THE USER'S OVERRIDE OF THE DECK'S NUMBERING.
    //
    // The deck's beat one, two, three and four are still used - the deck
    // is still the clock, and its tempo and its phase are its own - but
    // the beat the user marked by ear is the downbeat, so the deck's
    // numbering is ROTATED to it.  That is the whole of the override: an
    // offset from the deck's packets, and nothing else.
    const bool override = haveTf && tf.deckOverride != 0;
    b.deckOverride = override ? 1 : 0;
    if (override) {
      b.beatInBar = (uint8_t)((((uint32_t)prolink_beat_public - 1u + 4u
                                - (uint32_t)tf.deckDownbeat) & 3u) + 1u);
    }

    // The BEAT COUNT: the deck's beat in the song.  With the override on
    // it is counted from the MARK, so it restarts when the user marks
    // the downbeat - the same thing the master's counter does, and the
    // reason an effect that counts on this realigns with the mark.
    uint32_t deckBeats = prolink_beats_elapsed_public;
    if (override && tf.deckElapsedAtMark > 0u &&
        prolink_beats_elapsed_public > tf.deckElapsedAtMark) {
      deckBeats = prolink_beats_elapsed_public - tf.deckElapsedAtMark;
    }
    b.beatCount = deckBeats;

    // The THIRTY-TWO: the deck's beat in the song, which we ASSUME is set
    // to a measure start.  But the USER's mark of the downbeat overrides
    // it: he has said by ear where the bar starts, and that beats the
    // deck's numbering.  With the override on, the thirty-two is counted
    // from the mark - the deck's beat count at the moment he marked it.
    if (haveTf && tf.downbeatSet != 0 && !override) {
      b.beatInMeasure32 = tf.beatInMeasure32;
    } else if (deckBeats > 0u) {
      b.beatInMeasure32 = (uint8_t)(((deckBeats - 1u) % 32u) + 1u);
    }
    arApplyHandoff(&b);
    return b;
  }
  #endif

  // The estimator.
  //
  // arCopyTempo() returns false when the published frame is not a LIVE
  // measurement - which is what a board receiving its sound over the
  // network publishes, because it has no audio to measure. The answer is
  // then "not live" rather than a stale number.
  if (haveTf && tf.valid != 0 && tf.bpm > 0.0f &&
      tf.beatInBar >= 1 && tf.beatInBar <= 4) {
    b.bpm          = tf.bpm;
    b.beatInBar    = tf.beatInBar;
    b.beatProgress = tf.beatProgress;
    b.live         = 1;
    // The downbeat is the DECK's, or the USER's. The estimator's own is
    // not used, even when it believes it has one: the sound does not know
    // where the bar starts (audio_reactive.h). A downbeat the user SET is
    // as good as the deck's.
    b.downbeatKnown = (tf.downbeatSet != 0) ? 1 : 0;
    b.downbeatSet   = tf.downbeatSet;
    // The master's BEAT COUNTER: it restarts when the user marks the
    // downbeat, so it is the number an effect counts on to stay with
    // the mark.
    b.beatCount      = tf.beatCount;
    b.beatInMeasure32 = tf.beatInMeasure32;
    b.locked        = tf.hold;
    b.driftMs       = tf.driftMs;
    // The drift as a PERCENTAGE of the beat: how far off the beat the
    // grid is, in units a person can read at a glance. A beat at 120 BPM
    // is 500 ms, so 10 ms of phase error is 2 %.
    const float beatMs = 60000.0f / tf.bpm;
    b.driftPct = (beatMs > 0.0f) ? (tf.driftMs / beatMs * 100.0f) : 0.0f;
  }
  arApplyHandoff(&b);
  return b;
}

void arResetTempo() {
  ArPublishGuard guard;
  // The handoff correction goes too: after a reset there is no grid to
  // be out of step with.
  arHandoffCorr  = 0.0f;
  arHandoffAt    = 0;
  arHandoffSrc   = 0;
  arHandoffAcc   = 0.0f;
  arHandoffAccAt = 0;
  arHandoffBpm   = 0.0f;
  arTempoFrame.bpm = 0.0f;
  arTempoFrame.beatProgress = 0.0f;
  arTempoFrame.score = 0.0f;
  arTempoFrame.beatInBar = 0;
  arTempoFrame.valid = 0;
  arTempoFrame.beatInMeasure = 0;
  arTempoFrame.downbeatKnown = 0;
  arTempoFrame.downbeatSet = 0;
  arTempoFrame.downbeatT = 0.0f;
  arTempoFrame.beatCount = 0;
  arTempoFrame.refBpm = 0.0f;
  arTempoFrame.refErrMs = 0.0f;
  arTempoFrame.refBeat = 0;
  arTempoFrame.refOurBeat = 0;
  arTempoFrame.refCount = 0;
  arTempoFrame.refLocked = 0;
  arTempoFrame.refSlips = 0;
  arTempoFrame.refOffset = 0;
  arTempoValid = false;
}
