/*
 * usermod_v3_ableton_link.cpp - see the header.
 *
 * NOT a standalone translation unit: included at the end of
 * usermod_v3_ableton_link.h.
 */

#include "usermod_v3_ableton_link.h"

// The config keys, in PROGMEM so they do not cost RAM.
static const char _kLkName[] PROGMEM = "Ableton_Link";
static const char _kLkEnabled[] PROGMEM = "enabled";
static const char _kLkDebug[] PROGMEM = "enable_debug";
static const char _kLkGroup[] PROGMEM = "Multicast_Group";
static const char _kLkPort[] PROGMEM = "Multicast_Port";
static const char _kLkPhaseOffset[] PROGMEM = "Phase_Offset_ms";
static const char _kLkStale[] PROGMEM = "Stale_ms";

// A big-endian int64 off the wire.
static inline int64_t linkBe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t)p[i];
  return (int64_t)v;
}

static inline uint32_t linkBe32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// ---------------------------------------------------------------------------
// The usermod
// ---------------------------------------------------------------------------

void AbletonLinkUsermod::setup() {
  if (!enabled) return;

  // The group and the port the unit announces itself on.  The defaults are
  // what Live 12 uses; the SDK's own default is different, so they are
  // settings and not constants.
  IPAddress group;
  if (group.fromString(_group)) {
    _listening = _udp.listenMulticast(group, _port);
    if (_listening) {
      _udp.onPacket([this](AsyncUDPPacket packet) { onPacket(packet); });
      if (_debug) {
        USER_PRINTF("[Link] listening on %s:%u\n", _group.c_str(), (unsigned)_port);
      }
    } else {
      USER_PRINTF("[Link] could not join %s:%u\n", _group.c_str(), (unsigned)_port);
    }
  } else {
    USER_PRINTF("[Link] '%s' is not an address\n", _group.c_str());
  }
}

void AbletonLinkUsermod::loop() {
  if (!enabled) return;
  publish();
}

// ---------------------------------------------------------------------------
// The packet
//
// "_asdp_v" plus a version byte of 0x01, then the header, then the
// payload as entries of: a uint32 length, the value, then a four-ASCII-
// character key.  The record we want is "sess".
// ---------------------------------------------------------------------------

void AbletonLinkUsermod::onPacket(AsyncUDPPacket packet) {
  const uint8_t* d = packet.data();
  const size_t len = packet.length();

  // The header is 24 bytes: the magic, the message type, the ttl, the
  // group id and the node id.
  if (len < 48) return;
  if (memcmp(d, "_asdp_v", 7) != 0) return;
  if (d[7] != 0x01) return;          // the version byte is the INTEGER 1

  size_t o = 24;
  while (o + 4 <= len) {
    const uint32_t size = linkBe32(d + o);
    o += 4;
    if (size > len - o) return;         // a length we do not believe
    const uint8_t* val = d + o;
    o += size;
    if (o + 4 > len) break;
    const uint8_t* key = d + o;
    o += 4;

    if (memcmp(key, "sess", 4) == 0 && size >= 24) {
      const int64_t usPerBeat = linkBe64(val);
      const int64_t phaseMillionths = linkBe64(val + 8);
      if (usPerBeat <= 0) return;
      _bpm = 60000000.0f / (float)usPerBeat;
      _phase = (double)phaseMillionths / 1000000.0;
      _haveTempo = true;
      _lastPacketMs = millis();
      abletonlink_packets_public++;
      if (_debug && (abletonlink_packets_public % 400u) == 1u) {
        USER_PRINTF("[Link] %.3f BPM, phase %.3f beats\n", (double)_bpm, _phase);
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

void AbletonLinkUsermod::publish() {
  float bpm = 0.0f;
  double phase = 0.0;
  bool live = false;

  if (_haveTempo) {
    // The session is live while its packets are still arriving.
    const uint32_t age = millis() - _lastPacketMs;
    live = (age <= _staleMs);
    if (live) {
      bpm = _bpm;
      // The phase was true when the packet was sent, so it has moved on
      // since - and it moves at the tempo.
      const double beatMs = 60000.0 / (double)_bpm;
      phase = _phase + (double)age / beatMs;
      // The user's offset, in milliseconds - the same live trim the decks
      // have, for a grid that is a shade out.
      phase += (double)_phaseOffsetMs / beatMs;
    }
  }

  uint8_t beatInBar = 0;
  float progress = 0.0f;
  uint32_t beatsElapsed = 0;
  if (live) {
    // Link's phase is an absolute beat count, so the bar and the beat in
    // the bar fall out of it.  Its beat one is the music's beat one.
    double floored = floor(phase);
    const uint32_t whole = (uint32_t)floored;
    progress = (float)(phase - floored);
    beatInBar = (uint8_t)((whole & 3u) + 1u);
    beatsElapsed = whole + 1u;    // one-based, like the decks' counts
  }

  abletonlink_bpm_public = bpm;
  abletonlink_beat_public = beatInBar;
  abletonlink_beat_progress_public = progress;
  abletonlink_beats_elapsed_public = beatsElapsed;
  abletonlink_connected_public = live;
  abletonlink_phase_offset_ms_public = _phaseOffsetMs;
}

// ---------------------------------------------------------------------------
// The settings
// ---------------------------------------------------------------------------

void AbletonLinkUsermod::addToConfig(JsonObject& root) {
  JsonObject top = root.createNestedObject(FPSTR(_kLkName));
  top[FPSTR(_kLkEnabled)] = enabled;
  top[FPSTR(_kLkDebug)] = _debug;
  top[FPSTR(_kLkGroup)] = _group;
  top[FPSTR(_kLkPort)] = _port;
  top[FPSTR(_kLkPhaseOffset)] = _phaseOffsetMs;
  top[FPSTR(_kLkStale)] = _staleMs;
}

bool AbletonLinkUsermod::readFromConfig(JsonObject& root) {
  JsonObject top = root[FPSTR(_kLkName)];
  if (top.isNull()) return false;
  bool ok = getJsonValue(top[FPSTR(_kLkEnabled)], enabled);
  getJsonValue(top[FPSTR(_kLkDebug)], _debug);
  getJsonValue(top[FPSTR(_kLkGroup)], _group);
  getJsonValue(top[FPSTR(_kLkPort)], _port);
  getJsonValue(top[FPSTR(_kLkPhaseOffset)], _phaseOffsetMs);
  getJsonValue(top[FPSTR(_kLkStale)], _staleMs);
  if (_phaseOffsetMs < -250) _phaseOffsetMs = -250;
  if (_phaseOffsetMs > 250) _phaseOffsetMs = 250;
  if (_staleMs < 200) _staleMs = 200;
  if (_staleMs > 30000) _staleMs = 30000;
  return ok;
}

// ---------------------------------------------------------------------------
// The readouts
// ---------------------------------------------------------------------------

void AbletonLinkUsermod::addToJsonInfo(JsonObject& root) {
  JsonObject user = root["u"];
  if (user.isNull()) user = root.createNestedObject("u");

  JsonArray row = user.createNestedArray(F("Ableton Link"));
  if (!enabled) {
    row.add(F("off"));
    row.add(F("the usermod is disabled"));
  } else if (!_listening) {
    row.add(F("not listening"));
    row.add(F("the multicast group was refused"));
  } else if (!abletonlink_connected_public) {
    row.add(F("Listening..."));
    row.add(F("no Ableton Link session"));
  } else {
    char bpm[24];
    snprintf(bpm, sizeof(bpm), "%.1f BPM", (double)abletonlink_bpm_public);
    row.add(bpm);
    row.add(F("Link"));
  }

  JsonArray row2 = user.createNestedArray(F("Ableton Bar"));
  if (abletonlink_connected_public) {
    char bar[16], beat[16];
    const uint32_t beats = abletonlink_beats_elapsed_public ? abletonlink_beats_elapsed_public - 1u : 0u;
    snprintf(bar, sizeof(bar), "bar %u", (unsigned)(beats / 4u));
    snprintf(beat, sizeof(beat), "beat %u/4", (unsigned)abletonlink_beat_public);
    row2.add(bar);
    row2.add(beat);
  } else {
    row2.add(F("no session"));
    row2.add(F(""));
  }
}

void AbletonLinkUsermod::addToJsonAr(JsonObject& root) {
  // The machine-readable twin of the info pane: every number, as a number,
  // with the unit in the key's name.
  JsonObject al = root.createNestedObject(F("AbletonLink"));
  al[F("linked")] = abletonlink_connected_public ? 1 : 0;
  al[F("bpm")] = (float)abletonlink_bpm_public;
  al[F("beat")] = (unsigned)abletonlink_beat_public;
  al[F("beat_progress")] = (float)abletonlink_beat_progress_public;
  al[F("beats_elapsed")] = (unsigned)abletonlink_beats_elapsed_public;
  al[F("phase_ms")] = (int)abletonlink_phase_offset_ms_public;
  al[F("packets")] = (unsigned)abletonlink_packets_public;
  al[F("group")] = _group;
  al[F("port")] = (unsigned)_port;
}
