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

// The published tempo frame.
//
// It is ONE struct and not a double buffer, because the payload is twelve
// bytes and the copy is made under the lock: a reader cannot see half of it.
static ArTempoFrame arTempoFrame = {0.0f, 0.0f, 0.0f, 0, 0, 0, 0, 0.0f};
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
    dest->beatInBar = 0; dest->valid = 0;
    dest->beatInMeasure = 0; dest->downbeatKnown = 0; dest->downbeatT = 0.0f;
    dest->refBpm = 0.0f; dest->refErrMs = 0.0f;
    dest->refBeat = 0; dest->refOurBeat = 0;
    dest->refCount = 0; dest->refAgree = 0;
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

void arResetTempo() {
  ArPublishGuard guard;
  arTempoFrame.bpm = 0.0f;
  arTempoFrame.beatProgress = 0.0f;
  arTempoFrame.score = 0.0f;
  arTempoFrame.beatInBar = 0;
  arTempoFrame.valid = 0;
  arTempoFrame.beatInMeasure = 0;
  arTempoFrame.downbeatKnown = 0;
  arTempoFrame.downbeatT = 0.0f;
  arTempoFrame.refBpm = 0.0f;
  arTempoFrame.refErrMs = 0.0f;
  arTempoFrame.refBeat = 0;
  arTempoFrame.refOurBeat = 0;
  arTempoFrame.refCount = 0;
  arTempoFrame.refAgree = 0;
  arTempoValid = false;
}
