#pragma once
// ===========================================================================
// The MoonModules audio-sync format: one board's whole analysis to the others,
// one UDP datagram a frame, beside the WLED v2 packet.
//
// The format is shared by the MoonModules projects - MoonLight, this WLED, and
// proposed to WLED-MM and WLED - and its specification, with the bytes of an
// example packet, is:
//   https://moonmodules.org/MoonLight/reference/audio-sync.html
//
// The v2 packet stays exactly as it is, on 239.0.0.1, for every device that
// reads only it. This format goes to its own group, 239.0.0.2, on the same
// port, so a device that does not know it never receives it.
//
// A packet is a 5-byte header ("MM", the version, a 16-bit frame number) and
// then sections, in any order: a type byte, a 16-bit length, the bytes. A
// receiver skips a section type it does not know by its length, and reads the
// fields a section holds, so a shorter (older) section gives what it has and a
// longer (newer) one gives what this file knows. Every multi-byte field is
// little-endian. An absent section means "not measured", never zeros.
//
// This file holds only the bytes: building and parsing. Which of the board's
// values go in, and where a received frame goes, is the usermod's business.
// ===========================================================================

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// The multicast group and the header. The port is the usermod's audioSyncPort.
#define MM_SYNC_GROUP 239, 0, 0, 2
static constexpr uint8_t MM_SYNC_MAGIC_0 = 'M';
static constexpr uint8_t MM_SYNC_MAGIC_1 = 'M';
static constexpr uint8_t MM_SYNC_VERSION = 1;
static constexpr size_t  MM_SYNC_HEADER  = 5;   // magic (2), version (1), frame number (2)
static constexpr size_t  MM_SYNC_SECTION = 3;   // type (1), length (2)

// The section types. 3 (harmony) and 4 (notes) come from boards that track the
// keys; this board neither sends nor reads them, and skips them by their length.
enum MmSyncSectionType : uint8_t {
  MM_SECTION_CORE     = 0,
  MM_SECTION_SMOOTHED = 1,
  MM_SECTION_THIRDS   = 2,
  MM_SECTION_HARMONY  = 3,
  MM_SECTION_NOTES    = 4,
  MM_SECTION_BEAT     = 5
};

// Where a beat comes from. 0..3 are this WLED's own ArSourceId numbers, so its
// beat goes out as it is.
enum MmSyncBeatSource : uint8_t {
  MM_BEAT_TRACKER   = 0,   // the estimator: the beat heard in the sound
  MM_BEAT_PROLINK   = 1,   // a Pioneer deck, over Pro DJ Link
  MM_BEAT_STAGELINQ = 2,   // a Denon DJ unit, over StageLinq
  MM_BEAT_ABLETON   = 3,   // Ableton Live, over Link
  MM_BEAT_MIDICLOCK = 4,   // a MIDI clock
  MM_BEAT_TAP       = 5    // tap tempo
};

// The beat section's flags.
static constexpr uint8_t MM_BEAT_DOWNBEAT_KNOWN = 0x01;   // beatInBar is the source's measured or marked bar, not a count from the lock
static constexpr uint8_t MM_BEAT_FILL           = 0x02;   // the beat is in a fill: a deck's fill cue, or a faked one

// The largest datagram the format allows: the UDP payload every IPv4 host takes
// without fragmenting.
static constexpr size_t MM_SYNC_MAX_DATAGRAM = 508;

// Section sizes, as this version writes them.
static constexpr size_t MM_CORE_SIZE  = 27;
static constexpr size_t MM_BEAT_SIZE  = 11;
static constexpr size_t MM_MAX_THIRDS = 31;

// The core: the level, the gate, the drums, the peak and the 16 GEQ bands.
struct MmSyncCore {
  uint8_t  level;           // this frame's level, 0..255
  uint8_t  levelSmoothed;   // the level, smoothed
  uint8_t  gate;            // where the noise gate opens on the bands' scale; 0 when none is measured
  uint8_t  gateOpen;        // 1 while sound is over the gate
  uint8_t  onset;           // 0, or the strength 1..255 of a drum heard
  uint8_t  onsetLow;        // 0, or the strength of a hit in the bass (the kick)
  uint8_t  onsetMid;        // the same for the mids (the snare)
  uint8_t  onsetHigh;       // the same for the treble (the hats)
  uint16_t peakHz;          // the dominant frequency in Hz, 0 for none
  uint8_t  peakMag;         // its magnitude, 0..255
  uint8_t  bands[16];       // the 16 GEQ bands, 0..255, raw
};

// The beat one board decided and every board follows.
struct MmSyncBeat {
  uint16_t bpm100;          // the tempo x 100: 12800 is 128 BPM; a sender never sends 0
  uint16_t beatCount;       // the beats since the source locked, wrapping; a change is a beat
  uint8_t  phase;           // how far the current beat has run when the packet leaves, 0..255
  uint8_t  beatInBar;       // 1..4, 0 while the place in the bar is unknown
  uint8_t  source;          // an MmSyncBeatSource
  uint8_t  confidence;      // 0..255: the tracker's lock, 255 for a deck or a clock
  uint8_t  beatInPhrase;    // 1..phraseBeats, 0 while there is no phrase
  uint8_t  phraseBeats;     // the phrase's length in beats, 0 while there is none
  uint8_t  flags;           // MM_BEAT_DOWNBEAT_KNOWN, MM_BEAT_FILL
};

// One frame as this board sends and reads it.
struct MmSyncFrame {
  uint16_t   seq;                     // the sender's frame number
  bool       hasCore;
  MmSyncCore core;
  bool       hasSmoothed;
  uint8_t    bandsSmoothed[16];       // the sender's smoothed bands, so every board shows the same frame
  uint8_t    thirdsCount;             // 0 when no thirds came; at most MM_MAX_THIRDS
  uint8_t    thirds[MM_MAX_THIRDS];   // the third-octave bands from 20 Hz up (ISO 266 centers), 0..255
  uint8_t    thirdsHits[4];           // a bit per third, set on its hit; bit i%8 of byte i/8
  bool       hasBeat;
  MmSyncBeat beat;
};

// ---- building --------------------------------------------------------------

static inline uint8_t* mmSyncPut16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return p + 2; }

static inline uint8_t* mmSyncSectionStart(uint8_t* p, MmSyncSectionType type, size_t len) {
  p[0] = (uint8_t)type;
  return mmSyncPut16(p + 1, (uint16_t)len);
}

// The largest packet build() writes: the header, the core, the smoothed bands,
// 31 thirds and the beat.
static constexpr size_t MM_SYNC_MAX_BUILT = MM_SYNC_HEADER + 4 * MM_SYNC_SECTION + MM_CORE_SIZE + 16 + (1 + MM_MAX_THIRDS + 4) + MM_BEAT_SIZE;
static_assert(MM_SYNC_MAX_BUILT <= MM_SYNC_MAX_DATAGRAM, "a full packet fits the datagram the format allows");

// Build a packet from `f` into `out`, at least MM_SYNC_MAX_BUILT bytes, and
// return its length. Only the sections the frame has go in.
static inline size_t mmSyncBuild(uint8_t* out, const MmSyncFrame& f) {
  out[0] = MM_SYNC_MAGIC_0;
  out[1] = MM_SYNC_MAGIC_1;
  out[2] = MM_SYNC_VERSION;
  uint8_t* p = mmSyncPut16(out + 3, f.seq);
  if (f.hasCore) {
    const MmSyncCore& c = f.core;
    p = mmSyncSectionStart(p, MM_SECTION_CORE, MM_CORE_SIZE);
    *p++ = c.level; *p++ = c.levelSmoothed; *p++ = c.gate; *p++ = c.gateOpen;
    *p++ = c.onset; *p++ = c.onsetLow; *p++ = c.onsetMid; *p++ = c.onsetHigh;
    p = mmSyncPut16(p, c.peakHz);
    *p++ = c.peakMag;
    memcpy(p, c.bands, 16); p += 16;
  }
  if (f.hasSmoothed) {
    p = mmSyncSectionStart(p, MM_SECTION_SMOOTHED, 16);
    memcpy(p, f.bandsSmoothed, 16); p += 16;
  }
  const uint8_t thirds = f.thirdsCount > MM_MAX_THIRDS ? (uint8_t)MM_MAX_THIRDS : f.thirdsCount;
  if (thirds > 0) {
    p = mmSyncSectionStart(p, MM_SECTION_THIRDS, 1 + thirds + 4);
    *p++ = thirds;
    memcpy(p, f.thirds, thirds); p += thirds;
    memcpy(p, f.thirdsHits, 4);  p += 4;
  }
  if (f.hasBeat && f.beat.bpm100 > 0) {
    const MmSyncBeat& b = f.beat;
    p = mmSyncSectionStart(p, MM_SECTION_BEAT, MM_BEAT_SIZE);
    p = mmSyncPut16(p, b.bpm100);
    p = mmSyncPut16(p, b.beatCount);
    *p++ = b.phase; *p++ = b.beatInBar; *p++ = b.source; *p++ = b.confidence;
    *p++ = b.beatInPhrase; *p++ = b.phraseBeats; *p++ = b.flags;
  }
  return (size_t)(p - out);
}

// ---- parsing ---------------------------------------------------------------

// Is this datagram the MoonModules format? The first two bytes say so.
static inline bool mmSyncIsPacket(const uint8_t* pkt, size_t len) {
  return pkt && len >= 2 && pkt[0] == MM_SYNC_MAGIC_0 && pkt[1] == MM_SYNC_MAGIC_1;
}

// One section's bytes, read a field at a time: a field past the section's end
// reads as 0, so a shorter section gives the fields it holds.
struct MmSyncReader {
  const uint8_t* s;
  size_t n;
  size_t at;
  uint8_t  byte()  { return at < n ? s[at++] : 0; }
  uint16_t u16()   { if (at + 2 > n) { at = n; return 0; } uint16_t v = (uint16_t)(s[at] | (s[at + 1] << 8)); at += 2; return v; }
  bool     bytes(uint8_t* dst, size_t k) { if (at + k > n) { at = n; return false; } memcpy(dst, s + at, k); at += k; return true; }
};

// Parse a datagram into `f`. Returns false for another magic or version, a
// section running past the datagram, or a count past its array; a section type
// this file does not know is skipped. No section is required.
static inline bool mmSyncParse(const uint8_t* pkt, size_t len, MmSyncFrame& f) {
  memset(&f, 0, sizeof(f));
  if (!mmSyncIsPacket(pkt, len) || len < MM_SYNC_HEADER || pkt[2] != MM_SYNC_VERSION) return false;
  f.seq = (uint16_t)(pkt[3] | (pkt[4] << 8));
  for (size_t at = MM_SYNC_HEADER; at < len;) {
    if (at + MM_SYNC_SECTION > len) return false;
    const uint8_t type = pkt[at];
    const size_t  n    = (size_t)(pkt[at + 1] | (pkt[at + 2] << 8));
    at += MM_SYNC_SECTION;
    if (at + n > len) return false;
    MmSyncReader r = { pkt + at, n, 0 };
    switch (type) {
      case MM_SECTION_CORE: {
        MmSyncCore& c = f.core;
        c.level = r.byte(); c.levelSmoothed = r.byte(); c.gate = r.byte(); c.gateOpen = r.byte();
        c.onset = r.byte(); c.onsetLow = r.byte(); c.onsetMid = r.byte(); c.onsetHigh = r.byte();
        c.peakHz = r.u16(); c.peakMag = r.byte();
        r.bytes(c.bands, 16);
        f.hasCore = true;
        break;
      }
      case MM_SECTION_SMOOTHED:
        f.hasSmoothed = r.bytes(f.bandsSmoothed, 16);
        break;
      case MM_SECTION_THIRDS: {
        const uint8_t count = r.byte();
        if (count > MM_MAX_THIRDS) return false;
        if (r.bytes(f.thirds, count)) f.thirdsCount = count;
        r.bytes(f.thirdsHits, 4);
        break;
      }
      case MM_SECTION_BEAT: {
        MmSyncBeat& b = f.beat;
        b.bpm100 = r.u16(); b.beatCount = r.u16();
        b.phase = r.byte(); b.beatInBar = r.byte(); b.source = r.byte(); b.confidence = r.byte();
        b.beatInPhrase = r.byte(); b.phraseBeats = r.byte(); b.flags = r.byte();   // an 8-byte section gives no phrase and no flags
        f.hasBeat = b.bpm100 > 0;
        break;
      }
      default:
        break;   // a type this file does not know, or the keys a key-tracking board sends
    }
    at += n;
  }
  return true;
}

// Should a frame with sequence number `seq` be taken, after `last`? A repeated
// one, or one fewer than 64 behind, is late or doubled and is dropped; a jump
// further back is a sender that restarted, and is taken.
static inline bool mmSyncSeqNew(uint16_t seq, uint16_t last, bool haveLast) {
  if (!haveLast) return true;
  const uint16_t behind = (uint16_t)(last - seq);
  return !(behind < 64);   // behind == 0 is a repeat
}
