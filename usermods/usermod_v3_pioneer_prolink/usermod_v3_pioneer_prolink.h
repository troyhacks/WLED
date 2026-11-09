#pragma once

/*
   @title     Pro DJ Link v3 (With Metadata, Waveform & Artwork)
   @file      usermod_v3_pioneer_prolink.h
   @brief     Syncs WLED to Pioneer CDJs including Phrase/Mood analysis,
              track metadata, preview waveform, and album artwork.
              v3 successor to usermod_v2_pioneer_prolink.h, using the
              v3 event bus hooks (onPreStateChange, onEvent). The
              original v2 Pioneer is untouched.
   @target    ESP32 (Requires AsyncUDP, PSRAM recommended for waveform/artwork)
   @repo      WLED MoonModules

   Build flag: -D USERMOD_PIONEER_PROLINK_V3  (gated in usermods_list.cpp)
   The v2 and v3 usermods share the same file-scope prolink_*_public
   symbols that FX.cpp reads directly. Defining BOTH flags will cause
   duplicate-symbol link errors. Choose one.
*/

#include "wled.h"
#include <AsyncUDP.h>
#include <WiFiClient.h>
#include <vector>
#include "fcn_declare.h"
#include "esp_heap_caps.h"

// --- Protocol Constants ---
static constexpr uint16_t PORT_ANNOUNCE = 50000;
static constexpr uint16_t PORT_BEAT = 50001;
static constexpr uint16_t PORT_STATUS = 50002;
static constexpr uint16_t PORT_DBSERVER = 12523;

static constexpr uint8_t TYPE_ANNOUNCE = 0x06;
static constexpr uint8_t TYPE_STATUS = 0x0A;
static constexpr uint8_t TYPE_BEAT = 0x28;

static constexpr const char* WLED_DEVICE_NAME = "WLED-Lighting";
static constexpr uint8_t WLED_DEVICE_ID_DEFAULT = 0x03;

static constexpr uint32_t PITCH_CENTER = 0x10000000;
static constexpr float PITCH_SCALE = 2684352.0f;
static constexpr float SPEED_UNITY = 1048576.0f;

// --- Timing Constants ---
static constexpr uint32_t FETCH_TIMEOUT_MS = 5000;
static constexpr uint32_t FETCH_RETRY_DELAY_MS = 5000;
static constexpr uint8_t  FETCH_MAX_RETRIES = 3;
static constexpr uint32_t PEER_TIMEOUT_MS = 5000;
static constexpr uint32_t KEEPALIVE_INTERVAL_MS = 1500;
static constexpr uint32_t PEER_CHECK_INTERVAL_MS = 2000;
static constexpr uint32_t BEAT_FLASH_DURATION_MS = 300;
static constexpr uint32_t STARTUP_DELAY_US = 10 * 1000000;  // 10 seconds in microseconds

// The phase offset: how far the deck's grid is moved to land on the beat the
// board HEARS rather than on the beat the deck ANNOUNCES.
//
// MEASURED live (2026-11-03, a CDJ at ~118 BPM): the board's own
// audio-derived beat fires ~40 ms BEFORE the deck's ProLink beat packet
// reaches it.  The deck's grid, used exactly as it arrives, therefore
// flashes ~40 ms AFTER the music - late, by more than a frame at 30 fps.
//
// The SIGN is the whole content of this constant.  POSITIVE means the deck's
// grid is moved BACK in time, so the flash lands on the audible beat.
//
// It is a default, not a law: it depends on the deck's output latency, the
// network and the audio path, and it is live-tunable (Phase_Offset_ms).
static constexpr int32_t PROLINK_PHASE_OFFSET_DEFAULT_MS = 40;

// The clock word's layout - see the member's comment.
static constexpr uint32_t PROLINK_CLOCK_POS_SHIFT = 30;
static constexpr uint32_t PROLINK_CLOCK_ANC_MASK  = 0x3FFFFFFFu;
static constexpr uint32_t PROLINK_CLOCK_STALE_MS  = 0x20000000u;  // 6.2 days

// How old the clock's anchor may get before the grid is declared dead.
//
// The anchor is re-stamped by every beat packet, so its age grows by one
// beat between packets.  Four beats is three dropped packets of slack;
// past that the deck is not beating, and a grid that keeps running off an
// old anchor is worse than no grid at all - it beats at a tempo nobody is
// playing, and paints a downbeat that is a bar out of place.
static constexpr uint8_t PROLINK_ANCHOR_MAX_BEATS = 4;

static constexpr uint32_t WAVEFORM_COLLECT_MS = 1500;
static constexpr uint32_t ARTWORK_COLLECT_MS = 5000;
static constexpr uint32_t METADATA_COLLECT_MS = 500;

// --- Buffer Sizes ---
static constexpr size_t METADATA_BUFFER_MAX = 8192;
static constexpr size_t WAVEFORM_BUFFER_MAX = 4096;
static constexpr size_t ARTWORK_BUFFER_MAX = (512 * 1024);

// --- Waveform Data Structure ---
struct WaveformPoint {
  uint8_t height;  // 0-127 amplitude
  uint8_t color;   // Pioneer color index 0-7
  uint8_t r;       // High frequency (0-127)
  uint8_t g;       // Mid frequency (0-127)
  uint8_t b;       // Low frequency (0-127)
};

// --- Public Variables (Exposed to FX.cpp) ---

// Timing & Sync
volatile float    prolink_bpm_public = 0.0f;
volatile uint8_t  prolink_beat_public = 0;
volatile uint32_t prolink_beat_number_public = 0;
volatile float    prolink_beat_progress_public = 0.0f;
volatile uint32_t prolink_track_id_public = 0;
volatile uint32_t prolink_track_duration_ms = 0;

// The deck's beat clock, and the counters that say where it came from.
//
// The deck sends TWO streams and they are independent:
//   * the BEAT packet (0x28) carries the rate and TICKS the beat - it
//     arrives on the beat, so it is the anchor the clock wants;
//   * the STATUS packet (0x0A) carries the beat-in-measure (the position
//     in the bar) and arrives every ~200 ms.
//
// Only ONE of them has to arrive for the clock to run, and the old code
// anchored on the STATUS one - so on a link where the status packets did
// not arrive, the rate stayed at zero even though the beat packets were
// coming in, and every consumer (the strobe effect, the AudioReactive
// deck reference) saw "no deck".  The counters below are published so the
// first question when the clock is not running - "which stream is
// missing?" - has an answer without a serial console.
volatile uint32_t prolink_beat_packets_public = 0;    // beat packets parsed
volatile uint32_t prolink_status_packets_public = 0;  // status packets parsed
volatile uint32_t prolink_beat_elapsed_ms_public = 0; // ms into the current AUDIBLE beat

// The phase offset in force, in ms: how far the deck's grid was moved to
// land on the audible beat.  Published so a consumer - and the info page -
// can see what the deck's grid was shifted by, and so the shift can be
// reasoned about from outside the usermod.
volatile int32_t prolink_phase_offset_ms_public = 0;

// How often the usermod's loop runs, in Hz.  The deck's grid is PUBLISHED
// from that loop, so this is also how often the published phase and bar
// position are refreshed - and therefore how old a consumer's reading of
// them can be.  It is the number that says whether a consumer can trust a
// reading taken at a beat, so it is published rather than guessed at.
volatile uint16_t prolink_loop_rate_hz_public = 0;

// Pitch slider
volatile float prolink_pitchPercent = 0.0f;

// Bar/Beat Counters
volatile uint32_t prolink_beats_elapsed_public = 0;
volatile uint8_t  prolink_bars_elapsed_public = 0;
volatile uint8_t  prolink_bars_remaining_public = 0;

// Phrase / Structure
volatile int      prolink_phrase_index_public = -1;
String            prolink_phrase_name_public = "";
volatile uint16_t prolink_phrase_beats_public = 0;
volatile float    prolink_phrase_progress_public = 0.0f;
String            prolink_mood_public = "";
volatile uint16_t prolink_total_phrases_public = 0;

// Connection state
volatile bool prolink_connected_public = false;

// Beat flash state
volatile bool    prolink_beat_flash_active = false;
volatile uint8_t prolink_beat_flash_brightness = 0;

// Track Metadata
String prolink_track_title = "";
String prolink_track_artist = "";
String prolink_track_album = "";
String prolink_track_key = "";
String prolink_track_genre = "";
String prolink_track_label = "";
volatile bool prolink_metadata_valid = false;

// Preview Waveform (PSRAM allocated)
WaveformPoint* prolink_waveform_data = nullptr;
volatile uint16_t prolink_waveform_length = 0;
volatile bool     prolink_waveform_valid = false;

// Album Artwork (PSRAM allocated, raw JPEG)
uint8_t* prolink_artwork_data = nullptr;
volatile uint32_t prolink_artwork_size = 0;
volatile bool     prolink_artwork_valid = false;

// Track progress
volatile float    prolink_track_progress = 0.0f;
volatile uint32_t prolink_total_beats = 0;

// // Preset offset for phrase-based preset selection
// int prolink_presetOffset = 0;

bool altWaveformColors = false;

// --- Internal Data Structures ---

struct PhraseEntry {
  uint16_t index;
  uint32_t startBeat;
  uint32_t count;
  uint16_t kind;
  bool isFill;
  String label;
};

struct ProLinkState {
  volatile bool     isConnected = false;
  volatile bool     isMaster = false;
  volatile uint8_t  activePlayerID = 0;
  volatile float    bpm = 0.0f;
  volatile float    effectivePitch = 0.0f;
  volatile uint32_t beatNumber = 0;
  volatile uint8_t  beatInMeasure = 0;
  volatile uint32_t beatsElapsed = 0;
  volatile uint8_t  halfBarsRemaining = 0;
  volatile uint8_t  halfBarsElapsed = 0;
  volatile float    pitchPercent = 0.0f;
  volatile uint32_t currentTrackId = 0;
  volatile uint8_t  currentSlot = 0;
  IPAddress         activePlayerIP;

  // Peer tracking
  volatile uint64_t peerMap = 0;
  IPAddress         peerIPs[64];
  unsigned long     peerLastSeen[64] = { 0 };
};

enum FetchState {
  IDLE,
  CONNECT_STAGE1, WAIT_STAGE1,
  CONNECT_STAGE2, HANDSHAKE, WAIT_HANDSHAKE,
  REQUEST_PHRASES, WAIT_PHRASES,
  REQUEST_METADATA_SETUP, WAIT_METADATA_SETUP,
  REQUEST_METADATA_RENDER, WAIT_METADATA_RENDER, COLLECT_METADATA,
  REQUEST_WAVEFORM, WAIT_WAVEFORM, COLLECT_WAVEFORM,
  REQUEST_ARTWORK, WAIT_ARTWORK, COLLECT_ARTWORK,
  COOLDOWN, RETRY_WAIT
};

class ProLinkUsermodV3 : public Usermod {

private:
  AsyncUDP udpStatus;
  AsyncUDP udpBeat;
  AsyncUDP udpAnnounce;
  WiFiClient dbClient;

  ProLinkState linkState;

  // Phrase Storage
  std::vector<PhraseEntry> phrases;
  uint32_t lastAnalyzedTrackId = 0;

  // State Machine
  FetchState fetchState = IDLE;
  unsigned long stateTimer = 0;
  unsigned long collectTimer = 0;
  uint16_t dynamicPort = 0;
  uint32_t txId = 0;

  // Retry tracking
  uint8_t fetchRetryCount = 0;
  uint32_t pendingFetchTrackId = 0;

  // Phrase change tracking
  int previousPhraseIdx = -1;

  // Effect/Fill playlist id (0 = off, 1..250 = playlist id in /presets.json).
  // When non-zero and the playlist resolves to >=1 entries, Phrase-boundary
  // preset picks draw from that playlist instead of cycling all presets.
  // Fill phrases prefer prolinkFillPlaylist over prolinkEffectPlaylist.
  int prolinkEffectPlaylistId_cfg = 0;
  int prolinkFillPlaylistId_cfg   = 0;

  // Cached resolved preset pools for the two playlist ids above. Refreshed:
  //  (a) on usermod setup,
  //  (b) whenever the configured id changes (readFromConfig),
  //  (c) on a PresetListMutated v3 event.
  std::vector<int> prolinkEffectPool;
  std::vector<int> prolinkFillPool;

  // Last id the cached pool reflects. Used to detect "id changed" so we
  // know when to re-read the playlist JSON from disk.
  int prolinkEffectPlaylistId_cached = -1;
  int prolinkFillPlaylistId_cached   = -1;

  // Anti-repeat memory for phrase-boundary preset picks. -1 = no constraint.
  int previousEffectPresetId = -1;

  // Buffers
  std::vector<uint8_t> metadataBuffer;
  std::vector<uint8_t> waveformBuffer;
  std::vector<uint8_t> artworkBuffer;
  uint32_t artworkId = 0;

  // Buffer management
  size_t waveformAllocated = 0;
  size_t artworkAllocated = 0;

  // Startup timing
  int64_t startupTime = 0;

  // Settings
  bool enabled = false;
  bool enableDebug = true;
  bool enableBeatFlash = true;
  bool enableRandomPreset = false;
  bool enableHighResArtwork = false;
  String playerIPOverride = "";
  uint8_t virtualDeckNumber = WLED_DEVICE_ID_DEFAULT;

  // The phase offset: the deck's grid, shifted onto the audible beat.
  // Positive moves the deck's grid EARLIER, so a flash lands on the beat the
  // board hears instead of ~40 ms behind it.  See the constant above.
  int32_t phaseOffsetMs = PROLINK_PHASE_OFFSET_DEFAULT_MS;

  // How old the clock's anchor may get, in BEATS, before the deck's grid is
  // declared dead.  Live-tunable, because the right number depends on how
  // the deck behaves on the night: four beats is three dropped packets of
  // slack, and a deck that drops more than that needs more.
  //
  // Zero means "never use the deck's grid" - the deck's tempo is published
  // (it is still the deck's tempo) but its grid is not run, so the effect
  // falls back to the AudioReactive model.
  uint8_t anchorMaxBeats = PROLINK_ANCHOR_MAX_BEATS;

  // The deck's OWN phase: 0..1 through the beat as the DECK counts it,
  // measured from the beat packet's arrival.  It is the grid the deck's
  // beat counter runs on, so anything that adds the counter to a phase -
  // the phrase progress - uses this one and not the shifted phase.
  float deckRawPhase = 0.0f;

  // True when the clock's anchor has aged out and the grid is therefore
  // NOT running: the deck is linked but its beat clock has stopped (paused,
  // or its beat packets stopped arriving).  The consumers see no bar
  // position and fall back to their own models; this is only for the
  // info page's diagnosis.
  bool gridStale = false;

  // The clock's anchor, stamped WHERE THE PACKET ARRIVES, and the bar
  // position OF THAT ANCHOR - IN ONE WORD.
  //
  // It used to be stamped in the main loop, when the loop noticed that the
  // beat number had changed.  That put the main loop's own jitter into the
  // deck's phase - MEASURED at up to ~30 ms under a 30 fps effect - and the
  // error against the audio then swung by tens of milliseconds, so no single
  // offset could line the two grids up.  The packet callback is the only
  // place that knows when the beat actually was.
  //
  // The anchor and the position have to be read TOGETHER: the position only
  // means anything next to the beat it was measured at.  Two variables
  // cannot promise that - a reader that caught the callback between its two
  // stores would pair the OLD anchor with the NEW position, and that is a
  // whole beat of error for one frame (the flash in the wrong place, or the
  // downbeat colour on the wrong beat).  The tear is STABLE, so no amount
  // of re-reading detects it.  One 32-bit store cannot be read half-updated
  // on this CPU, so the pair travels in one word:
  //
  //   bits 30..31 : the bar position of the anchor packet, 1..4 as 0..3
  //   bits  0..29 : the anchor, ms since boot, modulo 2^30 (12.4 days)
  //
  // The anchor is always seconds old at most - it is re-stamped on every
  // beat - so the 30-bit wrap never bites, and the elapsed time is recovered
  // with a masked subtraction.
  volatile uint32_t prolinkClockPacked = 0;
  // The change detection for the anchor.  Written by the packet callback and
  // cleared by the main loop when the deck goes away, so it is touched from
  // two tasks: it is volatile, and a single byte's write is atomic here.
  volatile uint8_t  lastPacketBeatNo = 0;

  // Config strings
  static const char _name[];
  static const char _enabled[];
  static const char _debug[];
  static const char _beatFlash[];
  static const char _randomPreset[];
  static const char _highResArt[];
  static const char _ipOverride[];
  static const char _deckNumber[];
  static const char _altcolors[];
  static const char _effectPlaylist[];
  static const char _fillPlaylist[];
  static const char _phaseOffset[];
  static const char _anchorMaxBeats[];

  // Protocol constants
  const uint8_t DBSERVER_MAGIC[5] = { 0x11, 0x87, 0x23, 0x49, 0xae };
  const uint8_t PROLINK_MAGIC[10] = { 0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c };
  const uint8_t XOR_KEY[19] = {
    0xCB, 0xE1, 0xEE, 0xFA, 0xE5, 0xEE, 0xAD, 0xEE, 0xE9, 0xD2,
    0xE9, 0xEB, 0xE1, 0xE9, 0xF3, 0xE8, 0xE9, 0xF4, 0xE1
  };

  // --- Helpers ---

  uint8_t getVirtualDeckNumber() { return virtualDeckNumber; }

  IPAddress getTargetPlayerIP() {
    if (playerIPOverride.length() > 0) {
      IPAddress overrideIP;
      if (overrideIP.fromString(playerIPOverride)) return overrideIP;
    }
    return linkState.activePlayerIP;
  }

  float rawPitchToPercent(uint32_t rawPitch) {
    return (float)((int32_t)rawPitch - PITCH_CENTER) / PITCH_SCALE;
  }

  uint8_t getActivePeerCount() {
    unsigned long now = millis();
    uint8_t count = 0;
    for (int i = 0; i < 64; i++) {
      if ((linkState.peerMap & ((uint64_t)1 << i)) &&
        (now - linkState.peerLastSeen[i] <= PEER_TIMEOUT_MS)) {
        count++;
      }
    }
    return count;
  }

  // --- Playlist pool resolver ---
  // Reads the playlist stored at /presets.json keyed by `playlistId`,
  // extracts its `ps` array (the referenced preset ids), and pushes
  // those that exist in presetCache into `out`. Empty `out` means
  // "playlist unavailable" — caller treats this as cycling/fills
  // disabled per the user's confirmed UX.
  //
  // Acquires the JSON buffer lock briefly. If the lock is contended
  // (e.g. handlePresets() is mid-apply) the call gives up silently and
  // returns the prior cached pool — never blocks the loop.
  void refreshPlaylistPool(int playlistId, std::vector<int>& out) {
    if (playlistId <= 0 || playlistId > 250) return;
    if (presetCache == nullptr || !presetCache[playlistId].exists || !presetCache[playlistId].isPlaylist) {
      if (enableDebug) {
        USER_PRINTF("[ProLink] Playlist %d unavailable (exists=%d isPlaylist=%d)\n",
          playlistId,
          presetCache ? presetCache[playlistId].exists : 0,
          presetCache ? presetCache[playlistId].isPlaylist : 0);
      }
      return;
    }
    if (!requestJSONBufferLock(31)) {
      if (enableDebug) USER_PRINTLN(F("[ProLink] Playlist pool refresh: buffer lock contended, deferring"));
      return;
    }
    if (readObjectFromFileUsingId("/presets.json", playlistId, &doc)) {
      JsonObject fdo = doc.as<JsonObject>();
      JsonObject pl  = fdo["playlist"];
      if (!pl.isNull()) {
        JsonArray ps = pl["ps"];
        if (!ps.isNull()) {
          for (int v : ps) {
            int pid = v;
            if (pid > 0 && pid <= 250 && presetCache[pid].exists) {
              out.push_back(pid);
            }
          }
        }
      }
    }
    releaseJSONBufferLock();
    if (enableDebug) USER_PRINTF("[ProLink] Playlist %d pool resolved to %d entries\n", playlistId, (int)out.size());
  }

  // Wrapper to (id-change-detect) then refresh. Treats an already-attempted
  // (cachedId == configuredId) as "no-op regardless of pool size" so a
  // missing/empty playlist does not repeatedly hit the filesystem.
  void ensurePlaylistPool(int configuredId, int& cachedId, std::vector<int>& pool,
                          const char* whichPlaylist) {
    if (configuredId == cachedId) return;
    cachedId = configuredId;
    pool.clear();
    refreshPlaylistPool(configuredId, pool);
    if (configuredId > 0 && pool.empty() && enableDebug) {
      USER_PRINTF("[ProLink] %s playlist (id=%d) is empty/unresolved — cycling disabled\n",
        whichPlaylist, configuredId);
    }
  }

  // --- PSRAM Memory Management ---

  bool ensureWaveformBuffer(size_t neededSize) {
    if (prolink_waveform_data && waveformAllocated >= neededSize) return true;

    size_t allocSize = max(neededSize, WAVEFORM_BUFFER_MAX);

    if (prolink_waveform_data) {
      WaveformPoint* newBuf = (WaveformPoint*)heap_caps_realloc(
        prolink_waveform_data, allocSize * sizeof(WaveformPoint),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (newBuf) {
        prolink_waveform_data = newBuf;
        waveformAllocated = allocSize;
        return true;
      }
      return false;
    }

    prolink_waveform_data = (WaveformPoint*)heap_caps_malloc(
      allocSize * sizeof(WaveformPoint), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!prolink_waveform_data) {
      prolink_waveform_data = (WaveformPoint*)malloc(allocSize * sizeof(WaveformPoint));
      if (enableDebug && prolink_waveform_data) {
        Serial.println(F("[ProLink] Waveform buffer allocated from regular heap"));
      }
    }

    if (prolink_waveform_data) {
      waveformAllocated = allocSize;
      return true;
    }
    return false;
  }

  bool ensureArtworkBuffer(size_t neededSize) {
    if (prolink_artwork_data && artworkAllocated >= neededSize) return true;

    size_t allocSize = max(neededSize, ARTWORK_BUFFER_MAX);

    if (prolink_artwork_data) {
      uint8_t* newBuf = (uint8_t*)heap_caps_realloc(
        prolink_artwork_data, allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (newBuf) {
        prolink_artwork_data = newBuf;
        artworkAllocated = allocSize;
        return true;
      }
      return false;
    }

    prolink_artwork_data = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!prolink_artwork_data) {
      prolink_artwork_data = (uint8_t*)malloc(allocSize);
      if (enableDebug && prolink_artwork_data) {
        Serial.println(F("[ProLink] Artwork buffer allocated from regular heap"));
      }
    }

    if (prolink_artwork_data) {
      artworkAllocated = allocSize;
      return true;
    }
    return false;
  }

  void freeBuffers() {
    if (prolink_waveform_data) {
      heap_caps_free(prolink_waveform_data);
      prolink_waveform_data = nullptr;
      waveformAllocated = 0;
      prolink_waveform_length = 0;
      prolink_waveform_valid = false;
    }
    if (prolink_artwork_data) {
      heap_caps_free(prolink_artwork_data);
      prolink_artwork_data = nullptr;
      artworkAllocated = 0;
      prolink_artwork_size = 0;
      prolink_artwork_valid = false;
    }
  }

  void clearMetadata() {
    prolink_track_title = "";
    prolink_track_artist = "";
    prolink_track_album = "";
    prolink_track_key = "";
    prolink_track_genre = "";
    prolink_track_label = "";
    prolink_metadata_valid = false;
    artworkId = 0;
  }

  void clearWaveform() {
    prolink_waveform_length = 0;
    prolink_waveform_valid = false;
  }

  void clearArtwork() {
    prolink_artwork_size = 0;
    prolink_artwork_valid = false;
  }

  // --- Network Functions ---

  void sendKeepAlive() {
    if (!Network.isConnected()) return;

    uint8_t deckNum = getVirtualDeckNumber();
    uint8_t pkg[64] = { 0 };

    memcpy(pkg, PROLINK_MAGIC, 10);
    pkg[0x0A] = TYPE_ANNOUNCE;
    snprintf((char*)&pkg[0x0C], 20, "%s", WLED_DEVICE_NAME);
    pkg[0x20] = 0x01; pkg[0x21] = 0x02; pkg[0x23] = 0x36;
    pkg[0x24] = deckNum; pkg[0x25] = 0x01;
    Network.localMAC(&pkg[0x26]);
    uint32_t ip = (uint32_t)Network.localIP();
    memcpy(&pkg[0x2C], &ip, 4);
    pkg[0x30] = getActivePeerCount() + 1;
    pkg[0x34] = 0x01;

    IPAddress broadcast = Network.localIP();
    broadcast[3] = 255;
    udpAnnounce.writeTo(pkg, 54, broadcast, PORT_ANNOUNCE);

    IPAddress lastip = INADDR_NONE;
    for (int i = 0; i < 64; i++) {
      if ((linkState.peerMap & ((uint64_t)1 << i)) && linkState.peerIPs[i][0] != 0) {
        if (lastip != linkState.peerIPs[i]) {
          udpAnnounce.writeTo(pkg, 54, linkState.peerIPs[i], PORT_ANNOUNCE);
          lastip = linkState.peerIPs[i];
        }
      }
    }
  }

  void parseAnnouncePacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 36 || data[0x0A] != TYPE_ANNOUNCE) return;

    uint8_t devID = data[0x24];
    uint8_t deckNum = getVirtualDeckNumber();
    if (devID == 0 || devID >= 64 || devID == deckNum) return;

    unsigned long now = millis();
    uint8_t peerIdx = devID - 1;
    bool wasKnown = (linkState.peerMap & ((uint64_t)1 << peerIdx)) != 0;
    bool wasTimedOut = wasKnown && (now - linkState.peerLastSeen[peerIdx] > PEER_TIMEOUT_MS);

    linkState.peerMap |= ((uint64_t)1 << peerIdx);
    linkState.peerIPs[peerIdx] = packet.remoteIP();
    linkState.peerLastSeen[peerIdx] = now;
    linkState.isConnected = true;

    if (!wasKnown || wasTimedOut) {
      if (enableDebug) {
        Serial.printf("[ProLink] %s peer: Player %d at %s\n",
          wasKnown ? "Returned" : "New", devID, packet.remoteIP().toString().c_str());
      }
      sendKeepAlive();

      if (wasTimedOut && devID == linkState.activePlayerID &&
        linkState.currentTrackId > 0 && lastAnalyzedTrackId != linkState.currentTrackId) {
        fetchRetryCount = 0;
        pendingFetchTrackId = linkState.currentTrackId;
        cleanupTcpConnection();
        fetchState = IDLE;
        startMetadataFetch();
      }
    }
  }

  void parseStatusPacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 0x11F || data[0x0A] != TYPE_STATUS) return;

    prolink_status_packets_public++;

    uint8_t playerID = data[0x21];
    bool isMaster = (data[0x89] & 0x20) != 0;

    // Update peer last seen
    if (playerID > 0 && playerID < 64) {
      uint8_t peerIdx = playerID - 1;
      if (linkState.peerMap & ((uint64_t)1 << peerIdx)) {
        linkState.peerLastSeen[peerIdx] = millis();
      }
    }

    if (!isMaster) return;

    linkState.activePlayerID = playerID;
    linkState.isMaster = true;
    linkState.activePlayerIP = packet.remoteIP();

    // Effective pitch (0x99-0x9C)
    if (packet.length() > 0x9C) {
      uint32_t rawEff = ((uint32_t)data[0x99] << 24) | ((uint32_t)data[0x9A] << 16) |
        ((uint32_t)data[0x9B] << 8) | data[0x9C];
      linkState.effectivePitch = rawPitchToPercent(rawEff);
    }

    // Track ID (0x2C-0x2F)
    if (packet.length() > 0x2F) {
      uint32_t tid = ((uint32_t)data[0x2C] << 24) | ((uint32_t)data[0x2D] << 16) |
        ((uint32_t)data[0x2E] << 8) | data[0x2F];
      prolink_track_id_public = tid;

      if (tid > 0 && tid != linkState.currentTrackId) {
        linkState.currentTrackId = tid;
        linkState.currentSlot = data[0x28];

        if (tid != lastAnalyzedTrackId) {
          fetchRetryCount = 0;
          pendingFetchTrackId = tid;
          cleanupTcpConnection();
          fetchState = IDLE;
          clearMetadata();
          clearWaveform();
          clearArtwork();
          startMetadataFetch();
        }
      }
    }

    // Beat in measure (0xA6)
    if (packet.length() > 0xA6) {
      uint8_t newBeat = data[0xA6];
      // The progress clock is NOT reset here.
      //
      // It used to be, and that was wrong in a way that showed up as a
      // 40 ms phase error: the status packet arrives up to ~100 ms AFTER
      // the beat it reports, so zeroing the progress there re-zeroed it
      // late - the deck's phase was then short by that delay for the rest
      // of the beat.  The clock's anchor is the BEAT packet, which
      // ARRIVES ON THE BEAT, so the status packet has no business
      // touching it.
      linkState.beatInMeasure = newBeat;
      // The bar position is NOT published from here.
      //
      // It used to be, and it was published RAW - the deck's position as it
      // arrives - while the progress clock was shifted onto the audible
      // beat.  A raw position next to a shifted phase is a beat out for the
      // length of the offset, and the downbeat colour lands on the wrong
      // beat.  Both are published together now, in the loop, from the same
      // shifted grid.
      //
      // This value stays as the FALLBACK for a link that sends status but
      // no beat packets.
    }

    // Beats elapsed (0xA0-0xA3)
    if (packet.length() > 0xA3) {
      linkState.beatsElapsed = ((uint32_t)data[0xA0] << 24) | ((uint32_t)data[0xA1] << 16) |
        ((uint32_t)data[0xA2] << 8) | data[0xA3];
      prolink_beats_elapsed_public = linkState.beatsElapsed;
    }

    // Track duration (0x14C-0x14F)
    if (packet.length() > 0x14F) {
      uint32_t dur = ((uint32_t)data[0x14C] << 24) | ((uint32_t)data[0x14D] << 16) |
        ((uint32_t)data[0x14E] << 8) | data[0x14F];
      if (dur > 1000 && dur < 7200000) {
        prolink_track_duration_ms = dur;
      }
    }

    // Half-bars (0x11D, 0x11E)
    if (packet.length() > 0x11E) {
      linkState.halfBarsRemaining = data[0x11D];
      linkState.halfBarsElapsed = data[0x11E];
    }
  }

  void parseBeatPacket(AsyncUDPPacket packet) {
    uint8_t* data = packet.data();
    if (packet.length() < 60 || data[0x0A] != TYPE_BEAT) return;

    prolink_beat_packets_public++;

    uint8_t playerID = data[0x21];
    if (playerID != linkState.activePlayerID) return;

    // Update peer last seen
    if (playerID > 0 && playerID < 64) {
      uint8_t peerIdx = playerID - 1;
      if (linkState.peerMap & ((uint64_t)1 << peerIdx)) {
        linkState.peerLastSeen[peerIdx] = millis();
      }
    }

    uint16_t baseBpmRaw = (data[0x5A] << 8) | data[0x5B];
    uint32_t speedRaw = ((uint32_t)data[0x54] << 24) | ((uint32_t)data[0x55] << 16) |
      ((uint32_t)data[0x56] << 8) | data[0x57];

    float baseBpm = baseBpmRaw / 100.0f;
    float speedRatio = speedRaw / SPEED_UNITY;

    linkState.bpm = baseBpm * speedRatio;
    linkState.beatNumber = data[0x5C];

    prolink_beat_number_public = linkState.beatNumber;

    // The anchor, stamped HERE - at the packet - and not in the main loop.
    //
    // This packet announces a beat, and the moment it lands is where the
    // deck's beat IS as the board sees it - which is ~40 ms after the beat
    // the board HEARS.  The grid is anchored here and then shifted back
    // onto the audible beat by the phase offset.
    //
    // Stamping it later (in the main loop, when the loop noticed the beat
    // number change) put the main loop's own jitter into the deck's phase -
    // MEASURED at up to ~30 ms under a 30 fps effect - and the error against
    // the audio then swung by tens of milliseconds, so no single phase
    // offset could line the two grids up.
    //
    // The anchor AND its bar position go into ONE word, in ONE store.
    //
    // They have to travel together: the position is only meaningful next to
    // the beat it was measured at, and a reader that caught this callback
    // between two separate stores would pair the OLD anchor with the NEW
    // position - a whole beat of error for one frame, and a tear that no
    // amount of re-reading would detect.  See the member's comment.
    //
    // A beat-in-bar byte outside 1..4 is not a position at all, so it does
    // not move the clock: the previous packet's anchor and position stay.
    if (linkState.beatNumber != lastPacketBeatNo &&
        linkState.beatNumber >= 1 && linkState.beatNumber <= 4) {
      lastPacketBeatNo = linkState.beatNumber;
      prolinkClockPacked = (millis() & PROLINK_CLOCK_ANC_MASK) |
                           ((uint32_t)(linkState.beatNumber - 1u) << PROLINK_CLOCK_POS_SHIFT);
    }

    linkState.pitchPercent = (speedRatio - 1.0f) * 100.0f;
    prolink_pitchPercent = linkState.pitchPercent;
  }

  // --- TCP Connection Management ---

  void cleanupTcpConnection() {
    if (dbClient.connected()) dbClient.stop();
    while (dbClient.available()) dbClient.read();
    txId = 0;
  }

  void startMetadataFetch() {
    IPAddress targetIP = getTargetPlayerIP();
    if (targetIP[0] == 0) {
      if (enableDebug) Serial.println(F("[ProLink] No target player IP"));
      return;
    }

    if (fetchRetryCount >= FETCH_MAX_RETRIES) {
      if (enableDebug) Serial.printf("[ProLink] Max retries reached for track %u\n", pendingFetchTrackId);
      fetchState = IDLE;
      return;
    }

    fetchState = CONNECT_STAGE1;
    stateTimer = millis();
    if (enableDebug) {
      Serial.printf("[ProLink] Fetching track %u (attempt %d/%d)\n",
        linkState.currentTrackId, fetchRetryCount + 1, FETCH_MAX_RETRIES);
    }
  }

  void scheduleFetchRetry() {
    fetchRetryCount++;
    if (fetchRetryCount < FETCH_MAX_RETRIES) {
      fetchState = RETRY_WAIT;
      stateTimer = millis();
      if (enableDebug) Serial.printf("[ProLink] Retry %d/%d in %ums\n",
        fetchRetryCount + 1, FETCH_MAX_RETRIES, FETCH_RETRY_DELAY_MS);
    } else {
      if (enableDebug) Serial.println(F("[ProLink] All retries exhausted"));
      fetchState = IDLE;
    }
  }

  void sendDBQuery(uint16_t type, uint32_t* args, uint8_t argCount) {
    if (!dbClient.connected()) return;
    txId++;

    dbClient.write(DBSERVER_MAGIC, 5);

    uint8_t head[] = { 0x11, (uint8_t)(txId >> 24), (uint8_t)(txId >> 16),
                      (uint8_t)(txId >> 8), (uint8_t)txId,
                      0x10, (uint8_t)(type >> 8), (uint8_t)type, 0x0F, argCount };
    dbClient.write(head, 10);

    uint8_t argHead[] = { 0x14, 0, 0, 0, 0x0C };
    dbClient.write(argHead, 5);

    for (int i = 0; i < argCount; i++) dbClient.write((uint8_t)0x06);
    for (int i = argCount; i < 12; i++) dbClient.write((uint8_t)0x00);

    for (int i = 0; i < argCount; i++) {
      dbClient.write((uint8_t)0x11);
      uint32_t val = args[i];
      uint8_t b[] = { (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val };
      dbClient.write(b, 4);
    }
  }

  bool readBytesWithTimeout(uint8_t* buf, size_t len, int timeoutMs = 500) {
    unsigned long start = millis();
    size_t bytesRead = 0;
    while (bytesRead < len) {
      if (millis() - start > (unsigned long)timeoutMs) return false;
      if (dbClient.available()) {
        buf[bytesRead++] = dbClient.read();
      } else {
        vTaskDelay(1);
      }
    }
    return true;
  }

  void collectAvailableData(std::vector<uint8_t>& buffer, size_t maxSize = 65535) {
    size_t chunkSize = 0;
    while (dbClient.available() && buffer.size() < maxSize) {
      buffer.push_back(dbClient.read());
      if (++chunkSize >= 1024) {
        vTaskDelay(1);
        chunkSize = 0;
      }
    }
  }

  String getPhraseName(uint16_t kind, uint16_t mood, uint8_t k1, uint8_t k2, uint8_t k3) {
    if (mood == 1 || mood == 0) {
      switch (kind) {
      case 1: return (k1 == 1) ? F("Intro 1") : F("Intro 2");
      case 2:
        if (k2 == 0 && k3 == 0) return F("Up 1");
        if (k2 == 0 && k3 == 1) return F("Up 2");
        if (k2 == 1 && k3 == 0) return F("Up 3");
        return F("Up");
      case 3: return F("Down");
      case 5: return (k1 == 1) ? F("Chorus 2") : F("Chorus 1");
      case 6: return (k1 == 1) ? F("Outro 1") : F("Outro 2");
      default: return "High " + String(kind);
      }
    } else if (mood == 2) {
      switch (kind) {
      case 1: return F("Intro");
      case 2: return F("Verse 1");
      case 3: return F("Verse 2");
      case 4: return F("Verse 3");
      case 5: return F("Verse 4");
      case 6: return F("Verse 5");
      case 7: return F("Verse 6");
      case 8: return F("Bridge");
      case 9: return F("Chorus");
      case 10: return F("Outro");
      default: return "Mid " + String(kind);
      }
    } else if (mood == 3) {
      switch (kind) {
      case 1: return F("Intro");
      case 2: case 3: case 4: return F("Verse 1");
      case 5: case 6: case 7: return F("Verse 2");
      case 8: return F("Bridge");
      case 9: return F("Chorus");
      case 10: return F("Outro");
      default: return "Low " + String(kind);
      }
    }
    return String(kind);
  }

  // --- PSSI Parsing ---

  bool parsePSSI() {
    uint8_t head[28];
    if (!readBytesWithTimeout(head, 28)) return false;

    uint32_t headerLen = ((uint32_t)head[0] << 24) | ((uint32_t)head[1] << 16) |
      ((uint32_t)head[2] << 8) | head[3];
    uint16_t numEntries = (head[12] << 8) | head[13];

    if (headerLen > 10000 || numEntries > 500) {
      if (enableDebug) Serial.printf("[ProLink] Invalid PSSI: len=%u entries=%u\n", headerLen, numEntries);
      return false;
    }

    for (int i = 14; i < 28; i++) {
      head[i] ^= ((XOR_KEY[(i - 14) % 19] + numEntries) & 0xFF);
    }

    uint16_t mood = (head[14] << 8) | head[15];
    switch (mood) {
    case 1: prolink_mood_public = "High"; break;
    case 2: prolink_mood_public = "Mid"; break;
    case 3: prolink_mood_public = "Low"; break;
    default: prolink_mood_public = "Unknown";
    }

    phrases.clear();

    // Skip header padding
    int readSoFar = 4 + 28;
    while (readSoFar < (int)headerLen) {
      if (!dbClient.available()) {
        vTaskDelay(1);
        if (millis() - stateTimer > FETCH_TIMEOUT_MS) return false;
      }
      dbClient.read();
      readSoFar++;
    }

    for (int i = 0; i < numEntries; i++) {
      uint8_t e[24];
      if (!readBytesWithTimeout(e, 24)) return false;

      for (int b = 0; b < 24; b++) {
        int tagOffset = headerLen + (i * 24) + b;
        e[b] ^= ((XOR_KEY[(tagOffset - 18) % 19] + numEntries) & 0xFF);
      }

      uint16_t kind = (e[4] << 8) | e[5];
      uint16_t beat = (e[2] << 8) | e[3];
      bool fill = (e[21] != 0);
      uint16_t beatFill = (e[22] << 8) | e[23];

      PhraseEntry pe;
      pe.index = i;
      pe.kind = kind;
      pe.startBeat = beat;
      pe.count = 0;
      pe.isFill = false;
      pe.label = getPhraseName(kind, mood, e[7], e[9], e[19]);

      if (fill && beatFill > beat) {
        pe.count = beatFill - beat;
        phrases.push_back(pe);

        PhraseEntry peFill;
        peFill.index = i;
        peFill.kind = kind;
        peFill.startBeat = beatFill;
        peFill.count = 0;
        peFill.isFill = true;
        peFill.label = pe.label + " Fill";
        phrases.push_back(peFill);
      } else {
        phrases.push_back(pe);
      }
    }

    // Calculate durations and total beats
    prolink_total_beats = 0;
    for (size_t i = 0; i < phrases.size(); i++) {
      phrases[i].count = (i < phrases.size() - 1) ?
        phrases[i + 1].startBeat - phrases[i].startBeat : 32;
      uint32_t phraseEnd = phrases[i].startBeat + phrases[i].count;
      if (phraseEnd > prolink_total_beats) prolink_total_beats = phraseEnd;
    }
    prolink_total_phrases_public = phrases.size();

    // Initialize preset offset for phrase-based preset selection
    if (prolink_total_phrases_public > 0) {
      auto pool = buildPresetPool();
      if (!pool.empty()) {
        prolink_presetOffset = random(pool.size());
        if (enableDebug) USER_PRINTF("[ProLink] Initialized preset offset: %d\n", prolink_presetOffset);
      }
      // Reset anti-repeat memory so the first phrase boundary has no
      // prior to avoid. We re-refresh playlist pools here too in case
      // a track change happens to coincide with a PresetListMutated
      // event the usermod missed (e.g. between boot and the v3 hookup).
      previousEffectPresetId = -1;
      if (prolinkEffectPlaylistId_cfg > 0) {
        prolinkEffectPool.clear();
        refreshPlaylistPool(prolinkEffectPlaylistId_cfg, prolinkEffectPool);
      }
      if (prolinkFillPlaylistId_cfg > 0) {
        prolinkFillPool.clear();
        refreshPlaylistPool(prolinkFillPlaylistId_cfg, prolinkFillPool);
      }
    }

    return true;
  }

  void printPhrases() {
    Serial.println("=== Parsed PSSI Phrases ===");
    for (size_t i = 0; i < phrases.size(); i++) {
      PhraseEntry& pe = phrases[i];
      USER_PRINTF("Phrase%3d : Start:%5d  Count:%4d  %s %s\n",
        i, pe.startBeat, pe.count, prolink_mood_public.c_str(), pe.label.c_str());
    }
  }

  // --- Metadata Parsing ---

  void requestMetadataSetup() {
    uint8_t deckNum = getVirtualDeckNumber();
    uint32_t compound = ((uint32_t)deckNum << 24) | ((uint32_t)linkState.currentSlot << 16) | 0x0301;
    uint32_t args[] = { compound, linkState.currentTrackId };
    sendDBQuery(0x2002, args, 2);
  }

  void requestMetadataRender() {
    uint8_t deckNum = getVirtualDeckNumber();
    uint32_t compound = ((uint32_t)deckNum << 24) | ((uint32_t)linkState.currentSlot << 16) | 0x0301;
    uint32_t args[] = { compound, 0, 16, 0, 16, 0 };
    sendDBQuery(0x3000, args, 6);
  }

  uint32_t parseNumberField(size_t& pos) {
    if (pos >= metadataBuffer.size()) return 0;
    uint8_t type = metadataBuffer[pos];

    if (type == 0x0f && pos + 1 < metadataBuffer.size()) {
      pos += 2;
      return metadataBuffer[pos - 1];
    } else if (type == 0x10 && pos + 2 < metadataBuffer.size()) {
      uint32_t val = (metadataBuffer[pos + 1] << 8) | metadataBuffer[pos + 2];
      pos += 3;
      return val;
    } else if (type == 0x11 && pos + 4 < metadataBuffer.size()) {
      uint32_t val = ((uint32_t)metadataBuffer[pos + 1] << 24) |
        ((uint32_t)metadataBuffer[pos + 2] << 16) |
        ((uint32_t)metadataBuffer[pos + 3] << 8) |
        metadataBuffer[pos + 4];
      pos += 5;
      return val;
    }
    pos++;
    return 0;
  }

  String parseStringField(size_t& pos) {
    String result = "";
    if (pos >= metadataBuffer.size() - 5 || metadataBuffer[pos] != 0x26) {
      pos++;
      return result;
    }

    uint32_t len = ((uint32_t)metadataBuffer[pos + 1] << 24) |
      ((uint32_t)metadataBuffer[pos + 2] << 16) |
      ((uint32_t)metadataBuffer[pos + 3] << 8) |
      metadataBuffer[pos + 4];
    pos += 5;

    for (uint32_t i = 0; i < len && pos + 1 < metadataBuffer.size(); i++) {
      uint16_t ch = (metadataBuffer[pos] << 8) | metadataBuffer[pos + 1];
      pos += 2;
      if (ch == 0) continue;

      if (ch < 128) {
        result += (char)ch;
      } else if (ch < 0x800) {
        result += (char)(0xC0 | (ch >> 6));
        result += (char)(0x80 | (ch & 0x3F));
      } else {
        result += (char)(0xE0 | (ch >> 12));
        result += (char)(0x80 | ((ch >> 6) & 0x3F));
        result += (char)(0x80 | (ch & 0x3F));
      }
    }
    result.trim();
    return result;
  }

  bool parseMetadataResponse() {
    if (metadataBuffer.size() < 50) return false;

    artworkId = 0;
    clearMetadata();

    if (enableDebug) {
      Serial.printf("[ProLink] Parsing metadata buffer: %d bytes\n", metadataBuffer.size());
    }

    size_t pos = 0;
    int itemsParsed = 0;

    while (pos < metadataBuffer.size() - 30) {
      // Look for dbserver magic
      if (metadataBuffer[pos] != 0x11 || metadataBuffer[pos + 1] != 0x87 ||
        metadataBuffer[pos + 2] != 0x23 || metadataBuffer[pos + 3] != 0x49 ||
        metadataBuffer[pos + 4] != 0xae) {
        pos++;
        continue;
      }

      size_t msgStart = pos;
      pos += 10;  // Skip magic + txid

      if (pos + 3 > metadataBuffer.size() || metadataBuffer[pos] != 0x10) {
        pos = msgStart + 1;
        continue;
      }

      uint16_t msgType = (metadataBuffer[pos + 1] << 8) | metadataBuffer[pos + 2];
      pos += 3;

      if (msgType != 0x4101) {
        pos = msgStart + 1;
        continue;
      }

      if (pos + 2 > metadataBuffer.size() || metadataBuffer[pos] != 0x0f) {
        pos = msgStart + 1;
        continue;
      }
      pos += 2;

      if (pos + 5 > metadataBuffer.size() || metadataBuffer[pos] != 0x14) {
        pos = msgStart + 1;
        continue;
      }

      uint32_t blobLen = ((uint32_t)metadataBuffer[pos + 1] << 24) |
        ((uint32_t)metadataBuffer[pos + 2] << 16) |
        ((uint32_t)metadataBuffer[pos + 3] << 8) |
        metadataBuffer[pos + 4];
      pos += 5 + blobLen;

      if (pos >= metadataBuffer.size() - 20) break;

      parseNumberField(pos);  // Parent ID
      parseNumberField(pos);  // Main ID
      parseNumberField(pos);  // Label 1 size
      String label1 = parseStringField(pos);
      parseNumberField(pos);  // Label 2 size
      parseStringField(pos);  // Label 2
      uint32_t itemType = parseNumberField(pos) & 0xFFFF;
      parseNumberField(pos);  // Flags
      uint32_t arg9 = parseNumberField(pos);  // Artwork ID

      if (enableDebug) {
        Serial.printf("[ProLink] Menu item type=0x%02X label1='%s' arg9=%u\n",
          itemType, label1.c_str(), arg9);
      }

      switch (itemType) {
      case 0x04:
        prolink_track_title = label1;
        if (arg9 > 0 && arg9 < 0x7FFFFFFF) artworkId = arg9;
        break;
      case 0x07: prolink_track_artist = label1; break;
      case 0x02: prolink_track_album = label1; break;
      case 0x0f: prolink_track_key = label1; break;
      case 0x06: prolink_track_genre = label1; break;
      case 0x10: prolink_track_label = label1; break;
      }

      itemsParsed++;
    }

    prolink_metadata_valid = (prolink_track_title.length() > 0 || prolink_track_artist.length() > 0);

    if (enableDebug) {
      Serial.printf("[ProLink] Parsed %d items: %s - %s (art=%u)\n",
        itemsParsed, prolink_track_artist.c_str(), prolink_track_title.c_str(), artworkId);
    }

    return prolink_metadata_valid;
  }

  // --- Waveform Parsing ---

  void requestPreviewWaveform() {
    uint8_t deckNum = getVirtualDeckNumber();
    uint32_t compound = ((uint32_t)deckNum << 24) | ((uint32_t)linkState.currentSlot << 16) | 0x0301;
    uint32_t tag1 = 0x34565750;  // "PWV4"
    uint32_t tag2 = 0x00545845;  // "EXT\0"
    uint32_t args[] = { compound, linkState.currentTrackId, tag1, tag2 };
    sendDBQuery(0x2C04, args, 4);
  }

  bool parseWaveformData() {
    if (waveformBuffer.size() < 50) return false;

    // Find "PWV4" tag
    size_t pwv4Pos = 0;
    for (size_t i = 40; i < min((size_t)120, waveformBuffer.size() - 4); i++) {
      if (waveformBuffer[i] == 0x50 && waveformBuffer[i + 1] == 0x57 &&
        waveformBuffer[i + 2] == 0x56 && waveformBuffer[i + 3] == 0x34) {
        pwv4Pos = i;
        break;
      }
    }

    if (pwv4Pos == 0) return false;

    size_t waveStart = pwv4Pos + 24;
    if (waveStart >= waveformBuffer.size() - 10) return false;

    size_t bytesAvailable = waveformBuffer.size() - waveStart;
    size_t numSamples = min(min(bytesAvailable / 6, WAVEFORM_BUFFER_MAX), (size_t)1200);
    if (numSamples < 10) return false;

    if (!ensureWaveformBuffer(numSamples)) return false;

    uint16_t validPoints = 0;
    for (size_t i = 0; i < numSamples && (waveStart + i * 6 + 5) < waveformBuffer.size(); i++) {
      size_t offset = waveStart + i * 6;
      uint8_t ch3 = waveformBuffer[offset + 3] & 0x7F;
      uint8_t ch4 = waveformBuffer[offset + 4] & 0x7F;
      uint8_t ch5 = waveformBuffer[offset + 5] & 0x7F;

      uint8_t height = max(max(ch3, ch4), ch5);
      uint8_t low = ch5, mid = ch4, high = ch3;

      uint8_t colorIdx = 0;
      if (height > 0) {
        if (low >= mid && low >= high) {
          colorIdx = (mid > low / 2) ? 2 : (mid > low / 4) ? 1 : 0;
        } else if (high >= mid && high >= low) {
          colorIdx = (mid > high / 2) ? 5 : (mid > high / 4) ? 6 : 7;
        } else {
          colorIdx = (low > high) ? 3 : (high > low) ? 4 : 3;
        }
      }

      prolink_waveform_data[validPoints].height = height;
      prolink_waveform_data[validPoints].color = colorIdx;
      prolink_waveform_data[validPoints].r = high;
      prolink_waveform_data[validPoints].g = mid;
      prolink_waveform_data[validPoints].b = low;
      validPoints++;
    }

    prolink_waveform_length = validPoints;
    prolink_waveform_valid = (validPoints > 10);

    if (enableDebug) Serial.printf("[ProLink] Waveform: %u points\n", validPoints);
    return prolink_waveform_valid;
  }

  // --- Artwork Parsing ---

  void requestArtwork() {
    if (artworkId == 0) {
      cleanupTcpConnection();
      fetchState = COOLDOWN;
      stateTimer = millis();
      return;
    }

    uint8_t deckNum = getVirtualDeckNumber();
    uint32_t compound = ((uint32_t)deckNum << 24) | ((uint32_t)linkState.currentSlot << 16) | 0x0301;

    if (enableHighResArtwork) {
      uint32_t args[] = { compound, artworkId, 1 };
      sendDBQuery(0x2003, args, 3);
    } else {
      uint32_t args[] = { compound, artworkId };
      sendDBQuery(0x2003, args, 2);
    }
  }

  bool parseArtworkData() {
    if (artworkBuffer.size() < 100) return false;

    // Find JPEG markers
    int jpegStart = -1, jpegEnd = -1;

    for (size_t i = 0; i < artworkBuffer.size() - 1; i++) {
      if (artworkBuffer[i] == 0xFF && artworkBuffer[i + 1] == 0xD8) {
        jpegStart = i;
        break;
      }
    }
    if (jpegStart < 0) return false;

    for (size_t i = artworkBuffer.size() - 1; i > (size_t)jpegStart; i--) {
      if (artworkBuffer[i] == 0xD9 && artworkBuffer[i - 1] == 0xFF) {
        jpegEnd = i + 1;
        break;
      }
    }
    if (jpegEnd < 0) jpegEnd = artworkBuffer.size();

    size_t jpegSize = jpegEnd - jpegStart;
    if (!ensureArtworkBuffer(jpegSize)) return false;

    memcpy(prolink_artwork_data, &artworkBuffer[jpegStart], jpegSize);
    prolink_artwork_size = jpegSize;
    prolink_artwork_valid = true;

    if (enableDebug) Serial.printf("[ProLink] Artwork: %u bytes\n", jpegSize);
    return true;
  }

  // --- State Machine ---

  void handleFetchStateMachine() {
    if (fetchState == RETRY_WAIT) {
      if (millis() - stateTimer > FETCH_RETRY_DELAY_MS+hw_random(1000,2000)) startMetadataFetch();
      return;
    }

    if (fetchState == IDLE) return;

    if (fetchState == COOLDOWN) {
      if (millis() - stateTimer > 1000) fetchState = IDLE;
      return;
    }

    // Timeout check (except during collection)
    if (fetchState != COLLECT_METADATA && fetchState != COLLECT_WAVEFORM && fetchState != COLLECT_ARTWORK) {
      if (millis() - stateTimer > FETCH_TIMEOUT_MS) {
        if (enableDebug) Serial.println(F("[ProLink] Timeout"));
        cleanupTcpConnection();
        scheduleFetchRetry();
        return;
      }
    }

    IPAddress targetIP = getTargetPlayerIP();

    switch (fetchState) {
    case CONNECT_STAGE1:
      if (targetIP[0] == 0) {
        fetchState = IDLE;
        return;
      }
      if (dbClient.connect(targetIP, PORT_DBSERVER)) {
        dbClient.write("\x00\x00\x00\x0fRemoteDBServer\x00", 19);
        fetchState = WAIT_STAGE1;
        stateTimer = millis();
      } else {
        scheduleFetchRetry();
      }
      break;

    case WAIT_STAGE1:
      if (dbClient.available() >= 2) {
        uint8_t buf[2];
        dbClient.read(buf, 2);
        dynamicPort = (buf[0] << 8) | buf[1];
        dbClient.stop();
        fetchState = CONNECT_STAGE2;
        stateTimer = millis();
      }
      break;

    case CONNECT_STAGE2:
      if (dbClient.connect(targetIP, dynamicPort)) {
        uint8_t hand[] = { 0x11, 0, 0, 0, 1 };
        dbClient.write(hand, 5);
        fetchState = HANDSHAKE;
        stateTimer = millis();
      } else {
        scheduleFetchRetry();
      }
      break;

    case HANDSHAKE:
      if (dbClient.available()) {
        while (dbClient.available()) dbClient.read();

        uint8_t deckNum = getVirtualDeckNumber();
        uint8_t pkt[37] = { 0 };
        memcpy(pkt, DBSERVER_MAGIC, 5);
        uint8_t mid[] = { 0x11, 0xFF, 0xFF, 0xFF, 0xFE, 0x10, 0, 0, 0x0F, 0x01, 0x14, 0, 0, 0, 0x0C };
        memcpy(&pkt[5], mid, 15);
        pkt[20] = 0x06;
        pkt[32] = 0x11;
        pkt[36] = deckNum;
        dbClient.write(pkt, 37);

        fetchState = WAIT_HANDSHAKE;
        stateTimer = millis();
      }
      break;

    case WAIT_HANDSHAKE:
      if (dbClient.available() > 10) {
        while (dbClient.available()) dbClient.read();
        fetchState = REQUEST_PHRASES;
        stateTimer = millis();
      }
      break;

    case REQUEST_PHRASES: {
      uint8_t deckNum = getVirtualDeckNumber();
      uint32_t compound = ((uint32_t)deckNum << 24) | ((uint32_t)linkState.currentSlot << 16) | 0x0301;
      uint32_t args[] = { compound, linkState.currentTrackId, 0x49535350, 0x00545845 };
      sendDBQuery(0x2c04, args, 4);
      fetchState = WAIT_PHRASES;
      stateTimer = millis();
      break;
    }

    case WAIT_PHRASES:
      if (dbClient.available() > 32) {
        bool found = false;
        int limit = 1000;
        while (dbClient.available() >= 4 && limit-- > 0) {
          if (dbClient.peek() == 'P') {
            uint8_t tag[4];
            dbClient.read(tag, 4);
            if (memcmp(tag, "PSSI", 4) == 0) {
              found = true;
              break;
            }
          } else {
            dbClient.read();
          }
        }

        if (found) {
          if (parsePSSI()) {
            if (enableDebug) printPhrases();
            lastAnalyzedTrackId = linkState.currentTrackId;
            fetchRetryCount = 0;
          }
          fetchState = REQUEST_METADATA_SETUP;
          stateTimer = millis();
        }
      }
      break;

    case REQUEST_METADATA_SETUP:
      metadataBuffer.clear();
      requestMetadataSetup();
      fetchState = WAIT_METADATA_SETUP;
      stateTimer = millis();
      break;

    case WAIT_METADATA_SETUP:
      if (dbClient.available() > 10) {
        while (dbClient.available()) dbClient.read();
        fetchState = REQUEST_METADATA_RENDER;
        stateTimer = millis();
      }
      break;

    case REQUEST_METADATA_RENDER:
      requestMetadataRender();
      fetchState = WAIT_METADATA_RENDER;
      stateTimer = millis();
      break;

    case WAIT_METADATA_RENDER:
      if (dbClient.available() > 10) {
        collectTimer = millis();
        fetchState = COLLECT_METADATA;
      }
      break;

    case COLLECT_METADATA:
      collectAvailableData(metadataBuffer, METADATA_BUFFER_MAX);
      if (millis() - collectTimer > METADATA_COLLECT_MS ||
        (!dbClient.available() && metadataBuffer.size() > 100)) {
        parseMetadataResponse();
        fetchState = REQUEST_WAVEFORM;
        stateTimer = millis();
      }
      break;

    case REQUEST_WAVEFORM:
      waveformBuffer.clear();
      requestPreviewWaveform();
      fetchState = WAIT_WAVEFORM;
      stateTimer = millis();
      break;

    case WAIT_WAVEFORM:
      if (!dbClient.connected()) {
        cleanupTcpConnection();
        fetchState = COOLDOWN;
        stateTimer = millis();
        break;
      }
      if (dbClient.available() > 10) {
        collectTimer = millis();
        fetchState = COLLECT_WAVEFORM;
      }
      break;

    case COLLECT_WAVEFORM:
      collectAvailableData(waveformBuffer, 8192);
      vTaskDelay(1);
      if (millis() - collectTimer > WAVEFORM_COLLECT_MS) {
        parseWaveformData();
        fetchState = REQUEST_ARTWORK;
        stateTimer = millis();
      }
      break;

    case REQUEST_ARTWORK:
      artworkBuffer.clear();
      requestArtwork();
      if (fetchState == REQUEST_ARTWORK) {
        fetchState = WAIT_ARTWORK;
        stateTimer = millis();
      }
      break;

    case WAIT_ARTWORK:
      if (!dbClient.connected()) {
        cleanupTcpConnection();
        fetchState = COOLDOWN;
        stateTimer = millis();
        break;
      }
      if (dbClient.available() > 0) {
        collectTimer = millis();
        fetchState = COLLECT_ARTWORK;
      }
      break;

    case COLLECT_ARTWORK: {
      collectAvailableData(artworkBuffer, ARTWORK_BUFFER_MAX);
      vTaskDelay(1);

      static size_t lastSize = 0;
      static unsigned long lastGrowth = 0;

      if (artworkBuffer.size() > lastSize) {
        lastSize = artworkBuffer.size();
        lastGrowth = millis();
      }

      bool hasJpegEnd = false;
      if (artworkBuffer.size() > 100) {
        for (size_t i = artworkBuffer.size() - 1; i > artworkBuffer.size() - 10 && i > 0; i--) {
          if (artworkBuffer[i] == 0xD9 && artworkBuffer[i - 1] == 0xFF) {
            hasJpegEnd = true;
            break;
          }
        }
      }

      bool noGrowth = (millis() - lastGrowth > 500) && (artworkBuffer.size() > 100);
      bool timeout = (millis() - collectTimer > ARTWORK_COLLECT_MS);

      if (hasJpegEnd || noGrowth || timeout) {
        lastSize = 0;
        parseArtworkData();
        cleanupTcpConnection();
        fetchState = COOLDOWN;
        stateTimer = millis();
      }
      break;
    }

    default: break;
    }
  }

  void updatePhraseState() {
    if (phrases.empty()) {
      prolink_phrase_name_public = "";
      prolink_phrase_index_public = -1;
      prolink_phrase_beats_public = 0;
      prolink_phrase_progress_public = 0.0f;
      prolink_track_progress = 0.0f;
      return;
    }

    // Fix volatile issue: cast to non-volatile for comparison
    uint32_t beatsElapsed = linkState.beatsElapsed;
    uint32_t currentBeat = beatsElapsed > 0 ? beatsElapsed : 1;

    if (prolink_total_beats > 0) {
      prolink_track_progress = min((float)currentBeat / (float)prolink_total_beats, 1.0f);
    }

    int activeIdx = -1;
    for (size_t i = 0; i < phrases.size(); i++) {
      if (currentBeat >= phrases[i].startBeat &&
        currentBeat < (phrases[i].startBeat + phrases[i].count)) {
        activeIdx = i;
        break;
      }
    }

    // WLEDMM v3: was `if (prolink_presetMover && ...)` — the global
    // flag was borrowed from another module and synced via a
    // static-guard hack in readFromConfig(). v3 uses the Pioneer-local
    // enableRandomPreset config directly.
    if (enableRandomPreset && activeIdx != previousPhraseIdx && previousPhraseIdx != -1 && activeIdx != -1) {
      bool isFillPhrase = (phrases[activeIdx].label.indexOf("Fill") != -1);
      // Pick the right configured playlist (0 = off, fall through to legacy).
      const std::vector<int>* pickPool = nullptr;
      int configuredId = 0;
      if (isFillPhrase) {
        pickPool     = &prolinkFillPool;
        configuredId = prolinkFillPlaylistId_cfg;
      } else {
        pickPool     = &prolinkEffectPool;
        configuredId = prolinkEffectPlaylistId_cfg;
      }

      int newPreset = -1;

      if (configuredId > 0 && pickPool && !pickPool->empty()) {
        // Configured-playlist branch: anti-repeat picker over the pool.
        //   - 1 entry  → take it (nothing to shuffle)
        //   - 2+       → random, retry until != previousEffectPresetId
        const std::vector<int>& pool = *pickPool;
        if (pool.size() == 1) {
          newPreset = pool[0];
        } else {
          int prev = previousEffectPresetId;
          for (int tries = 0; tries < 8; tries++) {
            int c = pool[random(pool.size())];
            if (c != prev) { newPreset = c; break; }
          }
          if (newPreset < 0) newPreset = pool[random(pool.size())]; // give up anti-repeat
        }
        if (enableDebug) {
          Serial.printf("[ProLink] Phrase %d -> %d. Playlist-%s pick=%d (%s)\n",
            previousPhraseIdx, activeIdx, isFillPhrase ? "fill" : "effect",
            newPreset, newPreset > 0 ? presetCache[newPreset].name : "(none)");
        }
      } else {
        // Legacy behavior — cycle the full preset pool, force-fill
        // behavior for "Fill"-labeled phrases. Same code as before,
        // preserved verbatim except we still update previousEffectPresetId
        // so a later switch to a configured playlist has a prior to avoid.
        auto pool = buildPresetPool();
        newPreset = getPresetForPhraseNoRepeat(activeIdx, pool);

        if (isFillPhrase) {
          newPreset = 2;
        } else if (newPreset == 2 && !pool.empty()) {
          for (int attempts = 0; attempts < 10; attempts++) {
            int candidate = pool[random(pool.size())];
            if (candidate != 2) {
              newPreset = candidate;
              break;
            }
          }
        }
        if (enableDebug) {
          Serial.printf("[ProLink] Phrase %d -> %d. Legacy-pool pick=%d (%s)\n",
            previousPhraseIdx, activeIdx, newPreset,
            (newPreset > 0 && presetCache) ? presetCache[newPreset].name : "(none)");
        }
      }

      if (newPreset > 0) {
        if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
        if (currentPlaylist >= 0) unloadPlaylist();
        applyPreset(newPreset);
        handlePresets();
        previousEffectPresetId = newPreset;
      }
    }
    previousPhraseIdx = activeIdx;

    if (activeIdx != -1) {
      prolink_phrase_index_public = activeIdx;
      prolink_phrase_name_public = phrases[activeIdx].label;
      prolink_phrase_beats_public = phrases[activeIdx].count;

      uint32_t beatsIntoPhrase = currentBeat - phrases[activeIdx].startBeat;
      // The deck's OWN phase - the fraction of the beat since the packet
      // that announced it.  Not the shifted one.
      //
      // The deck's beat COUNTER turns over when the beat packet lands, so
      // the count and the phase have to turn over together: adding the
      // shifted phase (which turns over on the AUDIBLE beat, ~40 ms before
      // the packet) to a count that turns over later would saw the phrase
      // progress down by a whole beat and then jump back up, every beat.
      float totalProgress = (float)beatsIntoPhrase + deckRawPhase;
      if (phrases[activeIdx].count > 0) {
        prolink_phrase_progress_public = totalProgress / (float)phrases[activeIdx].count;
      }
    } else {
      prolink_phrase_index_public = -1;
      prolink_phrase_name_public = "Gap/End";
      prolink_phrase_beats_public = 0;
      prolink_phrase_progress_public = 0.0f;
    }
  }

  void checkPeerTimeouts() {
    unsigned long now = millis();

    for (int i = 0; i < 64; i++) {
      if (!(linkState.peerMap & ((uint64_t)1 << i))) continue;
      if (linkState.peerLastSeen[i] == 0 || (now - linkState.peerLastSeen[i]) <= PEER_TIMEOUT_MS) continue;

      uint8_t devID = i + 1;
      if (enableDebug) Serial.printf("[ProLink] Peer timeout: %d\n", devID);

      linkState.peerMap &= ~((uint64_t)1 << i);
      linkState.peerLastSeen[i] = 0;

      if (devID == linkState.activePlayerID) {
        linkState.isMaster = false;
        linkState.activePlayerID = 0;
        linkState.bpm = 0;
        linkState.beatNumber = 0;
        linkState.beatsElapsed = 0;
        linkState.currentTrackId = 0;

        // The clock's anchor goes with the deck, and so does the change
        // detection that stamps it - otherwise a deck that comes back with
        // the SAME beat number would not re-stamp the anchor, and the clock
        // would still be running off the old deck's arrival.
        prolinkClockPacked = 0;
        lastPacketBeatNo   = 0;

        phrases.clear();
        lastAnalyzedTrackId = 0;
        prolink_phrase_name_public = "";
        prolink_phrase_index_public = -1;
        prolink_mood_public = "";
        previousPhraseIdx = -1;
        previousEffectPresetId = -1;
        prolink_total_beats = 0;

        clearMetadata();
        clearWaveform();
        clearArtwork();
        cleanupTcpConnection();
        fetchState = IDLE;
        fetchRetryCount = 0;
        prolink_connected_public = false;
      }
    }

    linkState.isConnected = (linkState.peerMap > 0);
  }

public:
  ProLinkUsermodV3() : Usermod() { }

  void setup() {
    if (!enabled) return;

    startupTime = esp_timer_get_time();

    // First-run playlist pool resolution. readFromConfig() handles
    // refresh when the user changes the configured id later, but on the
    // very first boot presetCache may already be populated (it is built
    // during WLED boot from /presets.json) so we can pre-warm the pools.
    ensurePlaylistPool(prolinkEffectPlaylistId_cfg, prolinkEffectPlaylistId_cached, prolinkEffectPool, "Effect");
    ensurePlaylistPool(prolinkFillPlaylistId_cfg,   prolinkFillPlaylistId_cached,   prolinkFillPool,   "Fill");

    if (udpStatus.listen(PORT_STATUS)) {
      udpStatus.onPacket([this](AsyncUDPPacket packet) { parseStatusPacket(packet); });
    }
    if (udpBeat.listen(PORT_BEAT)) {
      udpBeat.onPacket([this](AsyncUDPPacket packet) { parseBeatPacket(packet); });
    }
    if (udpAnnounce.listen(PORT_ANNOUNCE)) {
      udpAnnounce.onPacket([this](AsyncUDPPacket packet) { parseAnnouncePacket(packet); });
    }

    sendKeepAlive();

    if (enableDebug) {
      Serial.println(F("[ProLink] Initialized"));
      Serial.printf("[ProLink] Deck: %d\n", virtualDeckNumber);
      if (playerIPOverride.length() > 0) {
        Serial.printf("[ProLink] IP Override: %s\n", playerIPOverride.c_str());
      }
    }
  }

  float getEffectiveBPM() {
    return linkState.bpm;  // Already effective from beat packet
  }

  void loop() {
    if (!enabled) return;

    // How often this loop runs.  The grid below is published from here, so
    // this rate is the age of the published phase - the number that says how
    // stale a consumer's reading can be.  It is measured, not assumed.
    static uint32_t loopTicks = 0;
    static uint32_t loopStamp = 0;
    loopTicks++;
    const uint32_t nowMs = millis();
    if (nowMs - loopStamp >= 1000) {
      prolink_loop_rate_hz_public = (uint16_t)(loopTicks * 1000UL / (nowMs - loopStamp));
      loopTicks = 0;
      loopStamp = nowMs;
    }

    // The connection flag is LIVE, and it is published from here and not
    // from setup().
    //
    // It used to be assigned at the end of setup() - which runs ONCE, at
    // boot, before any deck has had a chance to link.  So the flag said
    // "not connected" for the whole session, and every consumer that
    // gates on it - the strobe effect and the AudioReactive deck
    // reference - saw "no deck" even though the deck was there, its
    // packets were arriving, and the info page (which reads
    // linkState.isMaster directly) was showing it as connected.
    prolink_connected_public = linkState.isMaster;

    bool startupComplete = (esp_timer_get_time() >= startupTime + STARTUP_DELAY_US);

    // Keepalives must always run - CDJs need these every 1.5s
    static unsigned long lastKA = 0;
    if (millis() - lastKA > KEEPALIVE_INTERVAL_MS) {
      sendKeepAlive();
      lastKA = millis();
    }

    // Peer timeout checks are lightweight, keep them running
    static unsigned long lastPeerCheck = 0;
    if (millis() - lastPeerCheck > PEER_CHECK_INTERVAL_MS) {
      checkPeerTimeouts();
      lastPeerCheck = millis();
    }

    // Delay heavy TCP operations until startup complete
    if (!startupComplete) return;

    handleFetchStateMachine();

    // Beat timing
    static unsigned long lastBeatTime = 0;
    static uint32_t lastBeatNumber = 0;
    static uint8_t  lastBeatInMeasure = 0;
    static unsigned long beatFlashStart = 0;
    // The shifted phase as it was last seen, and whether the GRID has
    // already flashed for the beat that is in flight.  The grid's phase
    // crossing is the AUDIBLE beat, so it is what the flash follows.
    static float lastGridPhase = 0.0f;
    static bool  flashedOnGrid  = false;

    if (linkState.beatNumber != lastBeatNumber && linkState.beatNumber > 0) {
      // The BEAT packet announces a beat, and it arrives shortly AFTER the
      // beat it announces - MEASURED, the board's own audio-derived beat
      // fires ~40 ms before the packet reaches it.  So the packet is not
      // the audible beat; it is the deck's beat as the board sees it, LATE.
      // That is why the grid is anchored HERE and then SHIFTED back onto
      // the audible beat by the phase offset - see the clock block below.
      //
      // The status packet's beatInMeasure arrives up to 200 ms later still,
      // so it is only the POSITION IN THE BAR.
      //
      // The old code anchored on beatInMeasure and only drove the flash from
      // beatNumber.  On a link where the status packets do not arrive that
      // meant the clock never started: lastBeatTime stayed 0, so the rate was
      // never published, and every consumer - the strobe effect, the
      // AudioReactive deck reference - saw "no deck" while the beat packets
      // were arriving perfectly well.
      lastBeatNumber = linkState.beatNumber;
      lastBeatTime = millis();

      if (enableBeatFlash && !flashedOnGrid) {
        // The BACKSTOP.  The grid's phase crossing is the audible beat and
        // it is what the flash normally follows (see the grid block), but
        // a loop that stalled over a beat misses the crossing - and then the
        // packet is the only thing that knows a beat happened.  So the
        // packet flashes only when the grid has not already flashed for
        // this beat.
        beatFlashStart = millis();
        prolink_beat_flash_active = true;
        prolink_beat_flash_brightness = 255;
      }
      flashedOnGrid = false;   // the next beat starts unarmed
    }

    // The visible beat number (beatInMeasure) comes from the status packet
    // on a different stream than beatNumber (beat packet).  It is the
    // position in the bar, NOT the anchor - so it only moves the clock when
    // there is no beat packet to anchor it (a link that sends status but no
    // beats).
    if (linkState.beatInMeasure != lastBeatInMeasure && linkState.beatInMeasure > 0) {
      if (prolink_beat_packets_public == 0) lastBeatTime = millis();
      lastBeatInMeasure = linkState.beatInMeasure;
    }

    // Beat flash animation
    if (enableBeatFlash && prolink_beat_flash_active) {
      unsigned long flashElapsed = millis() - beatFlashStart;
      if (flashElapsed < BEAT_FLASH_DURATION_MS) {
        prolink_beat_flash_brightness = 255 - (uint8_t)((flashElapsed * 255) / BEAT_FLASH_DURATION_MS);
      } else {
        prolink_beat_flash_active = false;
        prolink_beat_flash_brightness = 0;
      }
    }

    // The rate is known as soon as a beat packet carries it.  It does NOT
    // wait for the clock's anchor: a rate with no phase is still a rate,
    // and publishing it is what tells a consumer that a deck is there.
    // It is published UNCONDITIONALLY, so a deck that goes away takes its
    // rate with it instead of leaving a stale one behind.
    prolink_bpm_public = linkState.bpm;

    // ---- The clock, and the phase offset ---------------------------------
    //
    // The deck's grid is shifted by `phaseOffsetMs` so that it lands on the
    // beat the board HEARS, not on the beat the deck ANNOUNCES.
    //
    // The shift moves the WHOLE grid: the phase AND the bar position.
    // Moving only the phase would leave the bar a beat behind for the
    // length of the offset, and the downbeat colour would land on the wrong
    // beat.
    //
    // The anchor and the position come out of ONE word, so they cannot be
    // read half-updated.  The word holds the anchor in ms since boot and the
    // bar position of that anchor; the elapsed time is the masked
    // difference, which is exact because the anchor is always seconds old at
    // most (it is re-stamped on every beat).
    //
    // The grid RUNS FREE between the packets: the phase and the bar
    // position advance on their own, and each packet re-anchors them.  That
    // is what makes the shift possible - a grid that only moved when a
    // packet arrived could not be moved onto the audible beat, because the
    // packet arrives after it - and it is why the anchor has to be stamped
    // where the packet LANDS rather than where the loop notices it.
    uint32_t elapsedSinceAnchor = 0;
    uint8_t  rawPos   = 0;
    bool     haveAnchor = false;

    const uint32_t packed = prolinkClockPacked;
    bool     anchorPacked = false;
    if (packed != 0) {
      const uint32_t anchor29 = packed & PROLINK_CLOCK_ANC_MASK;
      const uint32_t now29    = millis() & PROLINK_CLOCK_ANC_MASK;
      const uint32_t elapsed  = (now29 - anchor29) & PROLINK_CLOCK_ANC_MASK;
      if (elapsed < PROLINK_CLOCK_STALE_MS) {
        elapsedSinceAnchor = elapsed;
        rawPos   = (uint8_t)((packed >> PROLINK_CLOCK_POS_SHIFT) & 3u) + 1u;
        haveAnchor = true;
        anchorPacked = true;
      }
    }
    if (!haveAnchor && lastBeatTime > 0) {
      // The fallback: a link that sends status packets but no beat packets.
      elapsedSinceAnchor = millis() - lastBeatTime;
      rawPos   = linkState.beatInMeasure;
      haveAnchor = true;
    }

    // The grid runs only while the anchor is FRESH.
    //
    // A deck that stops sending beat packets - paused, or a 50001-only
    // outage - used to leave the grid running for as long as the LINK stayed
    // up: the anchor was never invalidated, so the phase and the bar
    // position free-ran off it, the strobe kept beating at the deck's tempo
    // in silence, and the downbeat colour painted a beat that was a bar out
    // of place.  The peer timeout does NOT catch this case, because a paused
    // deck keeps sending its announce and status packets - it has only
    // stopped its beat clock.
    //
    // So the anchor has an AGE: the limit is a live setting
    // (Anchor_Max_Beats, four beats by default - three dropped packets of
    // slack), and past it the grid is declared dead.  The consumers then
    // see no bar position (0) and fall back to their own models - the
    // effect's gate wants a position of 1..4, and the AudioReactive
    // reference drops the deck entirely.
    bool gridLive = haveAnchor && (linkState.bpm > 0.0f);
    if (anchorMaxBeats == 0) {
      // "Never use the deck's grid": the tempo is still the deck's, but
      // the grid is not run at all.
      gridLive = false;
    } else if (gridLive) {
      const float beatMs = 60000.0f / linkState.bpm;
      uint32_t maxAnchorAgeMs;
      if (anchorPacked) {
        // The beat packet re-stamps the anchor every beat, so its age grows
        // by one beat between packets.
        maxAnchorAgeMs = (uint32_t)(beatMs * (float)anchorMaxBeats);
      } else {
        // The fallback anchor comes from the STATUS stream, whose rate is
        // not the beat rate, so it gets the link's own liveness window
        // instead of a beat count.
        maxAnchorAgeMs = PEER_TIMEOUT_MS;
      }
      if (elapsedSinceAnchor > maxAnchorAgeMs) {
        gridLive = false;
        // A resume must not look like a phase crossing: the phase restarts
        // from the top of a beat, and a stale phase of 0.999 would fire a
        // flash on top of the packet's own backstop flash.
        lastGridPhase = 0.0f;
        // And the beat must not be left marked as already flashed, or the
        // packet's own backstop flash would be suppressed for the first
        // beat after the deck comes back - the beat that ends the outage
        // would be silent.
        flashedOnGrid = false;
      }
    }
    // Stale means: the deck is still linked and still claiming a tempo, but
    // its beat clock has stopped.  A deck that has gone away entirely (no
    // rate at all) is not stale, it is gone.
    gridStale = (linkState.bpm > 0.0f) && haveAnchor && !gridLive;

    if (gridLive) {
      const float beatMs = 60000.0f / linkState.bpm;
      // The offset, constrained: half a beat or more would fold the grid
      // onto itself and the bar would run backwards.
      int32_t off = phaseOffsetMs;
      const int32_t maxOff = (int32_t)(beatMs * 0.5f);
      if (off >  maxOff) off =  maxOff;
      if (off < -maxOff) off = -maxOff;

      // The shifted clock.  `wholeBeats` is how many shifted beats have
      // passed since the anchor packet's own beat, and the phase is where we
      // are inside the current one.
      //
      // A NEGATIVE shifted time is not an error, it is the previous beat:
      // the anchor is stamped where the packet LANDED, so with the grid
      // moved LATE (a negative offset) the shifted grid's beat has not
      // started yet and we are still inside the one before it.  Borrowing a
      // beat keeps the phase and the bar position CONTINUOUS across the
      // packet arrival; clamping to zero would snap the phase back to the
      // start of the beat on every packet and leave the bar position stuck
      // on the deck's raw announced value for the whole beat.
      float shiftedMs = (float)elapsedSinceAnchor + (float)off;
      int32_t wholeBeats;
      float progress;
      if (shiftedMs < 0.0f) {
        wholeBeats = -1;
        progress = (shiftedMs + beatMs) / beatMs;
      } else {
        const float beatIndex = shiftedMs / beatMs;
        wholeBeats = (int32_t)beatIndex;
        progress = beatIndex - (float)wholeBeats;
      }
      if (progress < 0.0f) progress = 0.0f;
      if (progress >= 1.0f) progress = 0.999f;

      // The deck's OWN phase, from the same anchor but WITHOUT the shift:
      // the fraction of the beat since the packet that announced it.  This
      // is the grid the deck's beat counter runs on, so the phrase progress
      // uses it - the shifted phase above turns over on the AUDIBLE beat,
      // ~40 ms before the counter turns over, and adding the two together
      // would saw the progress down by a beat every beat.
      {
        const float rawIndex = (float)elapsedSinceAnchor / beatMs;
        deckRawPhase = rawIndex - floorf(rawIndex);
      }

      prolink_beat_progress_public = progress;
      prolink_beat_elapsed_ms_public = (uint32_t)(shiftedMs - (float)wholeBeats * beatMs);
      prolink_phase_offset_ms_public = off;

      // The beat flash, on the AUDIBLE beat.
      //
      // The beat packet lands ~40 ms AFTER the beat it announces, so a
      // flash driven by the packet is late by more than a frame.  The grid
      // knows where the audible beat is: the phase crossing zero IS it -
      // that is what the phase offset bought.  So the flash follows the
      // grid, and the packet is only the backstop for a loop that stalled
      // over a beat and missed the crossing.
      if (enableBeatFlash && progress < lastGridPhase) {
        beatFlashStart = nowMs;
        prolink_beat_flash_active = true;
        prolink_beat_flash_brightness = 255;
        flashedOnGrid = true;
      }
      lastGridPhase = progress;

      // The bar position, in the SHIFTED grid: the anchor's own position
      // plus the number of shifted beats since it.  A 4/4 bar advances one
      // beat at a time, so the position follows the shifted grid exactly -
      // and the downbeat lands on the audible downbeat.
      //
      // The +4 keeps the wrap positive: `wholeBeats` is -1 while we are still
      // inside the beat before the anchor's, and a negative value masked with
      // 3 would be a two's-complement accident.
      if (rawPos >= 1 && rawPos <= 4) {
        prolink_beat_public = (uint8_t)(1 + ((((int32_t)(rawPos - 1) + wholeBeats) + 4) & 3));
      } else {
        prolink_beat_public = 0;
      }
    } else {
      // No deck, no clock, or a DEAD anchor: the grid is not running.  The
      // OFFSET is still published, because it is the SETTING - the info page
      // shows it as the number to change, and a page that read 0 with no deck
      // would look like the offset had been lost.
      prolink_beat_progress_public = 0.0f;
      deckRawPhase = 0.0f;
      prolink_beat_elapsed_ms_public = 0;
      prolink_phase_offset_ms_public = phaseOffsetMs;
      prolink_beat_public = 0;
    }

    updatePhraseState();

    // Update bar counters from half-bars
    prolink_bars_elapsed_public = linkState.halfBarsElapsed / 2;
    prolink_bars_remaining_public = linkState.halfBarsRemaining / 2;
    // NOTE: prolink_connected_public is published from loop(), not from
    // here - setup() runs once at boot, before any deck can have linked.
  }

  void addToConfig(JsonObject& root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_debug)] = enableDebug;
    top[FPSTR(_beatFlash)] = enableBeatFlash;
    top[FPSTR(_randomPreset)] = enableRandomPreset;
    top[FPSTR(_highResArt)] = enableHighResArtwork;
    top[FPSTR(_ipOverride)] = playerIPOverride;
    top[FPSTR(_deckNumber)] = virtualDeckNumber;
    top[FPSTR(_altcolors)] = altWaveformColors;
    top[FPSTR(_effectPlaylist)] = prolinkEffectPlaylistId_cfg;
    top[FPSTR(_fillPlaylist)]   = prolinkFillPlaylistId_cfg;
    top[FPSTR(_phaseOffset)]    = phaseOffsetMs;
    top[FPSTR(_anchorMaxBeats)] = anchorMaxBeats;
  }

  bool readFromConfig(JsonObject& root) {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;

    enabled = top[FPSTR(_enabled)] | enabled;
    enableDebug = top[FPSTR(_debug)] | enableDebug;
    enableBeatFlash = top[FPSTR(_beatFlash)] | enableBeatFlash;
    enableRandomPreset = top[FPSTR(_randomPreset)] | enableRandomPreset;
    enableHighResArtwork = top[FPSTR(_highResArt)] | enableHighResArtwork;
    altWaveformColors = top[FPSTR(_altcolors)] | altWaveformColors;
    playerIPOverride = top[FPSTR(_ipOverride)] | "";
    virtualDeckNumber = top[FPSTR(_deckNumber)] | WLED_DEVICE_ID_DEFAULT;
    virtualDeckNumber = constrain(virtualDeckNumber, 1, 127);

    // The phase offset: how far the deck's grid is moved back so the flash
    // lands on the audible beat.  Constrained to a quarter of a second
    // either way - beyond half a beat the shifted grid would fold onto
    // itself, and the bar would run backwards.
    //
    // The value is read the SAFE way, and not with `|` alone.  A settings
    // field that arrives as a string (a POST whose type twin did not arrive)
    // would be read by ArduinoJson's `as<int>()` as 0 - SILENTLY - and an
    // offset of 0 is not a no-op: it puts the deck's grid back where it
    // started, half a beat out from the music.  So a string is accepted only
    // if it really is a number, and anything else leaves the measured default
    // standing.
    {
      int off = (int)phaseOffsetMs;
      JsonVariant offVar = top[FPSTR(_phaseOffset)];
      if (offVar.is<int>()) {
        off = offVar.as<int>();
      } else if (offVar.is<const char*>()) {
        const char* txt = offVar.as<const char*>();
        if (txt != nullptr) {
          char* end = nullptr;
          long parsed = strtol(txt, &end, 10);
          if (end != nullptr && end != txt && *end == '\0') off = (int)parsed;
        }
      }
      if (off < -250) off = -250;
      if (off >  250) off =  250;
      phaseOffsetMs = off;
    }

    // How many beats the clock's anchor may be old before the deck's grid is
    // declared dead.  Read the same safe way as the offset above: a value
    // that arrives as a string would be read as 0 by ArduinoJson, and 0 here
    // means "never use the deck's grid" - a silent, total change of
    // behaviour rather than a rounding error.
    {
      int beats = (int)anchorMaxBeats;
      JsonVariant beatsVar = top[FPSTR(_anchorMaxBeats)];
      if (beatsVar.is<int>()) {
        beats = beatsVar.as<int>();
      } else if (beatsVar.is<const char*>()) {
        const char* txt = beatsVar.as<const char*>();
        if (txt != nullptr) {
          char* end = nullptr;
          long parsed = strtol(txt, &end, 10);
          if (end != nullptr && end != txt && *end == '\0') beats = (int)parsed;
        }
      }
      if (beats < 0)    beats = 0;
      if (beats > 64)   beats = 64;
      anchorMaxBeats = (uint8_t)beats;
    }

    // Effect / fill playlist ids. Default to 0 (off — legacy cycling/fill
    // behavior). Cached pools are rebuilt only when the configured id changes,
    // so toggling in the UI does not repeatedly hit the filesystem.
    int newEffectPlId = top[FPSTR(_effectPlaylist)] | prolinkEffectPlaylistId_cfg;
    int newFillPlId   = top[FPSTR(_fillPlaylist)]   | prolinkFillPlaylistId_cfg;
    newEffectPlId = constrain(newEffectPlId, 0, 250);
    newFillPlId   = constrain(newFillPlId,   0, 250);
    prolinkEffectPlaylistId_cfg = newEffectPlId;
    prolinkFillPlaylistId_cfg   = newFillPlId;

    if (prolinkEffectPlaylistId_cfg != prolinkEffectPlaylistId_cached) {
      prolinkEffectPlaylistId_cached = prolinkEffectPlaylistId_cfg;
      prolinkEffectPool.clear();
      refreshPlaylistPool(prolinkEffectPlaylistId_cfg, prolinkEffectPool);
      if (prolinkEffectPlaylistId_cfg > 0 && prolinkEffectPool.empty() && enableDebug) {
        USER_PRINTF("[ProLink] Effect playlist (id=%d) is empty/unresolved — cycling disabled\n",
          prolinkEffectPlaylistId_cfg);
      }
    }
    if (prolinkFillPlaylistId_cfg != prolinkFillPlaylistId_cached) {
      prolinkFillPlaylistId_cached = prolinkFillPlaylistId_cfg;
      prolinkFillPool.clear();
      refreshPlaylistPool(prolinkFillPlaylistId_cfg, prolinkFillPool);
      if (prolinkFillPlaylistId_cfg > 0 && prolinkFillPool.empty() && enableDebug) {
        USER_PRINTF("[ProLink] Fill playlist (id=%d) is empty/unresolved — fills disabled\n",
          prolinkFillPlaylistId_cfg);
      }
    }

    // WLEDMM v3: removed the static-guard hack that synced the
    // external `prolink_presetMover` global flag from enableRandomPreset.
    // Pioneer v3 now uses enableRandomPreset directly (see the
    // updatePhraseState check), so no shadow state is needed.

    return true;
  }

  void addToJsonInfo(JsonObject& root) {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

    if (!enabled) return;

    JsonArray infoArr = user.createNestedArray("Pro DJ Link");

    String status;
    if (linkState.isMaster && linkState.activePlayerID > 0) {
      status = "M" + String(linkState.activePlayerID) + " | " + String(getEffectiveBPM(), 1) + " BPM";
    } else if (linkState.isConnected) {
      status = String(getActivePeerCount()) + " peer(s)";
    } else {
      status = "Listening...";
    }
    infoArr.add(status);

    // The pane is for reading at a glance, and it had become a report: the
    // clock's counters, the anchor's age limit and the offset's explanation
    // were three more lines of prose.  They are all on /json/ar.
    //
    // What stays is the one number that is tuned LIVE: the phase offset -
    // how far the deck's grid was moved back so the flash lands on the
    // audible beat.
    //
    // The SETTING and the offset IN FORCE are both shown when they differ:
    // the offset in force is clamped to half a beat (a bigger shift would
    // fold the grid onto itself), and a setting past that clamp is not
    // what the grid is actually doing.
    String offsetRow = String(phaseOffsetMs) + " ms set";
    if (linkState.isConnected && (prolink_phase_offset_ms_public != phaseOffsetMs)) {
      offsetRow += ", " + String(prolink_phase_offset_ms_public) + " ms in force";
    }
    // A grid that has stopped is a fault, and a fault is worth a row of
    // its own - but it does not need a paragraph.
    if (gridStale) offsetRow += ", STALE (the grid stopped)";
    infoArr.add("<br />phase offset: " + offsetRow);

    if (prolink_phrase_name_public.length() > 0 && linkState.isMaster) {
      infoArr.add("<br />" + prolink_mood_public + " " + prolink_phrase_name_public);
    }
  }

  uint16_t getId() { return 0xCD30; }

  // WLEDMM v3 hooks. Both default to no-op in the Usermod base
  // class. The v3 Pioneer overrides them to:
  //   (a) log every v3 event for runtime debugging, and
  //   (b) update Pioneer-local state from v3 events (e.g., when
  //       an external usermod publishes PresetCycleRequested,
  //       Pioneer knows the new preset is "active" for display).
  //   Phrase-level detection (Pioneer-internal audio structure
  //   analysis) stays in loop() and is not part of the v3 surface.
  void onPreStateChange(uint8_t mode) override {
    // No-op for now. The latch was added in case we later want to
    // capture the pre-wipe value of currentPreset, but the v2
    // Pioneer doesn't currently track active preset.
  }

  void onEvent(const wled::Event& ev) override {
    // Gated on `enabled` so a disabled usermod doesn't spam the log
    // when something else (autoplaylist, autochange, API calls) drives
    // the preset/effect state machine.
    if (!enabled) return;

    // Log every v3 event the Pioneer v3 receives. Cheap (one
    // USER_PRINTLN per event, <WLED_MAX_USERMODS events/sec), and
    // gives us a record of what the v3 plumbing is doing on-device.
    switch (ev.type) {
      case wled::EventType::PresetApplied:
        USER_PRINTF("[ProLinkV3] PresetApplied slot=%u\n",
                    (unsigned)ev.payload.presetApplied.preset);
        break;
      case wled::EventType::PresetListMutated:
        {
          USER_PRINTF("[ProLinkV3] PresetListMutated kind=%u slot=%u\n",
                      (unsigned)ev.payload.presetListMutated.kind,
                      (unsigned)ev.payload.presetListMutated.slot);
          // A playlist was created/updated/deleted. Invalidate our cached
          // pools so the next phrase boundary re-reads from disk. We
          // don't know which playlist was affected, so refresh both.
          prolinkEffectPool.clear();
          prolinkFillPool.clear();
          prolinkEffectPlaylistId_cached = -1;
          prolinkFillPlaylistId_cached   = -1;
        }
        break;
      case wled::EventType::PlaylistStarted:
        USER_PRINTF("[ProLinkV3] PlaylistStarted playlist=%d entry=%u\n",
                    (int)ev.payload.playlistStarted.playlist,
                    (unsigned)ev.payload.playlistStarted.entry);
        break;
      case wled::EventType::PlaylistEnded:
        USER_PRINTF("[ProLinkV3] PlaylistEnded playlist=%d endPreset=%u\n",
                    (int)ev.payload.playlistEnded.playlist,
                    (unsigned)ev.payload.playlistEnded.endPreset);
        break;
      case wled::EventType::PowerEdge:
        USER_PRINTF("[ProLinkV3] PowerEdge wasOff=%d isOff=%d\n",
                    (int)ev.payload.powerEdge.wasOff,
                    (int)ev.payload.powerEdge.isOff);
        break;
      case wled::EventType::EffectIndexChanged:
        USER_PRINTF("[ProLinkV3] EffectIndexChanged newIndex=%u\n",
                    (unsigned)ev.payload.effectIndexChanged.newIndex);
        break;
      case wled::EventType::PresetCycleRequested:
        USER_PRINTF("[ProLinkV3] PresetCycleRequested slot=%u\n",
                    (unsigned)ev.payload.presetApplied.preset);
        break;
      case wled::EventType::UsbDeviceChanged:
        USER_PRINTF("[ProLinkV3] UsbDeviceChanged connected=%d vid=0x%04X pid=0x%04X name=%s\n",
                    (int)ev.payload.usbDevice.connected,
                    (unsigned)ev.payload.usbDevice.vid,
                    (unsigned)ev.payload.usbDevice.pid,
                    ev.payload.usbDevice.name);
        break;
    }
  }
};

// --- Helper Functions ---

uint32_t getPioneerColorRGB(uint8_t colorIndex) {
  static const uint32_t colors[8] = {
    0x0000FF, 0x0080FF, 0x00FFFF, 0x00FF00,
    0x80FF00, 0xFFFF00, 0xFF8000, 0xFF0000
  };
  return colors[colorIndex & 0x07];
}

uint32_t getWaveformRawRGB(uint16_t index) {
  if (!prolink_waveform_data || index >= prolink_waveform_length) return 0;

  WaveformPoint& wp = prolink_waveform_data[index];

  if (altWaveformColors) return getPioneerColorRGB(wp.color);

  uint8_t low = wp.b, mid = wp.g, high = wp.r;

  uint16_t total = low + mid + high;
  if (total == 0) return 0;

  int16_t hue;
  if (high >= mid && high >= low) {
    hue = (mid >= low) ? ((42 * mid) / (high + 1)) : (255 - ((25 * low) / (high + 1)));
  } else if (mid >= low && mid >= high) {
    hue = (high >= low) ? (85 - ((42 * high) / (mid + 1))) : (85 + ((42 * low) / (mid + 1)));
  } else {
    hue = (mid >= high) ? (170 - ((42 * mid) / (low + 1))) : (170 + ((42 * high) / (low + 1)));
  }

  if (hue < 0) hue += 256;
  if (hue > 255) hue -= 256;

  uint8_t val = wp.height > 0 ? 128 + wp.height : 0;
  uint8_t region = hue / 43;
  uint8_t remainder = (hue - (region * 43)) * 6;

  uint8_t p = 0;
  uint8_t q = (val * (255 - remainder)) >> 8;
  uint8_t t = (val * remainder) >> 8;

  uint8_t r, g, b;
  switch (region) {
  case 0:  r = val; g = t;   b = p;   break;
  case 1:  r = q;   g = val; b = p;   break;
  case 2:  r = p;   g = val; b = t;   break;
  case 3:  r = p;   g = q;   b = val; break;
  case 4:  r = t;   g = p;   b = val; break;
  default: r = val; g = p;   b = q;   break;
  }

  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint16_t getPioneerColorRGB565(uint8_t colorIndex) {
  uint32_t rgb = getPioneerColorRGB(colorIndex);
  return (((rgb >> 16) & 0xF8) << 8) | (((rgb >> 8) & 0xFC) << 3) | ((rgb & 0xFF) >> 3);
}

// --- Static Definitions ---
const char ProLinkUsermodV3::_name[] PROGMEM = "Pro_DJ_Link_v3";
const char ProLinkUsermodV3::_enabled[] PROGMEM = "enabled";
const char ProLinkUsermodV3::_debug[] PROGMEM = "enable_debug";
const char ProLinkUsermodV3::_beatFlash[] PROGMEM = "beat_flash";
const char ProLinkUsermodV3::_randomPreset[] PROGMEM = "Cycle_Presets_on_Phrase";
const char ProLinkUsermodV3::_highResArt[] PROGMEM = "High-Res_Artwork_240x240";
const char ProLinkUsermodV3::_ipOverride[] PROGMEM = "Player_IP_Override";
const char ProLinkUsermodV3::_deckNumber[] PROGMEM = "Virtual_Deck_Number";
const char ProLinkUsermodV3::_altcolors[] PROGMEM = "Use_Alt_Waveform_Colors";
const char ProLinkUsermodV3::_effectPlaylist[] PROGMEM = "Effect_Playlist_Id";
const char ProLinkUsermodV3::_fillPlaylist[] PROGMEM = "Fill_Playlist_Id";
const char ProLinkUsermodV3::_phaseOffset[] PROGMEM = "Phase_Offset_ms";
const char ProLinkUsermodV3::_anchorMaxBeats[] PROGMEM = "Anchor_Max_Beats";