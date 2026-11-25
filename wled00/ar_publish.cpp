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
#include <esp_random.h>    // the hardware random, for the fake fill's roll
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
    dest->downbeatGen = 0;
    dest->fakeFill = 0;
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

// The ProLink MARK's REBASE.
//
// The bar number is the grid's own count, and it counts from wherever the
// grid started - which is not where the user says the bar starts.  So the
// mark REBASES it: the bar the user marked is bar zero, and the count runs
// from there.
//
// The rebase is the grid's position at the START of the beat the user
// marked.  That beat is beat one of the bar - the rotation in
// arGetBeatSource() makes it so - so this is the bar's start, and the
// count is exact on the downbeat.
//
// The deck's OWN count cannot be used here.  It is the deck's position in
// the song, and it rides the status stream, which is not the beat rate:
// on a real deck it arrives about once a second, so it jumps two or
// three beats at a time.  The grid's phase runs free between the beat
// packets and is exact, so the counters are counted on THAT.
static float    arHandoffDeckAtMark = 0.0f;   // the grid's position at the marked beat
static uint32_t arHandoffMarkGen    = 0;      // the mark's identity, as it was last time
static uint8_t  arHandoffMarkOn     = 0;      // 1 when a mark was in force last time

// THE FAKE FILL's roll.
//
// Which measure the roll was made for - the measure's number and the mark
// it was counted from, so a new grid rolls again - and what the roll said.
static uint64_t arFakeFillMeasure   = 0xFFFFFFFFFFFFFFFFull;
static bool     arFakeFillOn        = false;

// The chance that a four-bar measure ends in a fill, in PERCENT.
//
// 25 for now - one measure in four.  It was picked for the first test: if
// the fakes read well it becomes a setting the user can turn, and if they
// do not, it goes back to zero.
#define AR_FAKE_FILL_PERCENT 25

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
      //
      // The unwrapped phase moves WITH the step rather than being reset to
      // the phase: the phase is the position inside the bar, and throwing
      // the accumulator away would take the bar number - and with it the
      // sweep's direction - back to zero.  On a deck loop the grid really
      // does move back, and the bar number has to move with it.
      arHandoffCorr = 0.0f;
      arHandoffAcc  = expected + step;
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
  //
  // WITH THE USER'S MARK ON, the number is counted from the mark: the bar
  // the user marked is bar zero.  That is what makes the mark a real
  // override and not only a rotation of the beat number - the effect's
  // cycle, its sweep direction and its sixteen-beat phrase all start again
  // on the beat the user marked.
  //
  // The rebase is the grid's position at the START of the marked beat: the
  // phase is the position inside the current beat, and the marked beat is
  // markBack whole beats back.  The user's own mark is the beat sounding
  // now, so markBack is zero for it - and the song's phrase is a mark in
  // the PAST, so markBack is how far back it is.
  if (b->markOn) {
    // The rebase is taken when the mark CHANGES - the user marks a beat,
    // or the song moves to another phrase - and held until then.  A mark
    // that comes back after being off is a new mark too, even if it is
    // the same beat: the counters have moved on since.
    if (b->markGen != arHandoffMarkGen || !arHandoffMarkOn) {
      arHandoffMarkGen = b->markGen;
      arHandoffDeckAtMark = arHandoffAcc - (float)b->markBack - phase;
    }
    arHandoffMarkOn = 1;
    const float rel = arHandoffAcc - arHandoffDeckAtMark;
    const float relBar = floorf(rel / 4.0f);
    b->barCount = (relBar > 0.0f) ? (uint32_t)relBar : 0u;
  } else {
    arHandoffMarkOn = 0;
    const float accBar = floorf(arHandoffAcc / 4.0f);
    b->barCount = (accBar > 0.0f) ? (uint32_t)accBar : 0u;
  }

  // THE FAKE FILL.
  //
  // A Pioneer knows where its fills are: the phrase packet carries a fill
  // flag and the beat the fill starts on, so a fill is the TAIL of a
  // phrase.  The other sources know nothing about the song - StageLinq has
  // no phrase structure at all, Ableton Link has a beat and a phase, and
  // the sound says nothing.
  //
  // So the board can INVENT one, when the user asks for it: the last bar
  // of a four-bar measure, some of the time.  A fill is the tail of
  // something, so the fake one is the tail of the measure - the fourth
  // bar, and it ends with it.  How often is AR_FAKE_FILL_PERCENT.
  //
  // The roll is made ONCE per measure, when the measure changes, so the
  // whole measure agrees on whether it ends in a fill.  The mark is part
  // of the roll's identity: a new grid is a new measure, and a new
  // chance.
  //
  // A Pioneer is never given a fake fill: it knows the song, and a fill
  // it does not have is a fill it does not want.
  b->fakeFillOn = 0;
  if (b->fakeFill && b->sourceId != AR_SRC_PROLINK && b->live) {
    const uint64_t id = ((uint64_t)b->markGen << 32) | (uint64_t)(b->barCount / 4u);
    if (id != arFakeFillMeasure) {
      arFakeFillMeasure = id;
      // The roll: AR_FAKE_FILL_PERCENT of the measures, and no more.
      arFakeFillOn = (esp_random() % 100u) < (uint32_t)AR_FAKE_FILL_PERCENT;
    }
    if (arFakeFillOn && (b->barCount % 4u) == 3u) b->fakeFillOn = 1;
  } else {
    arFakeFillMeasure = 0xFFFFFFFFFFFFFFFFull;
    arFakeFillOn = false;
  }

  arHandoffAccAt = now;
  arHandoffBpm   = b->bpm;
}

// ---------------------------------------------------------------------------
// The deck that is driving.
//
// Each deck usermod publishes its numbers in its own file-scope globals, and
// this asks them.  Only ONE deck drives at a time: the first one that is
// really there - connected, with a rate and a beat - wins, and the other is
// ignored until this one goes away.
//
// The three sources are different protocols on different networks: a
// Pioneer CDJ over Pro DJ Link, a Denon DJ unit over StageLinq, and
// Ableton Live over Link.  The core does not know about any of them beyond
// the numbers they publish, and it asks for them in the order they were
// written: the Pioneer first, then the Denon, then Ableton.
// ---------------------------------------------------------------------------
// The beat a MoonModules sender decided, as the AudioReactive usermod received
// it. The same volatile-globals handover the deck usermods use.
static volatile float    arNetBeatBpm      = 0.0f;
static volatile uint8_t  arNetBeatInBar    = 0;
static volatile float    arNetBeatProgress = 0.0f;
static volatile uint32_t arNetBeatElapsed  = 0;
static volatile uint32_t arNetBeatAtMs     = 0;
static constexpr uint32_t AR_NET_BEAT_STALE_MS = 2000;   // a sender silent this long has stopped
static inline uint32_t arNetNowMs() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }   // the native timer, as the handoff uses

void arPublishNetworkBeat(float bpm, uint8_t beatInBar, float beatProgress, uint32_t beatsElapsed) {
  arNetBeatBpm      = bpm;
  arNetBeatInBar    = beatInBar;
  arNetBeatProgress = beatProgress;
  arNetBeatElapsed  = beatsElapsed;
  arNetBeatAtMs     = arNetNowMs();
}

bool arGetSourceBeat(ArSourceBeat* d) {
  if (!d) return false;
  memset(d, 0, sizeof(*d));

  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  {
    // ProLink globals - file-scope volatile in usermods_list.cpp. The
    // beatInMeasure sentinel 0 ("no packet yet") falls through, so there is
    // no beat before the first CDJ hello.
    extern volatile float   prolink_bpm_public;            // beats per minute (0 = unknown)
    extern volatile uint8_t prolink_beat_public;           // beat-in-measure 1..4, in the SHIFTED grid
    extern volatile float   prolink_beat_progress_public;  // 0..1 within the current beat
    extern volatile bool    prolink_connected_public;      // true while following a master CDJ
    extern volatile uint32_t prolink_beats_elapsed_public;   // the deck's beat in the song
    extern volatile uint8_t prolink_bars_elapsed_public;    // the deck's own count of bars

    if (prolink_connected_public && prolink_bpm_public > 0.0f && prolink_beat_public != 0u) {
      d->bpm          = prolink_bpm_public;
      d->beatInBar    = prolink_beat_public;
      d->beatProgress = prolink_beat_progress_public;
      d->beatsElapsed = prolink_beats_elapsed_public;
      d->barsElapsed  = prolink_bars_elapsed_public;
      d->connected    = 1;
      d->sourceId     = AR_SRC_PROLINK;
      return true;
    }
  }
  #endif

  #if defined(USERMOD_DENON_STAGELINQ)
  {
    // The StageLinq globals - the same five numbers, from a Denon DJ unit.
    extern volatile float    stagelinq_bpm_public;           // beats per minute (0 = unknown)
    extern volatile uint8_t  stagelinq_beat_public;          // beat-in-bar 1..4, on the deck's own grid
    extern volatile float    stagelinq_beat_progress_public; // 0..1 within the current beat
    extern volatile bool     stagelinq_connected_public;     // true while a deck is driving
    extern volatile uint32_t stagelinq_beats_elapsed_public;  // the deck's beat in the song

    if (stagelinq_connected_public && stagelinq_bpm_public > 0.0f && stagelinq_beat_public != 0u) {
      d->bpm          = stagelinq_bpm_public;
      d->beatInBar    = stagelinq_beat_public;
      d->beatProgress = stagelinq_beat_progress_public;
      d->beatsElapsed = stagelinq_beats_elapsed_public;
      // StageLinq has no bar counter of its own - but its beat counter is
      // exact, so the bars are simply the beats over four.
      d->barsElapsed  = stagelinq_beats_elapsed_public / 4u;
      d->connected    = 1;
      d->sourceId     = AR_SRC_STAGELINQ;
      return true;
    }
  }
  #endif

  #if defined(USERMOD_ABLETON_LINK)
  {
    // Ableton Live's Link - the same numbers again, from a DAW rather than
    // a deck.  It is LAST in the order of preference: the decks are better
    // sources for a rig, because they publish the music's own grid, and
    // Live is the one that is there when nothing else is.
    extern volatile float    abletonlink_bpm_public;           // beats per minute (0 = unknown)
    extern volatile uint8_t  abletonlink_beat_public;          // beat-in-bar 1..4, on the session's grid
    extern volatile float    abletonlink_beat_progress_public; // 0..1 within the current beat
    extern volatile bool     abletonlink_connected_public;     // true while a session is live
    extern volatile uint32_t abletonlink_beats_elapsed_public;  // the phase, in beats

    if (abletonlink_connected_public && abletonlink_bpm_public > 0.0f && abletonlink_beat_public != 0u) {
      d->bpm          = abletonlink_bpm_public;
      d->beatInBar    = abletonlink_beat_public;
      d->beatProgress = abletonlink_beat_progress_public;
      d->beatsElapsed = abletonlink_beats_elapsed_public;
      // Link has no bar counter of its own - but its beat count is exact,
      // so the bars are simply the beats over four.
      d->barsElapsed  = abletonlink_beats_elapsed_public / 4u;
      d->connected    = 1;
      d->sourceId     = AR_SRC_ABLETON;
      return true;
    }
  }
  #endif

  // Another board's beat, received over the network: after the decks and
  // Ableton, so a deck this board hears itself wins. Between packets the phase
  // runs on at the received tempo.
  // A board that never received a beat compares one zero here and goes on.
  const uint32_t at = arNetBeatAtMs;
  if (at != 0u) {
    const uint32_t now = arNetNowMs();
    const float    bpm = arNetBeatBpm;
    const uint8_t  bar = arNetBeatInBar;
    if ((now - at) < AR_NET_BEAT_STALE_MS && bpm > 0.0f) {
      const float    beats = arNetBeatProgress + (float)(now - at) * bpm / 60000.0f;
      const uint32_t whole = (uint32_t)beats;
      d->bpm          = bpm;
      d->beatProgress = beats - (float)whole;
      d->beatsElapsed = arNetBeatElapsed + whole;
      // AI: below section was generated by an AI
      // The format lets a sender leave the place in the bar out (0). It is then counted from the
      // beat count: everything after this takes 1..4, and the handoff never ends on a 0.
      d->beatInBar    = (bar >= 1u && bar <= 4u) ? (uint8_t)(((bar - 1u + whole) % 4u) + 1u)
                                                 : (uint8_t)((d->beatsElapsed % 4u) + 1u);
      // AI: end
      d->barsElapsed  = d->beatsElapsed / 4u;
      d->connected    = 1;
      d->sourceId     = AR_SRC_NETWORK;
      return true;
    }
  }

  return false;
}

// Apply a deck's beat to the answer.
//
// This is the deck's half of the arbitration, and it does not care WHICH
// deck it is: a Pioneer over Pro DJ Link and a Denon over StageLinq both
// say the same five things, and the board treats them the same way - the
// deck's numbering is the real one, and the user's mark rotates it.
static void arApplySourceBeat(ArBeatSource* b, const ArSourceBeat& src,
                             bool haveTf, const ArTempoFrame& tf) {
  b->bpm           = src.bpm;
  b->beatInBar     = src.beatInBar;
  b->beatProgress  = src.beatProgress;
  b->live          = 1;
  b->downbeatKnown = 1;      // a deck knows its own downbeat
  b->fromProLink   = 1;
  b->sourceId      = src.sourceId;
  b->songPhrase    = haveTf ? (tf.songPhrase ? 1 : 0) : 0;

  // THE MARK: the beat that is beat one.
  //
  // Two things can say where the bar starts, and they say the SAME
  // thing: the user, marking a beat by ear, and the SONG's phrase - the
  // chorus, the fill - which the deck knows and the board does not.
  //
  // The deck's beat one, two, three and four are still used - the deck
  // is still the clock, and its tempo and its phase are its own - but
  // the marked beat is the downbeat, so the deck's numbering is
  // ROTATED to it.  That is the whole of the mark: an offset from the
  // deck's packets, and nothing else.
  //
  // The counters that describe a BAR - the bar number and the
  // sixteen-beat phrase - are counted from the mark as well, in
  // arApplyHandoff(): the marked bar is bar zero, so the effect's cycle
  // and its phrase restart on it.  Without that the mark was only a
  // rotation of the beat number, and everything that counted on the bar
  // carried on counting from wherever it had got to.
  //
  // THE PHRASE WINS when the user has asked for it: it knows the song,
  // and the user asked for the song.
  {
    const bool wantPhrase = haveTf && tf.songPhrase != 0;
    ArSongPhrase ph;
    const bool havePhrase = wantPhrase && arCopySongPhrase(&ph) && ph.index >= 0;

    if (havePhrase) {
      // The phrase's START is the downbeat.
      //
      // The rotation needs no counting: the phrase's start beat says
      // where in the bar it falls - the beat in the bar is (start - 1)
      // mod 4 - and the deck's beat in the bar is exact.
      b->markOn   = 1;
      b->markBeat = (uint8_t)((ph.startBeat - 1u) & 3u);
      // How far back the marked beat is: the beats since the phrase
      // started.  The phrase's beats and the deck's count are the same
      // numbering, so the difference is exact - even when the count
      // itself is a second old.
      b->markBack = (ph.currentBeat > ph.startBeat)
                  ? (ph.currentBeat - ph.startBeat) : 0u;
      // The mark's identity: the phrase's own generation, so a new
      // phrase is a new mark and the counters are rebased on it.
      b->markGen  = ph.generation | AR_MARK_GEN_PHRASE;
      b->markAt   = ph.startBeat;
      b->phraseOn   = 1;
      b->phraseFill = ph.fill;
      b->phraseKind = ph.kind;
      b->phraseIndex = ph.index;
      b->phraseIn   = b->markBack;
    } else if (haveTf && tf.deckOverride != 0 && tf.deckSourceId == src.sourceId) {
      // The user's own mark.  It is the beat sounding NOW, so nothing is
      // behind it.
      //
      // It belongs to the deck it was taken ON: a mark made while the
      // Denon was playing must not rotate the Pioneer's numbering, and the
      // other way round.
      b->markOn   = 1;
      b->markBeat = tf.deckDownbeat;
      b->markBack = 0;
      b->markGen  = tf.deckElapsedAtMark;
      b->markAt   = tf.deckElapsedAtMark;
    }
  }
  const bool override = b->markOn != 0;
  b->deckOverride = override ? 1 : 0;
  if (override) {
    b->beatInBar = (uint8_t)((((uint32_t)src.beatInBar - 1u + 4u
                              - (uint32_t)b->markBeat) & 3u) + 1u);
  }

  // The BEAT COUNT: the deck's beat in the song.  With the override on
  // it is counted from the MARK, so it restarts when the user marks
  // the downbeat - the same thing the master's counter does, and the
  // reason an effect that counts on this realigns with the mark.
  //
  // The count itself rides the deck's own stream, which a real deck sends
  // about once a second: it jumps two or three beats at a time.  So it
  // is not what anything counts on - the grid's own phase is - and it
  // is here as the deck's own answer, for a reader that wants it.
  //
  // The deck's count is ONE-BASED: the count is 1 on the deck's first
  // beat, and (count - 1) mod 4 is the beat in the bar.  The count
  // rebased on the mark keeps that convention, so the marked beat is
  // count 1.
  uint32_t deckBeats = src.beatsElapsed;
  if (override && src.beatsElapsed >= b->markAt) {
    deckBeats = src.beatsElapsed - b->markAt + 1u;
  }
  b->beatCount = deckBeats;

  arApplyHandoff(b);

  // The THIRTY-TWO: the position in the eight-bar measure, on the SAME
  // grid as the bar number and the sixteen-beat phrase.
  //
  // It is counted on the grid's phase instead of on the deck's count,
  // which is exact: the bar number and the beat in the bar are both
  // there already.
  //
  // And because the bar number is counted from the user's mark when
  // the override is on, the thirty-two starts on the beat they marked
  // - the same anchor as the bar and the phrase.
  b->beatInMeasure32 = (uint8_t)((((b->barCount * 4u) +
                                  (uint32_t)(b->beatInBar - 1u)) % 32u) + 1u);
}

// The beat source: a deck first, the estimator second.
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

  // The FAKE FILLS setting: the board invents a fill for the sources that
  // have no idea where the song's fills are.  It is asked for by the user,
  // and the handoff below works out whether the bar we are in is one.
  b.fakeFill = (haveTf && tf.fakeFill) ? 1 : 0;

  // A DECK, if there is one.
  //
  // ignoreProLink is the DEBUG switch: it ignores the deck and answers from
  // the estimator instead, so the two tempo models can be heard on the same
  // wall.  It is the effect's "Ignore Pro Link" option, passed through.
  if (!ignoreProLink) {
    ArSourceBeat deck;
    if (arGetSourceBeat(&deck)) {
      arApplySourceBeat(&b, deck, haveTf, tf);
      return b;
    }
  }

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
    // THE USER'S MARK, with the estimator as the clock.
    //
    // With a deck, the mark is an OVERRIDE of the deck's numbering: the
    // publish layer rotates the deck's beat in the bar onto the beat the
    // user marked, and counts the bars from it.  With the estimator there
    // is no deck to rotate - the estimator has ALREADY rotated its own
    // numbering, so the beat that was sounding when the user marked it is
    // beat one in arTempoBeatInBar() - so the only thing left to do is to
    // say that the mark is there.
    //
    // Without this the mark was obeyed by the beat number and by the
    // estimator's own counters, but NOT by the bar number - and the bar
    // number is what the effects count on.  The mark used to be a rotation
    // of the numbering and nothing else; this makes it what it is with a
    // deck: the bar the user marked is bar zero, and the count runs from
    // there.
    if (tf.downbeatSet != 0) {
      b.markOn   = 1;
      b.markBeat = 0;      // the estimator's numbering is already the marked one
      b.markBack = 0;      // the marked beat is the one sounding now
      b.markGen  = tf.downbeatGen | AR_MARK_GEN_INTERNAL;
    }
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

void arResetTempo() {  ArPublishGuard guard;
  // The handoff correction goes too: after a reset there is no grid to
  // be out of step with.
  arHandoffCorr  = 0.0f;
  arHandoffAt    = 0;
  arHandoffSrc   = 0;
  arHandoffAcc   = 0.0f;
  arHandoffAccAt = 0;
  arHandoffBpm   = 0.0f;
  // The mark's rebase goes too: after a reset there is no grid for the
  // bar number to be counted on, and no mark to count from.
  arHandoffDeckAtMark = 0.0f;
  arHandoffMarkGen    = 0;
  arHandoffMarkOn     = 0;
  // The fake fill's roll goes with it: the next measure is a new one.
  arFakeFillMeasure   = 0xFFFFFFFFFFFFFFFFull;
  arFakeFillOn        = false;
  arTempoFrame.bpm = 0.0f;
  arTempoFrame.beatProgress = 0.0f;
  arTempoFrame.score = 0.0f;
  arTempoFrame.beatInBar = 0;
  arTempoFrame.valid = 0;
  arTempoFrame.beatInMeasure = 0;
  arTempoFrame.downbeatKnown = 0;
  arTempoFrame.downbeatSet = 0;
  arTempoFrame.downbeatGen = 0;
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

// ---------------------------------------------------------------------------
// The published song phrase.
//
// One struct, copied under the lock like the others: a reader cannot see
// half of it.  It is NOT a measurement - it is something a source KNOWS - so
// there is no "live" flag for it: it is there or it is not.
// ---------------------------------------------------------------------------
static ArSongPhrase arPhrase = {};
static bool arPhraseValid = false;

void arPublishSongPhrase(const ArSongPhrase& p) {
  ArPublishGuard guard;
  arPhrase = p;
  arPhraseValid = true;
}

bool arCopySongPhrase(ArSongPhrase* dest) {
  ArPublishGuard guard;
  if (!arPhraseValid) {
    dest->index = -1;
    dest->startBeat = 0;
    dest->beatCount = 0;
    dest->currentBeat = 0;
    dest->kind = AR_PHRASE_NONE;
    dest->fill = 0;
    dest->name[0] = '\0';
    dest->generation = 0;
    return false;
  }
  *dest = arPhrase;
  return true;
}

bool arSongPhraseUsable(void) {
  ArPublishGuard guard;
  return arPhraseValid;
}

void arResetSongPhrase(void) {
  ArPublishGuard guard;
  arPhrase = ArSongPhrase();
  arPhrase.index = -1;
  arPhraseValid = false;
}
