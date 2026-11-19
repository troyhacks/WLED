/*
 * usermod_v3_denon_stagelinq.cpp - see the header.
 *
 * NOT a standalone translation unit: included at the end of
 * usermod_v3_denon_stagelinq.h.
 */

#include "stagelinq_client.h"

// The config keys, in PROGMEM so they do not cost RAM.  They are named
// with a k so they cannot be mistaken for the members they set.
static const char _kName[] PROGMEM = "Denon_StageLinq";
static const char _kEnabled[] PROGMEM = "enabled";
static const char _kDebug[] PROGMEM = "enable_debug";
static const char _kPlayerIp[] PROGMEM = "Player_IP";
static const char _kDeck[] PROGMEM = "Deck";
static const char _kPhaseOffset[] PROGMEM = "Phase_Offset_ms";
static const char _kAnchorMaxBeats[] PROGMEM = "Anchor_Max_Beats";
static const char _kConnect[] PROGMEM = "Connect";

// ---------------------------------------------------------------------------
// The usermod
// ---------------------------------------------------------------------------

void DenonStageLinqUsermod::setup() {
  if (!enabled) return;

  // An address override, for the rare case where the unit announces itself
  // from an address the board cannot use.  The PORT still comes from the
  // announcement - only the announcement carries it.
  if (_playerIp.length() > 0) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (sscanf(_playerIp.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
        a < 256 && b < 256 && c < 256 && d < 256) {
      _client.setOverrideIp(IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d));
    }
  }

  _client.enable(_connectEnabled);
  _client.begin();
  _lastMoveMs = millis();
  _lastPacketMs = millis();
}

void DenonStageLinqUsermod::loop() {
  if (!enabled) return;

  _client.setWantDeck(_deckCfg);
  _client.loop();

  const Stagelinq::DeckBeat& b = _client.beat();
  const Stagelinq::DeckState& s = _client.state(_client.deck());
  const bool linked = _client.linked() && b.valid;

  if (linked) {
    // The GRID'S LIVENESS.
    //
    // A paused deck keeps sending its beat frames - the beat simply stops
    // moving - so a frame arriving is not enough: the beat has to MOVE.
    // When it stops for longer than Anchor_Max_Beats the grid is dead, and
    // the board falls back to its own estimator rather than carrying on
    // flashing on a grid that has stopped.
    if (b.beat != _lastRawBeat) {
      _lastRawBeat = b.beat;
      _lastMoveMs = millis();
      stagelinq_beat_packets_public++;
    }
    _lastPacketMs = millis();
  }

  // The phase, from the deck's own continuous beat position.
  //
  // The beat is continuous, so the phase is simply where the beat has got
  // to, plus the time that has passed since the frame arrived.  That is
  // what makes the wall smooth between the frames.
  float bpm = 0.0f;
  if (linked) bpm = (float)b.bpm;
  if (bpm <= 0.0f && s.bpm > 0.0f) bpm = s.bpm;

  float progress = 0.0f;
  uint8_t beatInBar = 0;
  uint32_t beatsElapsed = 0;

  if (linked && bpm > 0.0f) {
    const float beatMs = 60000.0f / bpm;
    // how long since the frame arrived, and the offset the user wants
    const float sinceMs = (float)(millis() - b.at);
    // The grid is dead once the beat has stopped moving for too long.
    const uint32_t aliveMs = (_anchorMaxBeats == 0)
                           ? 0
                           : (uint32_t)(beatMs * (float)_anchorMaxBeats);
    const bool gridLive = (aliveMs == 0) ? false
                           : ((millis() - _lastMoveMs) < aliveMs)
                             && ((millis() - _lastPacketMs) < (aliveMs + 2000));
    if (gridLive) {
      // The offset shifts the whole grid - the phase AND the bar position -
      // so the light lands on the audible beat and not on the announced one.
      float phase = (float)b.beat + (sinceMs + (float)_phaseOffsetMs) / beatMs;
      while (phase < 0.0f) phase += 4.0f;
      const uint32_t whole = (uint32_t)floorf(phase);
      progress = phase - (float)whole;
      beatInBar = (uint8_t)((whole & 3u) + 1u);
      // The deck's own count of beats into the song, ONE-BASED - the same
      // convention the ProLink count uses, so the two read alike: the count
      // is 1 on the deck's first beat, and (count - 1) mod 4 is the beat in
      // the bar.
      beatsElapsed = (uint32_t)floorf((float)b.beat) + 1u;
    }
  }

  stagelinq_bpm_public = bpm;
  stagelinq_beat_public = beatInBar;
  stagelinq_beat_progress_public = progress;
  stagelinq_beats_elapsed_public = beatsElapsed;
  stagelinq_total_beats_public = (uint32_t)b.totalBeats;
  stagelinq_deck_public = _client.deck();
  stagelinq_phase_offset_ms_public = _phaseOffsetMs;

  const bool connected = linked && beatInBar != 0;
  _wasConnected = connected;
  stagelinq_connected_public = connected;

  // The track, for the info pane.
  if (connected) {
    strncpy(stagelinq_track_public, s.song, sizeof(stagelinq_track_public) - 1);
    stagelinq_track_public[sizeof(stagelinq_track_public) - 1] = 0;
    strncpy(stagelinq_artist_public, s.artist, sizeof(stagelinq_artist_public) - 1);
    stagelinq_artist_public[sizeof(stagelinq_artist_public) - 1] = 0;
  }
}

// ---------------------------------------------------------------------------
// The settings
// ---------------------------------------------------------------------------

void DenonStageLinqUsermod::addToConfig(JsonObject& root) {
  JsonObject top = root.createNestedObject(FPSTR(_kName));
  top[FPSTR(_kEnabled)] = enabled;
  top[FPSTR(_kDebug)] = _debug;
  top[FPSTR(_kPlayerIp)] = _playerIp;
  top[FPSTR(_kDeck)] = _deckCfg;
  top[FPSTR(_kPhaseOffset)] = _phaseOffsetMs;
  top[FPSTR(_kAnchorMaxBeats)] = _anchorMaxBeats;
  top[FPSTR(_kConnect)] = _connectEnabled;
}

bool DenonStageLinqUsermod::readFromConfig(JsonObject& root) {
  JsonObject top = root[FPSTR(_kName)];
  if (top.isNull()) return false;
  bool ok = getJsonValue(top[FPSTR(_kEnabled)], enabled);
  getJsonValue(top[FPSTR(_kDebug)], _debug);
  getJsonValue(top[FPSTR(_kPlayerIp)], _playerIp);
  getJsonValue(top[FPSTR(_kDeck)], _deckCfg);
  getJsonValue(top[FPSTR(_kPhaseOffset)], _phaseOffsetMs);
  getJsonValue(top[FPSTR(_kAnchorMaxBeats)], _anchorMaxBeats);
  getJsonValue(top[FPSTR(_kConnect)], _connectEnabled);

  if (_deckCfg > Stagelinq::STAGELINQ_DECKS) _deckCfg = Stagelinq::STAGELINQ_DECKS;
  if (_anchorMaxBeats > 64) _anchorMaxBeats = 64;
  if (_phaseOffsetMs < -250) _phaseOffsetMs = -250;
  if (_phaseOffsetMs > 250) _phaseOffsetMs = 250;
  return ok;
}

void DenonStageLinqUsermod::readFromJsonState(JsonObject& root) {
  JsonObject top = root[FPSTR(_kName)];
  if (top.isNull()) return;
  // The deck to follow: 0 = the master deck.
  if (top.containsKey(FPSTR(_kDeck))) {
    uint8_t d = 0;
    if (getJsonValue(top[FPSTR(_kDeck)], d)) {
      _deckCfg = (d > Stagelinq::STAGELINQ_DECKS) ? 0 : d;
      _client.setWantDeck(_deckCfg);
    }
  }
  if (top.containsKey(FPSTR(_kPhaseOffset))) {
    int32_t off = 0;
    if (getJsonValue(top[FPSTR(_kPhaseOffset)], off)) {
      _phaseOffsetMs = (off < -250) ? -250 : ((off > 250) ? 250 : off);
    }
  }
  if (top.containsKey(FPSTR(_kConnect))) {
    bool on = true;
    if (getJsonValue(top[FPSTR(_kConnect)], on)) {
      _connectEnabled = on;
      _client.enable(on);
    }
  }
}

// ---------------------------------------------------------------------------
// The readouts
// ---------------------------------------------------------------------------

void DenonStageLinqUsermod::addToJsonInfo(JsonObject& root) {
  JsonObject user = root["u"];
  if (user.isNull()) user = root.createNestedObject("u");

  const Stagelinq::Unit& u = _client.unit();
  char ip[20];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", u.ip[0], u.ip[1], u.ip[2], u.ip[3]);

  // The unit, and whether we are talking to it.
  JsonArray row = user.createNestedArray(FPSTR(_kName));
  if (!_client.enabled()) {
    row.add(F("Connect is off"));
    row.add(F("the deck is not asked for"));
  } else if (!u.known) {
    row.add(F("Listening..."));
    row.add(F("no Denon unit yet"));
  } else if (!stagelinq_connected_public) {
    row.add(ip);
    row.add(F("not connected"));
  } else {
    char bpm[24];
    snprintf(bpm, sizeof(bpm), "%.1f BPM", (double)stagelinq_bpm_public);
    row.add(ip);
    row.add(bpm);
  }

  // Where the beat is.
  JsonArray row2 = user.createNestedArray(F("Denon Bar"));
  if (stagelinq_connected_public) {
    char bar[16], beat[16];
    snprintf(bar, sizeof(bar), "bar %u",
             (unsigned)((stagelinq_beats_elapsed_public ? stagelinq_beats_elapsed_public - 1u : 0u) / 4u));
    snprintf(beat, sizeof(beat), "beat %u/4", (unsigned)stagelinq_beat_public);
    row2.add(bar);
    row2.add(beat);
  } else {
    row2.add(F("no deck"));
    row2.add(F(""));
  }

  // What is playing.  The globals are volatile, so they are copied out
  // before they go into the JSON - ArduinoJson will not take a volatile
  // pointer.
  JsonArray row3 = user.createNestedArray(F("Denon Track"));
  char track[sizeof(stagelinq_track_public)];
  char artist[sizeof(stagelinq_artist_public)];
  memcpy(track, stagelinq_track_public, sizeof(track));
  memcpy(artist, stagelinq_artist_public, sizeof(artist));
  row3.add(track);
  row3.add(artist);
}

void DenonStageLinqUsermod::addToJsonAr(JsonObject& root) {
  // The machine-readable twin of the info pane: every number, as a number,
  // with the unit in the key's name.
  JsonObject sl = root.createNestedObject(F("StageLinq"));
  const Stagelinq::Unit& u = _client.unit();
  char ip[20];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", u.ip[0], u.ip[1], u.ip[2], u.ip[3]);
  sl[F("linked")] = stagelinq_connected_public ? 1 : 0;
  sl[F("bpm")] = (float)stagelinq_bpm_public;
  sl[F("beat")] = (unsigned)stagelinq_beat_public;
  sl[F("beat_progress")] = (float)stagelinq_beat_progress_public;
  sl[F("beats_elapsed")] = (unsigned)stagelinq_beats_elapsed_public;
  sl[F("total_beats")] = (unsigned)stagelinq_total_beats_public;
  sl[F("deck")] = (unsigned)stagelinq_deck_public;
  sl[F("phase_ms")] = (int)stagelinq_phase_offset_ms_public;
  sl[F("packets")] = (unsigned)stagelinq_beat_packets_public;
  sl[F("unit_name")] = u.name;
  sl[F("unit_software")] = u.software;
  sl[F("unit_ip")] = ip;
  sl[F("unit_port")] = (unsigned)u.port;
  // the diagnostics
  sl[F("link")] = _client.linkState();
  sl[F("beat_port")] = _client.beatPort();
  sl[F("state_port")] = _client.statePort();
  sl[F("have_beat_port")] = _client.haveBeatPort() ? 1 : 0;
  sl[F("have_state_port")] = _client.haveStatePort() ? 1 : 0;
  sl[F("beat_up")] = _client.beatUp() ? 1 : 0;
  sl[F("state_up")] = _client.stateUp() ? 1 : 0;
  sl[F("beat_bytes")] = (unsigned)_client.beatBytes();
  sl[F("beat_frames")] = (unsigned)_client.beatFrames();
  sl[F("state_bytes")] = (unsigned)_client.stateBytes();
  sl[F("announce_calls")] = (unsigned)_client.announceCalls();
  sl[F("announce_sent")] = (unsigned)_client.announceSent();
  sl[F("loop_calls")] = (unsigned)_client.loopCalls();
}
