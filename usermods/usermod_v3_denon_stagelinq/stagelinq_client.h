/*
 * stagelinq_client.h - the Denon StageLinq protocol client.
 *
 * StageLinq is Denon DJ's network protocol for lighting and video control.
 * A unit broadcasts itself on UDP 51337 and then offers a set of TCP
 * services - StateMap, BeatInfo, TimeSynchronization, FileTransfer - on
 * dynamic ports.
 *
 * Everything here was MEASURED against a Denon DJ PRIME GO (Engine OS
 * 5.3.0).  The wire format and the reasoning behind every step are in
 * tools/stagelinq_protocol.md, and the host-side implementation that proved
 * it is tools/stagelinq_probe.py.
 *
 * THE THREE RULES, all of them learned the hard way:
 *
 *   1. The token in a services request must be OURS, with the top bit of
 *      the first byte CLEAR.  The unit's own tokens all have that bit set,
 *      and a request carrying one of those is ignored.
 *   2. We must ANNOUNCE OURSELVES before we ask.  Without an announcement
 *      the unit never answers: 0 out of 8 tries.  With one, 7 out of 8.
 *   3. The port in our announcement must be one we are LISTENING on.  The
 *      unit tries to connect back to it, and gives up when it cannot.
 *
 * WHAT THE UNIT GIVES US:
 *
 *   BeatInfo   the beat.  A continuous, fractional, ABSOLUTE beat position
 *              of the playhead, the track's total beats, the tempo, and
 *              the playhead in samples.  The counter advances at exactly
 *              bpm/60 beats per second - it is a beat count, not a clock
 *              that has to be trusted.
 *
 *   StateMap   the deck's own state: which deck is the master, whether it
 *              is playing, and /Engine/Deck{N}/BeatNumber - the deck's own
 *              position inside the bar, 0..4, which is where the bar
 *              starts.
 *
 * WHAT IT DOES NOT GIVE US: any phrase or section structure.  StageLinq
 * has no Phrase state path at all, and Engine's own beatgrid format holds
 * only beat markers, so the internal bar counter is all there is.
 *
 * NOT a standalone translation unit: included at the end of this header.
 */

#pragma once

#include <Arduino.h>
#include <AsyncUDP.h>
#include <AsyncTCP.h>

#define STAGELINQ_DISCOVERY_PORT 51337
// The name we announce ourselves under.  It is also how we tell our own
// announcement apart from the unit's when it comes back to us.
#define STAGELINQ_NAME "wled-stagelinq"
#define STAGELINQ_CALLBACK_PORT  51338   // the first port we try to listen on

namespace Stagelinq {

// The message ids, as measured.
static const uint8_t  MSG_SERVICE_ANNOUNCEMENT = 0;
static const uint8_t  MSG_TIMESTAMP            = 1;
static const uint8_t  MSG_SERVICES_REQUEST     = 2;
static const uint32_t BEAT_START = 0x00000000;
static const uint32_t BEAT_STOP  = 0x00000001;
static const uint32_t BEAT_EMIT  = 0x00000002;
static const uint32_t SMAA_EMIT      = 0x00000000;
static const uint32_t SMAA_SUBSCRIBE = 0x000007D2;

static const uint8_t STAGELINQ_DECKS = 4;   // the protocol knows four decks

// One deck, as the BeatInfo service reports it.
struct DeckBeat {
  double  beat;         // the absolute beat position of the playhead
  double  totalBeats;   // the track's total beats
  double  bpm;          // the tempo
  double  samples;      // the playhead, in samples
  bool    valid;
  uint32_t at;          // millis() when this frame arrived
};

// The deck's own state, from the StateMap.
struct DeckState {
  bool  play;
  bool  master;
  bool  haveMaster;
  bool  haveBeatNumber;
  float beatNumber;     // the deck's position inside the bar, 0..4
  float bpm;
  char  song[40];
  char  artist[40];
};

// What we know about the unit itself.
struct Unit {
  IPAddress ip;
  uint16_t  port;       // its service-request port
  char      name[24];
  char      software[24];
  uint32_t  seen;       // millis() of the last announcement
  bool      known;
};

class Client {
public:
  Client();
  ~Client();

  void begin();
  void loop();

  // Turn the whole thing on or off - the safe mode, for a board that must
  // not talk to anything on the network.
  void enable(bool on);
  bool enabled() const { return _enabled; }

  // Are the services up?  The beat stream is running.
  bool linked() const { return _link == LINK_RUN; }

  const Unit& unit() const { return _unit; }

  // The deck we follow: 0 means "whichever the unit calls the master".
  void setWantDeck(uint8_t d) { _wantDeck = d; }
  uint8_t wantDeck() const { return _wantDeck; }

  // --- diagnostics, for the info pane and /json/ar ---
  uint8_t  linkState() const { return (uint8_t)_link; }
  uint16_t beatPort() const { return _beatPort; }
  uint16_t statePort() const { return _statePort; }
  bool     haveBeatPort() const { return _haveBeatPort; }
  bool     haveStatePort() const { return _haveStatePort; }
  bool     beatUp() const { return _beatUp; }
  bool     stateUp() const { return _stateUp; }
  uint32_t beatBytes() const { return _beatBytes; }
  uint32_t beatFrames() const { return _beatFrames; }
  uint32_t stateBytes() const { return _stateBytes; }
  uint32_t announceCalls() const { return _announceCalls; }
  uint32_t announceSent() const { return _announceSent; }
  uint32_t loopCalls() const { return _loopCalls; }

  // The deck we are actually following, 1..4.
  uint8_t deck() const { return _deck; }

  // The deck number is 1..4, as the deck itself numbers them.
  const DeckBeat& beat(uint8_t deck) const { return _beats[(deck ? deck - 1 : 0) % STAGELINQ_DECKS]; }
  const DeckState& state(uint8_t deck) const { return _states[(deck ? deck - 1 : 0) % STAGELINQ_DECKS]; }

  // The last beat we saw, for the deck we follow.
  const DeckBeat& beat() const { return beat(_deck); }

  // An override for the unit's address, for the rare case where the
  // broadcast reaches us but we cannot use the address it came from.  The
  // PORT still comes from the broadcast - only the broadcast carries it.
  void setOverrideIp(IPAddress ip) { _overrideIp = ip; }

private:
  enum Link { LINK_IDLE, LINK_MAIN, LINK_OPEN, LINK_RUN, LINK_BACKOFF };

  // the transport
  void onDiscovery(AsyncUDPPacket packet);
  void onMainData(const uint8_t* d, size_t n);
  void onBeatData(const uint8_t* d, size_t n);
  void onStateData(const uint8_t* d, size_t n);
  void onMainGone();
  void onBeatGone();
  void onStateGone();

  // the protocol
  void startMain();
  void openMain();
  void openServices();
  void closeAll();
  void announce();
  void newToken();
  void subscribe(const char* path);
  void subscribeAll();
  void flushSubscribes();
  bool sendServiceAnnounce(AsyncClient* c, const char* name);
  void sendServicesRequest(AsyncClient* c);
  void sendReference();
  void pickDeck();

  // the parsing
  void parseMain(const uint8_t* d, size_t n);
  void parseBeat(const uint8_t* d, size_t n);
  void parseState(const uint8_t* d, size_t n);
  void applyState(const char* path, const char* json);

  AsyncUDP      _udp;
  AsyncServer* _server;
  AsyncClient* _main;
  AsyncClient* _beat;
  AsyncClient* _state;

  // A client that has gone away is not deleted where it died: the callback
  // is running INSIDE it.  It is flagged here and reaped in loop().
  bool     _mainDead;
  bool     _beatDead;
  bool     _stateDead;

  // The discovery callback runs in the UDP task and the connections live in
  // the TCP task, so the callback does not start anything: it only notes
  // that there is something to connect to, and loop() does the rest.
  bool     _wantConnect;

  // The network is not up when the usermod's setup() runs - the link can
  // take a second to come - so we do not ask the unit for anything until
  // this.  The unit announces itself once a second, so there is no hurry.
  uint32_t _readyAt;

  bool     _enabled;
  Link     _link;
  uint8_t  _token[16];
  uint16_t _callbackPort;
  uint32_t _backoffUntil;
  uint32_t _lastAnnounce;
  uint32_t _connectAt;      // when we may open the connection
  uint32_t _lastRequest;    // when we last asked for the service list
  uint32_t _requestAt;      // when we may ask (the unit needs a moment)
  uint32_t _lastReference;  // when we last said hello back
  uint64_t _reference;      // our reference counter
  bool     _askedAgain;      // have we nudged it once on this token

  Unit     _unit;
  IPAddress _overrideIp;
  // The address we are TALKING TO.  It is fixed for the life of a
  // connection: the unit's last announcement may be our own, and the
  // services have to be dialled where the main connection went.
  IPAddress _connIp;
  uint16_t  _connPort;
  uint16_t _beatPort;
  uint16_t _statePort;
  bool     _haveBeatPort;
  bool     _haveStatePort;
  bool     _beatUp;
  bool     _stateUp;

  uint8_t  _wantDeck;
  uint8_t  _deck;

  // how much has come in, for the diagnostics
  uint32_t _beatBytes;
  uint32_t _beatFrames;
  uint32_t _stateBytes;
  uint32_t _announceCalls;
  uint32_t _announceSent;
  uint32_t _loopCalls;

  // The subscriptions we still have to send.  They are sent one at a time,
  // from loop(), because twenty-four frames will not fit in the socket's
  // write buffer all at once - and a frame that is only half written is a
  // frame the unit cannot parse.
  static const uint8_t SUBS_MAX = 32;
  char      _subs[SUBS_MAX][56];
  uint8_t   _subsCount;
  uint8_t   _subsNext;

  DeckBeat  _beats[STAGELINQ_DECKS];
  DeckState _states[STAGELINQ_DECKS];

  // the receive buffers.  Each connection's messages are self-delimiting,
  // so a partial message just waits for the rest.
  uint8_t _mainBuf[256];
  size_t  _mainLen;
  uint8_t _beatBuf[256];
  size_t  _beatLen;
  uint8_t _stateBuf[1024];
  size_t  _stateLen;
};

} // namespace Stagelinq

#include "stagelinq_client.cpp"
