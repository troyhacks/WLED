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

// The tempo frame: the beat rate, the beat's place in the bar, the progress
// through the current beat and the score.
//
// It is published the same way as the RTA bands - under the ONE lock - so a
// reader never sees a BPM from one frame and a beat index from another. A
// strobe that flashes on the beat needs both to be from the same instant.
//
// The AudioReactive usermod writes it, from its own analysis of its own
// input. The effect that strobes to the beat reads it.
struct ArTempoFrame {
  float    bpm;             // beats per minute; 0 when there is no estimate
  float    beatProgress;    // 0..1 through the current beat
  float    score;           // the comb score, 0..1: how well the onsets fit the grid
  uint8_t  beatInBar;       // 1..4; 1 is the downbeat
  uint8_t  valid;           // 1 when the estimate is a real measurement
  uint8_t  beatInMeasure;   // 1..16; 1 is the measure's own beat
  uint8_t  downbeatKnown;   // 1 when the downbeat is a MEASUREMENT, not a convention
  float    downbeatT;       // the bar profile's t-score: how sure the downbeat is
  // The ProLink reference: the deck's grid, measured against ours.  All zero
  // when no deck is connected.
  float    refBpm;          // the deck's rate
  float    refErrMs;        // our beat minus the deck's beat, ms
  uint8_t  refBeat;         // the deck's beat in the bar, 1..4
  uint8_t  refOurBeat;      // our beat in the bar at the same moment
  uint16_t refCount;        // how many beats have been compared
  uint16_t refAgree;        // ... and how many agreed on the bar
};

// Publish one whole tempo frame.
void arPublishTempo(const ArTempoFrame& t);

// Copy the published tempo frame into dest. Returns whether the frame may be
// USED - false if nothing has been published yet, or if the frame is not a
// live measurement (see arSetTempoLive below). When it returns false, dest
// is zeroed.
bool arCopyTempo(ArTempoFrame* dest);

// May the published tempo frame be used? The same answer arCopyTempo gives,
// without the copy.
bool arTempoUsable(void);

// Is the published tempo frame a LIVE measurement?
//
// The tempo is a LOCAL measurement: it comes from this board's own analysis
// of its own input. A board that is RECEIVING the sound over the network
// runs no local analysis, so it has no tempo at all - and the frame that was
// published before the network took over is stale. The same holds for a
// realtime input (E131, ArtNet).
//
// The AudioReactive usermod sets this; it is the one that knows whether this
// board is measuring its own input.
void arSetTempoLive(bool live);

// The same, for a caller that ALREADY holds the publish lock. Taking the
// lock again would deadlock.
void arCopyTempoLocked(ArTempoFrame* dest);

// Forget the published tempo frame.
void arResetTempo();

#endif // WLED_AR_PUBLISH_H
