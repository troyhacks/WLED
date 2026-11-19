/*
 * stagelinq_client.cpp - see the header.
 *
 * NOT a standalone translation unit: included at the end of stagelinq_client.h.
 */

#include "stagelinq_client.h"
#include <esp_random.h>

namespace Stagelinq {

// ---------------------------------------------------------------------------
// The wire format's little helpers.
//
// All integers are big-endian.  A network string is a uint32 length IN BYTES
// followed by that many bytes of UTF-16BE.
// ---------------------------------------------------------------------------

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint64_t be64(const uint8_t* p) {
  return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}
static inline double bef64(const uint8_t* p) {
  uint64_t u = be64(p);
  double d;
  memcpy(&d, &u, sizeof(d));
  return d;
}

static inline void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void put64(uint8_t* p, uint64_t v) {
  put32(p, (uint32_t)(v >> 32));
  put32(p + 4, (uint32_t)(v & 0xFFFFFFFFu));
}

// Write a network string: the length in BYTES, then UTF-16BE.
//
// BIG ENDIAN: the high byte of each code unit comes FIRST, so for an ASCII
// character that is a zero byte and the character follows it.
static size_t putString(uint8_t* out, const char* s) {
  size_t n = strlen(s);
  put32(out, (uint32_t)(n * 2));
  for (size_t i = 0; i < n; i++) {
    out[4 + i * 2] = 0;
    out[4 + i * 2 + 1] = (uint8_t)s[i];
  }
  return 4 + n * 2;
}

// Is this UTF-16BE payload the ASCII string a?
//
// BIG ENDIAN: the HIGH byte of each code unit comes first, so for an ASCII
// character that high byte is zero and the CHARACTER is the second byte.
// Reading the first byte gives a string of NULs - which looks like an empty
// string, and matches nothing.
static bool utf16eq(const uint8_t* s, size_t n, const char* a) {
  size_t len = strlen(a);
  if (n != len * 2) return false;
  for (size_t i = 0; i < len; i++) {
    if (s[i * 2] != 0 || s[i * 2 + 1] != (uint8_t)a[i]) return false;
  }
  return true;
}

// Copy a UTF-16BE payload out as ASCII.  Good enough for a track title.
static void utf16toAscii(const uint8_t* s, size_t n, char* out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; i + 1 < n && o + 1 < outLen; i += 2) out[o++] = (char)s[i + 1];
  out[o] = 0;
}

// The StateMap's values are a little JSON object: {"type":0,"value":129.9}
// or {"type":1,"state":true} or {"type":8,"string":"..."}.  Hand-parsed, so
// that nothing has to take the JSON buffer lock on a network callback.
static bool jsonNum(const char* js, const char* key, float* out) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(js, pat);
  if (!p) return false;
  p += strlen(pat);
  char* end = nullptr;
  double v = strtod(p, &end);
  if (end == p) return false;
  *out = (float)v;
  return true;
}

static bool jsonBool(const char* js, const char* key, bool* out) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(js, pat);
  if (!p) return false;
  p += strlen(pat);
  if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
  if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

static bool jsonStr(const char* js, const char* key, char* out, size_t outLen) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char* p = strstr(js, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t o = 0;
  while (*p && *p != '"' && o + 1 < outLen) out[o++] = *p++;
  out[o] = 0;
  return true;
}

// ---------------------------------------------------------------------------
// The client
// ---------------------------------------------------------------------------

Client::Client() {
  _server = nullptr;
  _main = _beat = _state = nullptr;
  _enabled = false;
  _link = LINK_IDLE;
  _callbackPort = 0;
  _backoffUntil = 0;
  _lastAnnounce = 0;
  _beatPort = _statePort = 0;
  _haveBeatPort = _haveStatePort = false;
  _beatUp = _stateUp = false;
  _wantDeck = 0;
  _deck = 1;
  _wantConnect = false;
  _beatBytes = _beatFrames = _stateBytes = 0;
  _announceCalls = _announceSent = _loopCalls = 0;
  _readyAt = 0;
  _mainLen = _beatLen = _stateLen = 0;
  memset(_token, 0, sizeof(_token));
  memset(&_unit, 0, sizeof(_unit));
  _connIp = IPAddress(0, 0, 0, 0);
  _connPort = 0;
  memset(_beats, 0, sizeof(_beats));
  memset(_states, 0, sizeof(_states));
}

Client::~Client() {
  closeAll();
  if (_server) { _server->end(); delete _server; _server = nullptr; }
}

void Client::enable(bool on) {
  if (_enabled == on) return;
  _enabled = on;
  if (!on) {
    closeAll();
    _link = LINK_IDLE;
  }
}

// A fresh token, with the top bit of the first byte CLEAR.  The unit
// ignores a request that carries one of ITS tokens, and its own tokens all
// have that bit set.
void Client::newToken() {
  for (int i = 0; i < 4; i++) {
    uint32_t r = esp_random();
    memcpy(_token + i * 4, &r, 4);
  }
  _token[0] &= 0x7F;
}

void Client::begin() {
  if (!_enabled) return;

  // The port we announce must be one the unit can CONNECT to, so we open a
  // real listener for it.  The unit does call back on it - with its own
  // ServicesRequest, which we have no use for.
  if (!_server) {
    for (uint16_t p = STAGELINQ_CALLBACK_PORT; p < STAGELINQ_CALLBACK_PORT + 8; p++) {
      AsyncServer* s = new AsyncServer(p);
      s->onClient([](void* arg, AsyncClient* c) {
        // The unit's callback.  It carries a ServicesRequest we do not need.
        (void)arg;
        if (c) { c->close(); }
      }, this);
      s->begin();
      if (s->status() != 0) {          // the port is taken
        _server = s;
        _callbackPort = p;
        break;
      }
      s->end();
      delete s;
    }
  }

  // The discovery broadcast.  The unit tells us where it is once a second;
  // we never have to be told an address.
  _udp.listen(STAGELINQ_DISCOVERY_PORT);
  _udp.onPacket([this](AsyncUDPPacket packet) { onDiscovery(packet); });

  _link = LINK_IDLE;
  _readyAt = millis() + 8000;
}

// ---------------------------------------------------------------------------
// Discovery
//
// The unit's announcement: "airD" + token + source + action + software +
// version + the TCP port it wants us to use.
// ---------------------------------------------------------------------------

void Client::onDiscovery(AsyncUDPPacket packet) {
  if (!_enabled) return;
  uint8_t* data = packet.data();
  size_t len = packet.length();
  if (len < 4 + 16 + 4) return;
  if (memcmp(data, "airD", 4) != 0) return;
  // Our OWN announcement comes back to us - we are listening on the very
  // port we broadcast to - so ignore the one carrying our own token.
  if (len >= 4 + 16 && memcmp(data + 4, _token, 16) == 0) return;

  size_t o = 4 + 16;                       // the token, which we do not need
  char source[24] = "", software[24] = "", version[16] = "";
  // four strings, then the port
  for (int i = 0; i < 4; i++) {
    if (o + 4 > len) return;
    uint32_t n = be32(data + o);
    o += 4;
    if (o + n > len) return;
    char* dst = (i == 0) ? source : (i == 1 ? nullptr : (i == 2 ? software : version));
    if (dst) {
      size_t max = (i == 0) ? sizeof(source) : (i == 2 ? sizeof(software) : sizeof(version));
      utf16toAscii(data + o, n, dst, max);
    }
    o += n;
  }
  if (o + 2 > len) return;
  uint16_t port = be16(data + o);

  // OUR OWN ANNOUNCEMENT COMES BACK TO US.
  //
  // We are listening on the very port we broadcast to, so our own packet
  // arrives here like any other.  If we took it for the unit's, we would
  // spend our whole time talking to ourselves: the unit's address would be
  // overwritten with ours, and the service connections would be dialled
  // back to this board.  The token cannot be used to tell them apart - it
  // changes every time we reconnect - so the NAME is what we go by.
  if (strcmp(source, STAGELINQ_NAME) == 0) return;

  // The unit announces itself under more than one identity.  Only the
  // Engine OS one - JP11 on a PRIME GO - serves the services we want; the
  // others never answer.  So we take the one whose software name is not
  // the offline analyser.
  if (software[0] && strstr(software, "Offline") != nullptr) return;

  IPAddress ip = packet.remoteIP();
  bool changed = (ip != _unit.ip) || (port != _unit.port) || !_unit.known;
  _unit.ip = ip;
  _unit.port = port;
  _unit.seen = millis();
  _unit.known = true;
  strncpy(_unit.name, source, sizeof(_unit.name) - 1);
  strncpy(_unit.software, software, sizeof(_unit.software) - 1);

  // NOTE: we do not connect from here.  This callback runs in the UDP
  // task, and the connections live in the TCP task: tearing one down from
  // here could delete a client that is running its own callback.  We only
  // note that there is something to connect to.
  if (changed) _wantConnect = true;
}

// ---------------------------------------------------------------------------
// The handshake
// ---------------------------------------------------------------------------

// Tell the unit we are here.  Both broadcast and unicast: a broadcast the
// unit misses means it never answers us.
void Client::announce() {
  _announceCalls++;
  if (!_server) return;
  uint8_t buf[160];
  size_t o = 0;
  memcpy(buf + o, "airD", 4); o += 4;
  memcpy(buf + o, _token, 16); o += 16;
  o += putString(buf + o, STAGELINQ_NAME);
  o += putString(buf + o, "DISCOVERER_HOWDY_");
  o += putString(buf + o, STAGELINQ_NAME);
  o += putString(buf + o, "1");
  put16(buf + o, _callbackPort); o += 2;

  _announceSent += _udp.broadcastTo(buf, o, STAGELINQ_DISCOVERY_PORT);
  IPAddress ip = (_overrideIp != IPAddress(0, 0, 0, 0)) ? _overrideIp : _unit.ip;
  if (ip != IPAddress(0, 0, 0, 0)) {
    _announceSent += _udp.writeTo(buf, o, ip, STAGELINQ_DISCOVERY_PORT);
  }
  _lastAnnounce = millis();
}

void Client::startMain() {
  closeAll();
  newToken();
  _haveBeatPort = _haveStatePort = false;
  _link = LINK_MAIN;

  // We do NOT announce here.  The announcement goes out once the
  // connection is OPEN - see openMain() - because the unit ties the
  // announcement to the connection, and a connection that arrives before
  // the announcement is a connection it never answers.

  _connIp = (_overrideIp != IPAddress(0, 0, 0, 0)) ? _overrideIp : _unit.ip;
  _connPort = _unit.port;
  if (_connIp == IPAddress(0, 0, 0, 0) || _connPort == 0) { _link = LINK_IDLE; return; }

  // ... and give it a MOMENT to hear us before we knock.  The unit
  // processes our announcement and our connection on the same task, and a
  // connection that arrives first is answered by a unit that has not
  // heard us yet - which is most of the time.
  _connectAt = millis() + 300;
  _lastRequest = 0;
  _requestAt = 0;
  _askedAgain = false;
}

void Client::openMain() {
  _main = new AsyncClient();
  if (!_main) { _link = LINK_BACKOFF; _backoffUntil = millis() + 2000; return; }
  _main->onConnect([](void* arg, AsyncClient* c) {
    Client* self = (Client*)arg;
    self->_main = c;
    // ANNOUNCE OURSELVES NOW, with the connection open.
    //
    // This is the order that works, and it is the order the unit expects:
    // open the connection, then announce, then ask.  The announcement
    // carries our token, and the unit ties it to the connection that is
    // already open - so an announcement that arrives BEFORE the
    // connection is an announcement about a client it has no connection
    // for, and it never answers.
    self->announce();
    // ... and give it a moment to hear us before we ask.
    self->_requestAt = millis() + 300;
  }, this);
  _main->onData([](void* arg, AsyncClient* c, void* d, size_t n) {
    (void)c;
    ((Client*)arg)->onMainData((const uint8_t*)d, n);
  }, this);
  _main->onDisconnect([](void* arg, AsyncClient* c) {
    (void)c;
    ((Client*)arg)->onMainGone();
  }, this);
  _main->onError([](void* arg, AsyncClient* c, int8_t e) {
    (void)c; (void)e;
    ((Client*)arg)->onMainGone();
  }, this);
  _main->connect(_connIp, _connPort);
}

void Client::openServices() {
  if (!_haveBeatPort && !_haveStatePort) return;
  _link = LINK_OPEN;
  _beatUp = _stateUp = false;

  if (_haveBeatPort) {
    _beat = new AsyncClient();
    _beat->onConnect([](void* arg, AsyncClient* c) {
      Client* self = (Client*)arg;
      self->_beat = c;
      self->_beatUp = true;
      self->sendServiceAnnounce(c, "BeatInfo");
      // Ask for the beat stream.
      uint8_t buf[8];
      put32(buf, 4);
      put32(buf + 4, BEAT_START);
      c->write((const char*)buf, sizeof(buf));
    }, this);
    _beat->onData([](void* arg, AsyncClient* c, void* d, size_t n) {
      (void)c;
      ((Client*)arg)->onBeatData((const uint8_t*)d, n);
    }, this);
    _beat->onDisconnect([](void* arg, AsyncClient* c) {
      (void)c;
      ((Client*)arg)->onBeatGone();
    }, this);
    _beat->onError([](void* arg, AsyncClient* c, int8_t e) {
      (void)c; (void)e;
      ((Client*)arg)->onBeatGone();
    }, this);
    _beat->connect(_connIp, _beatPort);
  }

  if (_haveStatePort) {
    _state = new AsyncClient();
    _state->onConnect([](void* arg, AsyncClient* c) {
      Client* self = (Client*)arg;
      self->_state = c;
      self->_stateUp = true;
      self->sendServiceAnnounce(c, "StateMap");
      // and ask for the states we want
      self->subscribeAll();
    }, this);
    _state->onData([](void* arg, AsyncClient* c, void* d, size_t n) {
      (void)c;
      ((Client*)arg)->onStateData((const uint8_t*)d, n);
    }, this);
    _state->onDisconnect([](void* arg, AsyncClient* c) {
      (void)c;
      ((Client*)arg)->onStateGone();
    }, this);
    _state->onError([](void* arg, AsyncClient* c, int8_t e) {
      (void)c; (void)e;
      ((Client*)arg)->onStateGone();
    }, this);
    _state->connect(_connIp, _statePort);
  }
}

// SAY HELLO BACK.
//
// The unit sends US a message of its own every second or so - the same
// shape as this one - and that message is how it knows the client is still
// there.  A client that only listens is a client that goes quiet, and the
// unit drops it after a while.  So we answer, every quarter of a second.
//
// This is the message the reference implementation calls a "reference": the
// id is 1, the first token is ours, the second is zeros, and the last eight
// bytes are a counter.
void Client::sendReference() {
  if (!_main || !_main->connected()) return;
  uint8_t buf[44];
  put32(buf, MSG_TIMESTAMP);
  memcpy(buf + 4, _token, 16);
  memset(buf + 20, 0, 16);
  put64(buf + 36, _reference++);
  if (_main->space() < 64) return;
  _main->write((const char*)buf, sizeof(buf));
}

// Ask the unit for its service list.
void Client::sendServicesRequest(AsyncClient* c) {
  if (!c || !c->connected()) return;
  uint8_t buf[20];
  put32(buf, MSG_SERVICES_REQUEST);
  memcpy(buf + 4, _token, 16);
  c->write((const char*)buf, sizeof(buf));
}

// Announce ourselves on a service connection.
bool Client::sendServiceAnnounce(AsyncClient* c, const char* name) {
  if (!c) return false;
  uint8_t buf[64];
  size_t o = 0;
  put32(buf + o, MSG_SERVICE_ANNOUNCEMENT); o += 4;
  memcpy(buf + o, _token, 16); o += 16;
  o += putString(buf + o, name);
  put16(buf + o, _callbackPort); o += 2;
  c->write((const char*)buf, o);
  return true;
}

void Client::subscribe(const char* path) {
  if (!_state) return;
  // The frame is a length, then the body - and the length counts every
  // byte of the body, magic and all.
  uint8_t body[160];
  size_t o = 0;
  memcpy(body + o, "smaa", 4); o += 4;
  put32(body + o, SMAA_SUBSCRIBE); o += 4;
  o += putString(body + o, path);
  put32(body + o, 0); o += 4;              // the interval
  uint8_t frame[164];
  put32(frame, (uint32_t)o);
  memcpy(frame + 4, body, o);
  _state->write((const char*)frame, o + 4);
}

// What we ask the deck for.  The unit only sends the states that were
// asked for, so anything we want to read has to be asked for by name.
//
// The list is QUEUED, not sent: twenty-four frames will not fit in the
// socket's write buffer at once, and a frame that is only half written is a
// frame the unit cannot parse.  loop() sends them one at a time.
void Client::subscribeAll() {
  _subsCount = 0;
  _subsNext = 0;
  for (uint8_t d = 1; d <= STAGELINQ_DECKS; d++) {
    const char* names[] = { "Play", "DeckIsMaster", "CurrentBPM", "BeatNumber",
                            "Track/SongName", "Track/ArtistName" };
    for (const char* n : names) {
      if (_subsCount >= SUBS_MAX) return;
      snprintf(_subs[_subsCount], sizeof(_subs[0]), "/Engine/Deck%d/%s", d, n);
      _subsCount++;
    }
  }
}

// Send the next subscription, if there is room in the socket for it.
void Client::flushSubscribes() {
  if (!_state || _subsNext >= _subsCount) return;
  if (_state->space() < 192) return;     // room for the frame and then some
  subscribe(_subs[_subsNext]);
  _subsNext++;
}

void Client::closeAll() {
  if (_main) { _main->close(); delete _main; _main = nullptr; }
  if (_beat) { _beat->close(); delete _beat; _beat = nullptr; }
  if (_state) { _state->close(); delete _state; _state = nullptr; }
  _mainLen = _beatLen = _stateLen = 0;
  _beatUp = _stateUp = false;
}

void Client::onMainGone() {
  _mainDead = true;
  if (_link == LINK_RUN || _link == LINK_OPEN || _link == LINK_MAIN) {
    _link = LINK_BACKOFF;
    _backoffUntil = millis() + 2000;
  }
}

void Client::onBeatGone() {
  _beatDead = true;
  _beatUp = false;
  if (_link == LINK_RUN || _link == LINK_OPEN) { _link = LINK_BACKOFF; _backoffUntil = millis() + 2000; }
}

void Client::onStateGone() {
  _stateDead = true;
  _stateUp = false;
  if (_link == LINK_RUN || _link == LINK_OPEN) { _link = LINK_BACKOFF; _backoffUntil = millis() + 2000; }
}

// Reap the clients that died.  This runs from loop(), never from inside a
// client's own callback.
static void reapClient(AsyncClient*& c, bool& dead) {
  if (!c || !dead) return;
  dead = false;
  c->close();
  delete c;
  c = nullptr;
}

// ---------------------------------------------------------------------------
// The housekeeping
// ---------------------------------------------------------------------------

void Client::loop() {
  if (!_enabled) return;
  _loopCalls++;

  // Send the subscriptions we owe the unit, one frame at a time.
  flushSubscribes();

  // Something new announced itself.
  if (_wantConnect) {
    _wantConnect = false;
    if (_link == LINK_IDLE || _link == LINK_BACKOFF) startMain();
  }

  // Reap whatever died since the last time round.
  reapClient(_main, _mainDead);
  reapClient(_beat, _beatDead);
  reapClient(_state, _stateDead);

  switch (_link) {
    case LINK_IDLE:
      // Waiting for the unit to say hello.  It does so once a second.
      if (millis() >= _readyAt && _unit.known && millis() - _unit.seen < 5000) startMain();
      break;

    case LINK_MAIN:
      // Waiting for the service list.
      if (!_main) {
        if (millis() >= _connectAt) openMain();
      } else if (_main->connected() && _requestAt && millis() >= _requestAt) {
        // Now we may ask.
        _requestAt = 0;
        _lastRequest = millis();
        sendServicesRequest(_main);
      } else if (_main->connected() && millis() - _lastRequest > 1200) {
        // The unit answers about four times in five, and it answers a
        // TOKEN once.  So: nudge it, and if it still says nothing, start
        // over with a FRESH token - asking again with the same one is
        // asking a stranger.
        if (_askedAgain) {
          closeAll();
          _link = LINK_BACKOFF;
          _backoffUntil = millis() + 1500;
          _askedAgain = false;
        } else {
          _askedAgain = true;
          _lastRequest = millis();
          sendServicesRequest(_main);
        }
      }
      if (_haveBeatPort && _haveStatePort) openServices();
      break;

    case LINK_OPEN:
      if (_beatUp && _stateUp) _link = LINK_RUN;
      if (millis() - _lastReference > 250) {
        _lastReference = millis();
        sendReference();
      }
      // KEEP THE UNIT'S REGISTRATION ALIVE.
      //
      // The unit expires a client that stops announcing itself: measured,
      // a client that announced once and then went quiet was dropped after
      // three seconds, and one that re-announced every second was never
      // dropped at all.  It is the SAME token each time, so it is a refresh
      // and not a new registration - the session is left alone.
      if (millis() - _lastAnnounce > 1000) announce();
      break;

    case LINK_RUN:
      // Say hello back, so the unit knows we are still here.
      if (millis() - _lastReference > 250) {
        _lastReference = millis();
        sendReference();
      }
      // ... and keep our registration alive (see LINK_OPEN).
      if (millis() - _lastAnnounce > 1000) announce();
      break;

    case LINK_BACKOFF:
      if (millis() >= _backoffUntil) {
        if (_unit.known) startMain();
        else _link = LINK_IDLE;
      }
      break;
  }

  // The master deck can change at any time.
  pickDeck();
}

// Which deck are we following?  The one the unit calls the master, unless
// the user pinned one.
void Client::pickDeck() {
  if (_wantDeck) { _deck = _wantDeck; return; }
  for (uint8_t i = 0; i < STAGELINQ_DECKS; i++) {
    if (_states[i].haveMaster && _states[i].master) { _deck = i + 1; return; }
  }
  _deck = 1;
}

// ---------------------------------------------------------------------------
// The main connection: the service list
// ---------------------------------------------------------------------------

void Client::onMainData(const uint8_t* d, size_t n) {
  if (_mainLen + n > sizeof(_mainBuf)) { _mainLen = 0; return; }
  memcpy(_mainBuf + _mainLen, d, n);
  _mainLen += n;
  parseMain(_mainBuf, _mainLen);
}

void Client::parseMain(const uint8_t* buf, size_t len) {
  size_t o = 0;
  while (len - o >= 4) {
    uint32_t id = be32(buf + o);
    if (id == MSG_SERVICE_ANNOUNCEMENT) {
      if (len - o < 4 + 16 + 4) break;
      uint32_t nlen = be32(buf + o + 20);
      if (nlen > 512) break;                 // a name this long is not a name
      size_t need = 4 + 16 + 4 + nlen + 2;
      if (len - o < need) break;
      const uint8_t* nm = buf + o + 24;
      uint16_t port = be16(buf + o + 24 + nlen);
      if (utf16eq(nm, nlen, "BeatInfo")) { _beatPort = port; _haveBeatPort = true; }
      else if (utf16eq(nm, nlen, "StateMap")) { _statePort = port; _haveStatePort = true; }
      o += need;
    } else if (id == MSG_TIMESTAMP) {
      size_t need = 4 + 16 + 16 + 8;
      if (len - o < need) break;
      o += need;
    } else if (id == MSG_SERVICES_REQUEST) {
      // The unit asks what WE offer.  We offer the services, not a source.
      size_t need = 4 + 16;
      if (len - o < need) break;
      o += need;
    } else {
      break;   // unknown: the stream is out of step, so drop it
    }
  }
  if (o) { memmove(_mainBuf, _mainBuf + o, len - o); _mainLen = len - o; }
}

// ---------------------------------------------------------------------------
// The BeatInfo service: the beat
// ---------------------------------------------------------------------------

void Client::onBeatData(const uint8_t* d, size_t n) {
  _beatBytes += (uint32_t)n;
  if (_beatLen + n > sizeof(_beatBuf)) { _beatLen = 0; return; }
  memcpy(_beatBuf + _beatLen, d, n);
  _beatLen += n;
  parseBeat(_beatBuf, _beatLen);
}

void Client::parseBeat(const uint8_t* buf, size_t len) {
  size_t o = 0;
  while (len - o >= 4) {
    uint32_t blen = be32(buf + o);
    if (blen < 4 || blen > sizeof(_beatBuf) - 4) { o += 4; continue; }  // out of step
    if (len - o < 4 + blen) break;
    const uint8_t* p = buf + o + 4;
    if (be32(p) == BEAT_EMIT) {
      // clock, the deck count, then 24 bytes per deck
      if (blen >= 16) {
        uint32_t decks = be32(p + 12);
        size_t need = 16 + (size_t)decks * 24;
        if (decks <= STAGELINQ_DECKS && blen >= need) {
          for (uint32_t i = 0; i < decks; i++) {
            const uint8_t* d3 = p + 16 + i * 24;
            DeckBeat& b = _beats[i];
            b.beat       = bef64(d3);
            b.totalBeats = bef64(d3 + 8);
            b.bpm        = bef64(d3 + 16);
            b.samples    = 0.0;
            // the optional sample positions follow the deck records
            if (blen >= need + 8 * (i + 1)) b.samples = bef64(p + need + i * 8);
            b.valid = true;
            b.at = millis();
            _beatFrames++;
          }
        }
      }
    }
    o += 4 + blen;
  }
  if (o) { memmove(_beatBuf, _beatBuf + o, len - o); _beatLen = len - o; }
}

// ---------------------------------------------------------------------------
// The StateMap service: the deck's own state
// ---------------------------------------------------------------------------

void Client::onStateData(const uint8_t* d, size_t n) {
  _stateBytes += (uint32_t)n;
  if (_stateLen + n > sizeof(_stateBuf)) { _stateLen = 0; return; }
  memcpy(_stateBuf + _stateLen, d, n);
  _stateLen += n;
  parseState(_stateBuf, _stateLen);
}

void Client::parseState(const uint8_t* buf, size_t len) {
  size_t o = 0;
  while (len - o >= 4) {
    uint32_t blen = be32(buf + o);
    if (blen < 8 || blen > sizeof(_stateBuf) - 4) { o += 4; continue; }
    if (len - o < 4 + blen) break;
    const uint8_t* p = buf + o + 4;
    if (memcmp(p, "smaa", 4) == 0 && be32(p + 4) == SMAA_EMIT) {
      // the name, then the JSON
      if (blen >= 12) {
        uint32_t nlen = be32(p + 8);
        if (nlen < 1024 && blen >= 12 + nlen + 4) {
          const uint8_t* nm = p + 12;
          const uint8_t* jp = nm + nlen;
          uint32_t jlen = be32(jp);
          if (jlen < 4096 && 12 + nlen + 4 + jlen <= blen) {
            char path[64];
            utf16toAscii(nm, nlen, path, sizeof(path));
            char json[192];
            utf16toAscii(jp + 4, jlen, json, sizeof(json));
            applyState(path, json);
          }
        }
      }
    }
    o += 4 + blen;
  }
  if (o) { memmove(_stateBuf, _stateBuf + o, len - o); _stateLen = len - o; }
}

// The paths we asked for, and what they mean.
void Client::applyState(const char* path, const char* json) {
  // "/Engine/Deck{N}/..."
  if (strncmp(path, "/Engine/Deck", 12) != 0) return;
  const char* p = path + 12;
  if (*p < '1' || *p > '4') return;
  uint8_t deck = (uint8_t)(*p - '1');
  p++;
  if (*p != '/') return;
  p++;
  DeckState& s = _states[deck];

  if (strcmp(p, "Play") == 0) {
    jsonBool(json, "state", &s.play);
  } else if (strcmp(p, "DeckIsMaster") == 0) {
    s.haveMaster = jsonBool(json, "state", &s.master);
  } else if (strcmp(p, "CurrentBPM") == 0) {
    jsonNum(json, "value", &s.bpm);
  } else if (strcmp(p, "BeatNumber") == 0) {
    s.haveBeatNumber = jsonNum(json, "value", &s.beatNumber);
  } else if (strcmp(p, "Track/SongName") == 0) {
    jsonStr(json, "string", s.song, sizeof(s.song));
  } else if (strcmp(p, "Track/ArtistName") == 0) {
    jsonStr(json, "string", s.artist, sizeof(s.artist));
  }
}

} // namespace Stagelinq
