/*
 * usermod_v3_ableton_link.h - Ableton Live's Link, as a clock source.
 *
 * THE THIRD SOURCE
 * ----------------
 * The board can already follow a Pioneer CDJ over Pro DJ Link, and a Denon
 * DJ unit over StageLinq.  Both are decks: they know their own tempo and
 * their own beat position, and they publish it.  Ableton Live's Link is the
 * same idea from the other side - a DAW publishing its tempo and its beat
 * phase over the LAN - so it slots into the same arbitration as the third
 * source.
 *
 * The order of preference is the order of authoring: the Pioneer first,
 * then the Denon, then Ableton.  Only one source drives at a time; the
 * first one that is there wins.
 *
 * WHAT LINK PUBLISHES
 * -------------------
 * A unit broadcasts itself on UDP 224.76.78.75, port 20808, about
 * eighteen times a second.  Each packet carries a session state - the SDK's
 * Timeline - as a 24-byte record called "sess":
 *
 *     +0   int64   MICROSECONDS PER BEAT    the tempo is 60000000 / this
 *     +8   int64   MILLIONTHS OF A BEAT     the phase: where in the bar we are
 *     +16  int64   a microsecond time origin
 *
 * Both are big endian.  The phase advances at exactly the beat rate, so
 * the bar and the beat in the bar fall straight out of it, and the downbeat
 * comes with them: Link's beat one is the music's beat one.
 *
 * So a follower needs no peer handshake, no offset, and no help from the
 * board's own estimator.  It only has to listen.
 *
 * The whole wire format, and the measurements behind it, are in
 * tools/stagelinq_protocol.md.
 *
 * NOT a standalone translation unit: included at the end of this header.
 */

#pragma once

#include "wled.h"
#include <AsyncUDP.h>
#include "fcn_declare.h"
// The source's grid is published to the CORE, so the board's clock and the
// effects can use it without knowing that a DAW is where it came from.
#include "ar_publish.h"

// The published globals.
volatile float    abletonlink_bpm_public = 0.0f;           // beats per minute
volatile uint8_t  abletonlink_beat_public = 0;               // beat in the bar, 1..4
volatile float    abletonlink_beat_progress_public = 0.0f;  // 0..1 through the beat
volatile uint32_t abletonlink_beats_elapsed_public = 0;    // the phase, in beats
volatile bool     abletonlink_connected_public = false;     // true while a session is live
volatile int32_t  abletonlink_phase_offset_ms_public = 0;  // the offset in force
volatile uint32_t abletonlink_packets_public = 0;          // diagnostics

class AbletonLinkUsermod : public Usermod {
public:
  AbletonLinkUsermod() : Usermod() {}

  void setup() override;
  void loop() override;

  uint16_t getId() override { return 0x4C4B; }   // 'LK'

  void addToConfig(JsonObject& root) override;
  bool readFromConfig(JsonObject& root) override;
  void addToJsonInfo(JsonObject& root) override;
  void addToJsonAr(JsonObject& root) override;

private:
  void onPacket(AsyncUDPPacket packet);
  void publish();

  AsyncUDP _udp;
  bool     _haveTempo = false;
  float    _bpm = 0.0f;
  double   _phase = 0.0;      // the phase, in beats
  uint32_t _lastPacketMs = 0;
  uint32_t _staleMs = 2000;   // no packets for this long = the session is gone
  int32_t  _phaseOffsetMs = 0;
  bool     _debug = true;
  bool     _listening = false;
  String   _group = "224.76.78.75";
  uint16_t _port = 20808;
};

#include "usermod_v3_ableton_link.cpp"
