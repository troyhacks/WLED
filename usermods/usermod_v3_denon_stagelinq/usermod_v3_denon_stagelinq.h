/*
 * usermod_v3_denon_stagelinq.h - the Denon DJ StageLinq usermod.
 *
 * THE DECK, AS A CLOCK
 * --------------------
 * A Pioneer CDJ sends its position out over the network and the board
 * follows it.  A Denon DJ unit does the same thing, with a different
 * protocol: StageLinq.  This usermod speaks it and feeds the same global
 * beat clock the ProLink usermod feeds, so every beat-locked effect on the
 * board works with a Denon deck exactly as it does with a Pioneer one.
 *
 * WHAT IT GIVES THE BOARD
 *   BPM          the tempo, straight from the deck
 *   the beat     a continuous, absolute beat position - the playhead's
 *                place in the track, to the fraction of a beat
 *   the bar      the beat in the bar, 1..4, and the bar number
 *   the master   which deck the unit has made the master
 *
 * WHAT IT DOES NOT GIVE THE BOARD
 *   the song's phrase.  StageLinq has no phrase structure at all - there
 *   is no Phrase state path, and Engine's own beatgrid format holds only
 *   beat markers - so the board's internal bar counter is all there is.
 *   The downbeat, though, IS known: the deck's beat counter starts at the
 *   deck's cue, which is the analysed downbeat, so beat zero is the
 *   downbeat and the bar is simply beat/4.
 *
 * HOW IT FINDS THE DECK
 *   It does not have to be told.  A Denon unit broadcasts itself on UDP
 *   51337 once a second; the board listens, and the announcement carries
 *   both the address and the port.  Nothing has to be configured, and the
 *   board needs to know nothing about the unit in advance.
 *
 * NOT a standalone translation unit: included at the end of this header.
 */

#pragma once

#include "wled.h"
#include <AsyncUDP.h>
#include <AsyncTCP.h>
#include "fcn_declare.h"
// The deck's grid is published to the CORE, so the board's clock and the
// effects can use it without knowing that a deck is where it came from.
#include "ar_publish.h"
#include "stagelinq_client.h"

// The published globals.  The core reads these, the way it reads the
// ProLink's: a deck source is a set of numbers in one place, and the
// arbitration in ar_publish.cpp is the only thing that decides which
// source is driving.
volatile float    stagelinq_bpm_public = 0.0f;           // beats per minute (0 = unknown)
volatile uint8_t  stagelinq_beat_public = 0;             // beat-in-bar 1..4, 0 = unknown
volatile float    stagelinq_beat_progress_public = 0.0f; // 0..1 through the current beat
volatile uint32_t stagelinq_beats_elapsed_public = 0;    // the deck's beat in the song
volatile uint32_t stagelinq_total_beats_public = 0;      // the track's total beats
volatile bool     stagelinq_connected_public = false;    // true while a deck is driving
volatile uint8_t  stagelinq_deck_public = 0;              // 1..4
volatile int32_t  stagelinq_phase_offset_ms_public = 0;  // the offset in force
volatile uint32_t stagelinq_beat_packets_public = 0;     // diagnostics
// The track and the artist, for the info pane.  Not volatile: nothing
// outside this usermod reads them, and ArduinoJson will not take a
// volatile pointer.
char     stagelinq_track_public[40] = "";
char     stagelinq_artist_public[40] = "";

class DenonStageLinqUsermod : public Usermod {
public:
  DenonStageLinqUsermod() : Usermod() {}

  void setup() override;
  void loop() override;

  uint16_t getId() override { return 0x534C; }   // 'SL'

  void addToConfig(JsonObject& root) override;
  bool readFromConfig(JsonObject& root) override;
  void addToJsonInfo(JsonObject& root) override;
  void addToJsonAr(JsonObject& root) override;
  void readFromJsonState(JsonObject& root) override;

private:
  Stagelinq::Client _client;

  // the settings
  bool     _debug = true;
  bool     _connectEnabled = true;
  uint8_t  _deckCfg = 0;          // 0 = the master deck
  int32_t  _phaseOffsetMs = 0;
  uint8_t  _anchorMaxBeats = 4;   // 0 = never trust the deck's grid
  String   _playerIp;

  // the grid
  double   _lastRawBeat = -1.0;   // the last beat we saw, for the liveness test
  uint32_t _lastMoveMs = 0;       // when it last MOVED
  uint32_t _lastPacketMs = 0;     // when the last frame arrived
  bool     _wasConnected = false;
};

#include "usermod_v3_denon_stagelinq.cpp"
