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
