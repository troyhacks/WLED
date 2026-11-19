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

// The ABSOLUTE band levels.
//
// The published RTA BANDS above are a BYTE: a bar HEIGHT, drawn in a window
// that moves with the signal (the auto-range). A meter that draws its OWN dB
// axis needs the LEVEL, not the height - the height has already been windowed
// into the display's scale, so an axis drawn over it would be a lie.
//
// These are the levels the axis is for: the calibrated dB of each band, on
// the analyser's own scale, in the same unit as the reference offset.
//
// They come with the WINDOW they are read against, because the window moves:
// the auto-range holds the loudest band at its top, so the axis a meter draws
// against them moves too. The two are published together, so a reader cannot
// get the levels from one frame and the window from another.
struct ArRtaScale {
  float    topDb;     // the window's top edge, in the same scale as the levels
  float    rangeDb;   // the window's span, dB
  uint8_t  valid;     // 1 when there is a window to draw against
};

void arPublishRtaDb(const float* db, const ArRtaScale& scale);

// Copy the published band levels into dest (NUM_RTA_CHANNELS floats), and the
// window they are read against into scale. A reader that sets its own window
// passes nullptr for scale and does not get one.
// Returns whether they may be USED - false if nothing has been published yet,
// or if the frame is not a live measurement (see arSetRtaLive above). When
// it returns false, dest is filled with -120 dB, which is silence, and the
// scale is filled with a window that is not there.
bool arCopyRtaDb(float* dest, ArRtaScale* scale);

// The loudness frame: the programme loudness and its family.
//
// These are the numbers a hardware analyser's programme meter shows, measured
// to ITU-R BS.1770-4 on this board's own input. Like the tempo, they are a
// LOCAL measurement, so they carry the same live flag: a board that is
// receiving the sound over the network has no loudness of its own.
struct ArLoudnessFrame {
  float    programme;      // the gated mean over the programme, LUFS
  float    momentary;      // the last 400 ms, LUFS
  float    momentaryMax;   // the highest 400 ms seen, LUFS
  float    shortTerm;      // the last 3 s, LUFS
  float    shortTermMax;   // the highest 3 s seen, LUFS
  float    sliding10;      // the rolling ten seconds, LUFS
  float    lra;            // the spread of the short-term values, LU
  float    truePeak;       // the inter-sample peak, dBTP (held)
  float    truePeakNow;    // the inter-sample peak, dBTP (this frame)
  uint8_t  valid;          // 1 when there is a programme to talk about
};

// Publish one whole loudness frame.
void arPublishLoudness(const ArLoudnessFrame& l);

// Copy the published loudness frame into dest. Returns whether the frame may
// be USED - false if nothing has been published yet, or if the frame is not a
// live measurement (see arSetLoudnessLive below). When it returns false,
// dest is filled with the "nothing measured" values.
bool arCopyLoudness(ArLoudnessFrame* dest);

// Is the published loudness frame a LIVE measurement?
//
// The same rule as the RTA bands and the tempo: while the sound is the
// NETWORK's, there is no local analysis and the frame is stale.
void arSetLoudnessLive(bool live);

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
  float    quality;         // the alignment x the coverage: how well the music explains the RUNNING grid
  float    qualityPeak;     // the quality's recent best: the yardstick the relative gate uses
  float    driftMs;         // the grid's phase error against the music, ms (signed)
  float    driftBpm;        // the estimate minus the clock, BPM (signed)
  uint8_t  hold;            // 1 when the USER has frozen the clock
  uint8_t  beatInBar;       // 1..4; 1 is the downbeat
  uint8_t  valid;           // 1 when the estimate is a real measurement
  uint8_t  beatInMeasure;   // 1..16; 1 is the measure's own beat
  uint8_t  beatInMeasure32; // 1..32; 1 is the FIRST beat of the thirty-two
  uint8_t  downbeatKnown;   // 1 when the downbeat is a MEASUREMENT, not a convention
  uint8_t  downbeatSet;     // 1 when the USER marked the downbeat by hand
  uint8_t  songPhrase;      // 1 when the downbeat comes from the SONG's phrase
  // The ProLink OVERRIDE: the user marked a downbeat while the deck was
  // playing, so the deck's numbering is rotated to the beat they marked.
  // It is an OFFSET from the deck's packets - the deck's tempo and phase
  // are still the deck's own - and the bar number and the sixteen-beat
  // phrase are counted FROM THE MARK (see ArBeatSource::barCount).
  uint8_t  deckOverride;    // 1 when the override is on
  uint8_t  deckDownbeat;    // the DECK's beat that was marked, 0..3
  uint32_t deckElapsedAtMark; // the deck's beat count when it was marked
  float    downbeatT;       // the bar profile's t-score: how sure the downbeat is
  uint32_t beatCount;       // beats counted since the lock (the monotonic counter)
  // The ProLink reference: the deck's grid, measured against ours.  All zero
  // when no deck is connected.
  float    refBpm;          // the deck's rate
  float    refErrMs;        // our beat minus the deck's beat, ms
  uint8_t  refBeat;         // the deck's beat in the bar, 1..4
  uint8_t  refOurBeat;      // our beat in the bar at the same moment
  uint8_t  refValid;        // 1 when refErrMs is a real measurement
  uint16_t refCount;        // how many beats have been compared
  uint16_t refLocked;       // ... and how many advanced together
  uint16_t refSlips;        // ... and how many times one of them skipped
  uint8_t  refOffset;       // the deck's bar position minus ours, 0..3
};

// Publish one whole tempo frame.
void arPublishTempo(const ArTempoFrame& t);

// The BEAT SOURCE: the board's one answer to "what is the beat?"
//
// TWO sources, and the order is the design: ProLink first if it is there,
// the estimator second, and NEITHER when neither is live.
//
// The ProLink usermod gets the tempo from a Pioneer deck's network packets:
// it is a master clock, and it is exact. The AudioReactive estimator gets
// it from the sound: it is a measurement, and it is only as good as the
// music. So when a deck is connected the deck wins - and when it is not,
// the estimate is better than nothing.
//
// What is NOT allowed is a fabricated rate. A board that is receiving its
// sound over the network has no audio to measure, so it has no tempo, and
// the answer is "not live" rather than a stale number.
//
// This is the ONE arbitration, and everybody reads it: the effect that
// strobes to the beat (mode_bpm_strobe in wled00/FX.cpp) and the web GUI's
// tempo panel both call this, so the wall and the screen cannot disagree
// about where the beat is.
struct ArBeatSource {
  float    bpm;             // beats per minute; 0 when there is no beat
  uint8_t  beatInBar;       // 1..4; 1 is the downbeat; 0 when unknown
  float    beatProgress;    // 0..1 through the current beat
  uint8_t  live;            // 1 when there is a beat to show
  uint8_t  downbeatKnown;   // the downbeat is KNOWN - the deck's, or the user's
  uint8_t  fromProLink;     // 1 when the DECK is the source, 0 for the estimator
  uint8_t  beatInMeasure32; // 1..32; 1 is the FIRST beat of the thirty-two
  uint8_t  locked;          // 1 when the estimator's clock is frozen
  uint8_t  downbeatSet;     // 1 when the USER marked the downbeat by hand
  uint8_t  deckOverride;    // 1 when the user's mark overrides the DECK's numbering
  // The MARK: the beat that is beat one.
  //
  // It is the user's own mark, or the song's phrase - both of them say
  // where the bar starts, and the phrase wins when the user has asked for
  // it.  The phrase's start is in the PAST, so the mark carries how far
  // back it is.
  uint8_t  markOn;          // 1 when a mark is in force
  uint8_t  markBeat;        // the deck's beat (0..3) that is beat one
  uint32_t markBack;        // the whole beats from now back to the marked beat
  uint32_t markAt;          // the source's count at the marked beat
  uint32_t markGen;         // bumped when the mark changes
  // The PHRASE: the song's structure, when there is one.
  uint8_t  phraseOn;        // 1 when there is a phrase
  uint8_t  phraseFill;      // 1 when the phrase is a FILL
  uint8_t  phraseKind;      // the phrase's TYPE - an ArPhraseKind
  int32_t  phraseIndex;     // the phrase's number in the track
  uint32_t phraseIn;        // the beats into the phrase
  uint8_t  songPhrase;      // 1 when the downbeat comes from the SONG's phrase
  // The BAR NUMBER - on the same grid as the phase below.
  //
  // It is the bar the music is in, counted on the grid the phase runs on.
  // The master's COUNT cannot answer this: with a deck driving, that count
  // is the deck's position in the song, and it rides the status stream,
  // which is not the beat rate - so its bar boundary can be a beat away
  // from the phase's.  A light that turns round when the COUNT says the
  // bar turned round reverses in the middle of the bar.
  //
  // WITH THE USER'S PROLINK MARK ON it is counted FROM THE MARK: the bar
  // the user marked is bar zero, and the count runs from there.  So the
  // mark restarts the effect's cycle, its sweep direction and its
  // sixteen-beat phrase, and not only the beat number.
  uint32_t barCount;
  // The beat COUNTER - the master's, for the source that is driving.
  //
  // It RESTARTS when the user marks the downbeat, so it realigns with the
  // mark instead of carrying on from wherever it had got to.
  //
  // It is NOT what the effects count on.  With a deck driving it is the
  // deck's own count, and that rides the status stream, which is not the
  // beat rate - so it jumps two or three beats at a time.  The BAR NUMBER
  // above is the counter the effects use: it is on the grid's own phase,
  // so it is exact.
  uint32_t beatCount;
  float    driftMs;         // the grid's phase error against the music, ms
  float    driftPct;        // that error as a percentage of the beat
  // The bar's phase in BEATS - 0 at the downbeat, 4 at the top of the bar -
  // and CONTINUOUS.  It is the source's phase with the handoff correction
  // on it, so it does not jump when the grid changes hands.
  //
  // It is a separate number from the beat number on purpose: the beat
  // NUMBER may change when the deck takes over (the deck's numbering is
  // the track's real one, and the estimator's is only a convention) and
  // that is information, not a glitch.  The POSITION of the light is not
  // information - it is where the music is - so it glides.
  float    phaseBeats;
};

// The beat, from whichever source is answering.
//
// ignoreProLink is the DEBUG switch: it ignores the deck and answers from
// the estimator instead, so the two tempo models can be heard on the same
// wall. It is the effect's "Ignore Pro Link" option, passed through.
ArBeatSource arGetBeatSource(bool ignoreProLink = false);

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

// ---------------------------------------------------------------------------
// The SONG PHRASE.
//
// Where the music is, structurally: the intro, the verse, the chorus, the
// fill.  A source that knows - a DJ deck, a lighting desk - publishes the
// phrase the music is in, and the board's clock uses it.
//
// It is in the CORE on purpose.  The source that provides it may change - a
// deck today, something else tomorrow - and everything that reads the phrase
// reads it from here, so nothing downstream has to know where it came from.
// ---------------------------------------------------------------------------

// The phrase's TYPE.
//
// The source maps its own vocabulary onto these: the Pioneer's kinds are
// relative to the mood, so its "kind 5" is a chorus in a high-mood track and
// a verse in a low-mood one.
enum ArPhraseKind : uint8_t {
  AR_PHRASE_NONE     = 0,
  AR_PHRASE_UNKNOWN  = 1,
  AR_PHRASE_INTRO    = 2,
  AR_PHRASE_VERSE    = 3,
  AR_PHRASE_CHORUS   = 4,
  AR_PHRASE_BRIDGE   = 5,
  AR_PHRASE_UP       = 6,
  AR_PHRASE_DOWN     = 7,
  AR_PHRASE_OUTRO    = 8
};

struct ArSongPhrase {
  int32_t  index;        // the phrase's number in the track; -1 = there is none
  uint32_t startBeat;    // the beat it starts on, in the SOURCE's beat numbering (1-based)
  uint32_t beatCount;    // how many beats it lasts
  uint32_t currentBeat;  // the source's CURRENT beat - the same numbering
  uint8_t  kind;         // the phrase's TYPE - an ArPhraseKind
  uint8_t  fill;         // 1 when it is a FILL - a flag on top of the kind
  char     name[24];     // the label, for a reader that wants to show it
  uint32_t generation;   // bumped on every change, so a reader can see it
};

// Publish the phrase the music is in.
//
// The source calls it when the phrase changes.  The board's clock reads it
// and uses the phrase's START as the downbeat, so the bar the music is in
// starts there.
void arPublishSongPhrase(const ArSongPhrase& p);

// Copy the published phrase into dest.  Returns whether there is one.
bool arCopySongPhrase(ArSongPhrase* dest);

// May the published phrase be used?  The same answer arCopySongPhrase
// gives, without the copy.
bool arSongPhraseUsable(void);

// Forget the published phrase.
void arResetSongPhrase(void);

#endif // WLED_AR_PUBLISH_H
