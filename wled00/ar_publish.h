#ifndef WLED_AR_PUBLISH_H
#define WLED_AR_PUBLISH_H

#include <stdint.h>

/*
 * The AudioReactive publish path.
 *
 * Everything that publishes an analysed frame goes through ONE lock and ONE
 * published frame.
 *
 * Before this module existed the RTA frame had two of each. The usermod kept
 * a private copy for its own snapshot, and a second, board-local copy was
 * written straight into by the sound simulator - with no lock at all - and the
 * effect that draws the analyser read that copy while it was being written.
 * Two writers, one unguarded, one reader with no idea either.
 *
 * The lock now lives here, where every writer and every reader can reach it,
 * and the simulator publishes through the same door the usermod does.
 */

// The number of RTA (third-octave analyser) bands published for the effects
// that draw an analyser: the ISO 266 third-octave set, 19.95 Hz - 19.95 kHz,
// 31 bands. These are the INTEGRATED power over each band - the unit a
// hardware analyser's bar shows - and they are a different measurement from
// the 16 display bands, which are point PSDs.
#ifndef NUM_RTA_CHANNELS
#define NUM_RTA_CHANNELS 31
#endif

// The publish critical section.
//
// The section is a 16-byte copy and a pointer store - a few hundred
// nanoseconds - so a spin lock is the right tool: neither task can be blocked
// by the other for longer than that.
//
// It is NOT recursive. Taking it twice from the same task deadlocks, so
// anything that already holds it uses the *Locked variants below.
class ArPublishGuard {
public:
  ArPublishGuard();
  ~ArPublishGuard();
  ArPublishGuard(const ArPublishGuard&) = delete;             // not copyable
  ArPublishGuard& operator=(const ArPublishGuard&) = delete;
};

// Publish one whole frame of RTA bars (NUM_RTA_CHANNELS bytes).
//
// The frame is copied into the buffer that is NOT being read and the index is
// swapped, so a reader never sees half of a frame.
void arPublishRtaBands(const uint8_t* bands);

// Copy the published frame of RTA bars into dest (NUM_RTA_CHANNELS bytes).
// Returns whether the frame may be USED - false if nothing has been published
// yet, or if the frame is not a live measurement (see arSetRtaLive below).
// When it returns false, dest is filled with zeros.
bool arCopyRtaBands(uint8_t* dest);

// May the published frame of RTA bars be used? The same answer arCopyRtaBands
// gives, without the copy.
bool arRtaUsable(void);

// Is the published frame of RTA bars a LIVE measurement?
//
// The thirty-one third-octave bands are a LOCAL measurement: they come from
// this board's own analysis of its own input. The UDP sound sync packets
// carry the SIXTEEN display bands and nothing else, so a board that is
// receiving has no 31-band measurement at all - and the frame that was
// published before the network took over is stale. The same holds for a
// realtime input (E131, ArtNet): the local analysis is suspended, so the
// frame is not being refreshed.
//
// While the frame is not live, it may not be used, and the effect that draws
// an analyser stays on the sixteen display bands - the ones the packets DO
// carry - instead of drawing a stale (or empty) thirty-one band view.
//
// The AudioReactive usermod sets this; it is the one that knows whether
// this board is measuring its own input.
void arSetRtaLive(bool live);

// The same, for a caller that ALREADY holds the publish lock. Taking the lock
// again would deadlock.
void arCopyRtaBandsLocked(uint8_t* dest);

// Forget the published frame: the buffers are cleared and the next reader is
// told there is nothing to read.
void arResetRtaBands();

#endif // WLED_AR_PUBLISH_H
