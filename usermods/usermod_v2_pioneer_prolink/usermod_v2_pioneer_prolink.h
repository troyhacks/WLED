#pragma once

/*
   @title     Pro DJ Link (With Metadata, Waveform & Artwork)
   @file      usermod_v2_prolink.h
   @brief     Syncs WLED to Pioneer CDJs including Phrase/Mood analysis,
              track metadata, preview waveform, and album artwork
   @target    ESP32 (Requires AsyncUDP, PSRAM recommended for waveform/artwork)
   @repo      WLED MoonModules
*/

#include "wled.h"
#ifndef WLED_IDF_BUILD
#include <AsyncUDP.h>
#include <WiFiClient.h>
#else
#include "idf_shims/WiFiClient.h"
#endif
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

class ProLinkUsermod : public Usermod {

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
      linkState.beatInMeasure = data[0xA6];
      prolink_beat_public = linkState.beatInMeasure;
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

    if (prolink_presetMover && activeIdx != previousPhraseIdx && previousPhraseIdx != -1 && activeIdx != -1) {
      auto pool = buildPresetPool();
      int newPreset = getPresetForPhraseNoRepeat(activeIdx, pool);

      if (phrases[activeIdx].label.indexOf("Fill") != -1) {
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

      if (newPreset > 0) {
        if (enableDebug) {
          Serial.printf("[ProLink] Phrase %d -> %d. Applying Preset %d (%s)\n",
            previousPhraseIdx, activeIdx, newPreset, presetCache[newPreset].name);
        }
        if (strip.getSegmentsNum() > 1) strip.resetSegments(false);
        if (currentPlaylist >= 0) unloadPlaylist();
        applyPreset(newPreset);
        handlePresets();
      }
    }
    previousPhraseIdx = activeIdx;

    if (activeIdx != -1) {
      prolink_phrase_index_public = activeIdx;
      prolink_phrase_name_public = phrases[activeIdx].label;
      prolink_phrase_beats_public = phrases[activeIdx].count;

      uint32_t beatsIntoPhrase = currentBeat - phrases[activeIdx].startBeat;
      float totalProgress = (float)beatsIntoPhrase + prolink_beat_progress_public;
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

        phrases.clear();
        lastAnalyzedTrackId = 0;
        prolink_phrase_name_public = "";
        prolink_phrase_index_public = -1;
        prolink_mood_public = "";
        previousPhraseIdx = -1;
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
  ProLinkUsermod() : Usermod() { }

  void setup() {
    if (!enabled) return;

    startupTime = esp_timer_get_time();

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
    static unsigned long beatFlashStart = 0;

    if (linkState.beatNumber != lastBeatNumber && linkState.beatNumber > 0) {
      lastBeatTime = millis();
      lastBeatNumber = linkState.beatNumber;

      if (enableBeatFlash) {
        beatFlashStart = millis();
        prolink_beat_flash_active = true;
        prolink_beat_flash_brightness = 255;
      }
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

    // Beat progress calculation
    if (linkState.bpm > 0 && lastBeatTime > 0) {
      float effectiveBpm = linkState.bpm;
      prolink_bpm_public = effectiveBpm;
      float beatDurationMs = 60000.0f / effectiveBpm;
      unsigned long timeSinceBeat = millis() - lastBeatTime;

      float progress = timeSinceBeat / beatDurationMs;
      while (progress >= 1.0f) {
        progress -= 1.0f;
        lastBeatTime += (unsigned long)beatDurationMs;
      }
      prolink_beat_progress_public = progress;
    } else {
      prolink_beat_progress_public = 0.0f;
    }

    updatePhraseState();

    // Update bar counters from half-bars
    prolink_bars_elapsed_public = linkState.halfBarsElapsed / 2;
    prolink_bars_remaining_public = linkState.halfBarsRemaining / 2;
    prolink_connected_public = linkState.isMaster;
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

    // Static guards to persist across calls
    static bool prolink_presetMover_init = false;
    static bool enableRandomPreset_config_val;  // cache of last config value

    // Seed once, or resync if config changes externally
    if (!prolink_presetMover_init || enableRandomPreset_config_val != enableRandomPreset) {
      prolink_presetMover = enableRandomPreset;          // sync from config
      enableRandomPreset_config_val = enableRandomPreset; // update cache
      prolink_presetMover_init = true;
    }
    
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

    if (prolink_phrase_name_public.length() > 0 && linkState.isMaster) {
      infoArr.add("<br />" + prolink_mood_public + " " + prolink_phrase_name_public);
    }
  }

  uint16_t getId() { return 0xCD30; }
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
const char ProLinkUsermod::_name[] PROGMEM = "Pro_DJ_Link";
const char ProLinkUsermod::_enabled[] PROGMEM = "enabled";
const char ProLinkUsermod::_debug[] PROGMEM = "enable_debug";
const char ProLinkUsermod::_beatFlash[] PROGMEM = "beat_flash";
const char ProLinkUsermod::_randomPreset[] PROGMEM = "Cycle_Presets_on_Phrase";
const char ProLinkUsermod::_highResArt[] PROGMEM = "High-Res_Artwork_240x240";
const char ProLinkUsermod::_ipOverride[] PROGMEM = "Player_IP_Override";
const char ProLinkUsermod::_deckNumber[] PROGMEM = "Virtual_Deck_Number";
const char ProLinkUsermod::_altcolors[] PROGMEM = "Use_Alt_Waveform_Colors";