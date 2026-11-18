/*
 * usermod_v3_c5remote.cpp - see the header.
 *
 * NOT a standalone translation unit: included at the end of
 * usermod_v3_c5remote.h, which usermods_list.cpp includes.
 */

// ArduinoJson comes in with wled.h (src/dependencies/json/ArduinoJson-v6.h).

// ---------------------------------------------------------------------------
// The screen, in colours
// ---------------------------------------------------------------------------
static const uint16_t C5_BG        = 0x0000;   // black
static const uint16_t C5_PANEL_BG  = 0x0861;   // very dark blue
static const uint16_t C5_TITLE_BG  = 0x01E0;   // dark teal
static const uint16_t C5_FOOT_BG   = 0x1082;   // dark grey-blue
static const uint16_t C5_TEXT      = 0xFFFF;
static const uint16_t C5_DIM       = 0x8410;
static const uint16_t C5_ACCENT    = 0x07FF;   // cyan
static const uint16_t C5_GOOD      = 0x07E0;   // green
static const uint16_t C5_WARN      = 0xFFE0;   // yellow
static const uint16_t C5_BAD       = 0xF800;   // red

C5RemoteUsermod::C5RemoteUsermod() : Usermod("C5Remote", false) {}

// ---------------------------------------------------------------------------
// The HTTP client
// ---------------------------------------------------------------------------

// A GET. The body comes back in the string.
//
// There is no watchdog feed here.  The core watches the task that runs
// the main loop and feeds it there, and the watchdog on this board is
// set to a hundred seconds - a request that waits a second and a half
// for its answer cannot come anywhere near it.  Asking for the watchdog
// to be reset by a task that is not on it is an error, and it filled the
// log with "task not found".

// The filter that keeps only what this board wants out of the other
// board's answer.
//
// The AudioReactive document is ten KB - it carries the whole spectrum,
// the banks and the calibration.  All of it is parsed, and all of it is
// thrown away, three times a minute.  On a board with 327 KB of RAM that
// is a document that does not fit, and a heap that fragments.
//
// So the parse is told what to keep, and the rest is skipped as it is
// read: the tempo, and the few numbers beside it.  The document that
// holds the result is then a few hundred bytes, and it is a STATIC one -
// made once, at startup, and used for every poll.  Nothing is allocated
// and freed over and over, which is what had the heap shrinking.
//
// The filter is itself a document, and it says where in the answer the
// wanted numbers are.
static StaticJsonDocument<256> s_c5TempoFilter;

// The document the poll's answer goes into.  It is made once and kept:
// making it on every poll is what filled the heap with fragments.
static StaticJsonDocument<1024> s_c5TempoDoc;


static bool c5HttpGet(const char *url, String &out, int *statusOut) {
  out = "";
  if (statusOut) *statusOut = 0;

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 1500;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return false;

  bool ok = false;
  if (esp_http_client_open(client, 0) >= 0) {
    // The headers have to be fetched BEFORE the status code is asked
    // for: the status line is in the headers, so asking first always
    // answers zero.
    if (esp_http_client_fetch_headers(client) >= 0) {
      const int status = esp_http_client_get_status_code(client);
      if (statusOut) *statusOut = status;
      char buf[512];
      int len;
      while ((len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        out.concat(buf, (size_t)len);
      }
      ok = (status == 200);
    }
    esp_http_client_close(client);
  }
  esp_http_client_cleanup(client);
  return ok;
}

// A POST with a JSON body.
static bool c5HttpPost(const char *url, const char *body, int *statusOut) {
  if (statusOut) *statusOut = 0;

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 1500;
  cfg.buffer_size = 512;
  cfg.buffer_size_tx = 512;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return false;

  bool ok = false;
  // The body goes on before the request is sent, and the request is made
  // with perform(): the blocking open()+fetch_headers() pair never sends
  // the body at all, so the other board would get an empty one and
  // answer with a 400.
  const int len = (int)strlen(body);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, len);
  esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  if (statusOut) *statusOut = status;
  ok = (status == 200);
  esp_http_client_cleanup(client);
  return ok;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void C5RemoteUsermod::setup() {
  // What to keep out of the other board's answer.  It says where the
  // wanted numbers are, so the parse can skip everything else.
  s_c5TempoFilter["AudioReactive"]["tempo"]["usable"]       = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["bpm"]          = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["beat_in_bar"]   = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["beat_progress"] = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["quality"]       = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["drift_ms"]      = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["hold"]          = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["downbeat_set"]  = true;
  s_c5TempoFilter["AudioReactive"]["tempo"]["mode_name"]     = true;

  // The panel's pins. They are taken so that nothing else in WLED hands
  // them to a bus.
  const managed_pin_type pins[] = {
    {C5_LCD_SCLK, false}, {C5_LCD_MOSI, false}, {C5_LCD_MISO, false},
    {C5_LCD_DC,    true},  {C5_LCD_CS,  true},  {C5_SD_CS,   true}
  };
  if (!pinManager.allocateMultiplePins(pins, 6, PinOwner::UM_Unspecified)) {
    USER_PRINTLN("C5: the panel's pins are taken - no panel");
    return;
  }

  // The I2C bus. The expander and the IMU are both on it.
  if (!pinManager.joinWire(C5_I2C_SDA, C5_I2C_SCL)) {
    USER_PRINTLN("C5: the I2C bus did not come up");
  }

  // The IO expander: the panel's reset and its backlight live there.
  _ioexpOk = _ioexp.begin(Wire);
  if (!_ioexpOk) {
    USER_PRINTLN("C5: no IO expander at 0x24 - the panel's reset and backlight are not there");
  }

  // The panel.
  if (_ioexpOk && _panel.begin(_ioexp)) {
    _panel.clear(C5_BG);
  }

  // The IMU. For the console for now.
  _imuOk = _imu.begin();

  // THE BOARD'S OWN NETWORK, when the config has none.
  //
  // The build can carry the credentials of the network this board belongs
  // to (CLIENT_SSID and CLIENT_PASS). A board whose config file was
  // written before those were in the build - or a board whose config was
  // wiped - comes up with no network at all, and the network it is MEANT
  // to be on is right here in the firmware. So it is filled in, and
  // remembered, so that the next boot does not have to think about it.
  //
  // A network that was set by hand is never touched: this only runs when
  // there is no network at all.
  if (!WLED_WIFI_CONFIGURED && strcmp(CLIENT_SSID, DEFAULT_CLIENT_SSID) != 0) {
    strlcpy(clientSSID, CLIENT_SSID, sizeof(clientSSID));
    strlcpy(clientPass, CLIENT_PASS, sizeof(clientPass));
    USER_PRINTF("C5: the board had no network of its own - it is %s now\n", clientSSID);
    serializeConfigSec();
  }

  // The HTTP client's own log is chatty, and every failed poll says three
  // lines about it. The failures are this usermod's business, and it says
  // them itself.
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
  esp_log_level_set("esp-tls", ESP_LOG_NONE);
  esp_log_level_set("transport_base", ESP_LOG_NONE);

  setupButtons();

#if defined(USERMOD_ZIGBEE)
  // The radio. It is started last: it wants the flash's Zigbee partitions
  // to be there, and the board's own setup first.
  setupZigbee();
#endif

  // The screen, over HTTP: /c5screen.bmp is what the panel is showing,
  // as a picture.  It is the only way to see the screen without looking
  // at the screen.
  //
  // It comes in TILES: "row=<n>" is 40 rows of the picture, and the
  // whole screen is eight of them.  A quarter of a megabyte in one request
  // is more than this board's Wi-Fi likes - the driver gives up in the
  // middle of it - and a tile goes over without trouble.
  //
  // The picture is built in one buffer that is made once and kept: the
  // web server reads it while it sends it, so it has to stay where it is.
  _bmp = (uint8_t *)heap_caps_malloc(54 + (size_t)C5_PANEL_W * C5_TILE_ROWS * 3,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  server.on("/c5screen.bmp", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!_panel.ready() || _bmp == nullptr) {
      request->send(503, "text/plain", "no panel");
      return;
    }
    // Which tile? 40 rows at a time, from the top.
    int row = 0;
    if (request->hasParam("row")) row = request->getParam("row")->value().toInt();
    if (row < 0) row = 0;
    if (row > C5_PANEL_H - C5_TILE_ROWS) row = C5_PANEL_H - C5_TILE_ROWS;

    // A BMP is the simplest picture there is: a 54 byte header and the
    // pixels, bottom row first.
    const size_t len = 54 + (size_t)C5_PANEL_W * C5_TILE_ROWS * 3;
    uint8_t *bmp = _bmp;
    const uint16_t *fb = _panel.framebuffer();
    // The file header: "BM", the file's size, and the offset of the
    // pixels.  Then the info header: the size, the width, the height,
    // the planes, the bits per pixel and the compression.
    //
    // The whole 54 bytes are written every time: the buffer is kept
    // between the requests, and a byte left over from the last one is a
    // byte that says something else.
    memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    uint32_t size = (uint32_t)len;
    bmp[2] = (uint8_t)(size & 0xFF);       bmp[3] = (uint8_t)((size >> 8) & 0xFF);
    bmp[4] = (uint8_t)((size >> 16) & 0xFF); bmp[5] = (uint8_t)((size >> 24) & 0xFF);
    bmp[10] = 54;                                       // where the pixels start
    bmp[14] = (uint8_t)(C5_PANEL_W & 0xFF); bmp[15] = (uint8_t)((C5_PANEL_W >> 8) & 0xFF);
    bmp[18] = (uint8_t)(C5_TILE_ROWS & 0xFF); bmp[19] = (uint8_t)((C5_TILE_ROWS >> 8) & 0xFF);
    bmp[22] = 1;                                         // the planes
    bmp[26] = 1;                                         // the planes
    bmp[28] = 16;                                        // the bits per pixel
    bmp[30] = 0; bmp[31] = 3;                             // the compression: none
    // The pixels, bottom row first, and in BGR.
    uint8_t *p = bmp + 54;
    for (int y = row + C5_TILE_ROWS - 1; y >= row; y--) {
      const uint16_t *r = fb + (size_t)y * C5_PANEL_W;
      for (int x = 0; x < C5_PANEL_W; x++) {
        // RGB565 to 8 bits per colour.  The green is the awkward one: its
        // six bits sit three in the high byte and three in the low one.
        uint16_t c = r[x];
        *p++ = (uint8_t)((c & 0x1F) << 3);                              // the blue
        *p++ = (uint8_t)(((c >> 3) & 0xE0) | ((c >> 5) & 0x1F));         // the green
        *p++ = (uint8_t)((c >> 8) & 0xF8);                              // the red
      }
    }
    request->send(request->beginResponse_P(200, "image/bmp", bmp, len));
  });

  USER_PRINTF("C5: the remote is up - the panel is %s, the expander is %s, the IMU is %s\n",
              _panel.ready() ? "there" : "not there",
              _ioexpOk ? "there" : "not there",
              _imuOk ? "there" : "not there");
}

// ---------------------------------------------------------------------------
// The buttons
// ---------------------------------------------------------------------------

void C5RemoteUsermod::setupButtons() {
  // The three on the chip. The boot button (GPIO28) is a button once the
  // chip has booted - it is the download button before that, and nothing
  // after it.
  struct { int8_t pin; uint16_t action; } chip[] = {
    {C5_BTN_PREV, C5_ACT_PAGE_PREV},
    {C5_BTN_NEXT, C5_ACT_PAGE_NEXT},
    {C5_BTN_BOOT, WLED_ACT_TEMPO_DOWNBEAT},
  };
  for (uint8_t i = 0; i < 3; i++) {
    btnPin[i] = chip[i].pin;
    buttonType[i] = BTN_TYPE_PUSH;
    macroButton[i] = chip[i].action;
    macroLongPress[i] = 0;
    macroDoublePress[i] = 0;
    pinMode(chip[i].pin, INPUT_PULLUP);
  }
  // The boot button does more than mark the downbeat: held down, it locks
  // and unlocks the clock on the board that has the tracker.
  macroLongPress[2] = WLED_ACT_TEMPO_TOGGLE_LOCK;
  // The other two: held down, they walk through the presets on that board.
  macroLongPress[0] = C5_ACT_PRESET_PREV;
  macroLongPress[1] = C5_ACT_PRESET_NEXT;

  // The expander's buttons.
  //
  // They are not on a GPIO of this chip.  The core skips a button whose
  // pin is negative, and a button that IS given a pin has that pin read -
  // so neither of those can be used for a button that lives on the
  // expander.  Giving them a pin that is never read also had the core
  // trying to allocate it, once for each of them, and complaining every
  // time.
  //
  // So they are not given to the core at all.  This usermod reads the
  // expander itself and does the debouncing, the long press and the
  // macros, which is what the core does for a button on a pin.  The
  // core's own buttons - the three on the chip - are untouched.
  for (uint8_t b = C5_EXPANDER_BTN_FIRST; b < WLED_MAX_BUTTONS; b++) {
    btnPin[b] = -1;
    buttonType[b] = BTN_TYPE_NONE;
    macroButton[b] = 0;
    macroLongPress[b] = 0;
    macroDoublePress[b] = 0;
  }
}

// The expander's buttons.
//
// The core asks every usermod whether it owns a button, and this one owns
// the ones on the expander: they are read from the expander's input
// register, not from a GPIO of this chip.
//
// The core's own logic - the debounce, the long press, the double press
// and the macros - is not used for them, because the core never gets
// that far with a button that has no pin.  So the same logic is done
// here, on the readings this usermod takes.
bool C5RemoteUsermod::handleButton(uint8_t index) {
  if (!_ioexpOk) return false;
  if (index < C5_EXPANDER_BTN_FIRST) return false;   // those are real pins
  const uint8_t io = C5_IOEXP_BTN_FIRST + (index - C5_EXPANDER_BTN_FIRST);
  if (io > C5_IOEXP_BTN_LAST) return false;

  // The reading that is believed is the one that has not moved: an
  // expander's input that floats would otherwise read as a button that
  // is being pressed and let go.
  const bool high = (_ioInputsStable & (uint16_t)(1U << io)) != 0;
  const bool pressed = _ioActiveHigh ? high : !high;

  // The state of each of the expander's buttons: when it was last seen
  // pressed, and whether it has already fired.
  static uint32_t pressedMs[C5_IOEXP_BTN_COUNT] = {0};
  static bool     wasPressed[C5_IOEXP_BTN_COUNT] = {false};
  static bool     fired[C5_IOEXP_BTN_COUNT] = {false};
  static bool     longFired[C5_IOEXP_BTN_COUNT] = {false};
  const uint8_t slot = io - C5_IOEXP_BTN_FIRST;
  const uint32_t now = millis();

  if (pressed && !wasPressed[slot]) {
    // Just pressed: remember when, and let go of whatever came before.
    pressedMs[slot] = now;
    wasPressed[slot] = true;
    fired[slot] = false;
    longFired[slot] = false;
  } else if (!pressed && wasPressed[slot]) {
    // Just let go.  A short press fires the button; a long one already
    // fired on its own, and fires nothing now.
    wasPressed[slot] = false;
    if (!fired[slot] && !longFired[slot]) {
      fired[slot] = true;
      if (_expanderAction[slot] != 0) performAction(_expanderAction[slot]);
    }
  } else if (pressed && !longFired[slot] && (now - pressedMs[slot]) >= C5_EXPANDER_LONG_MS) {
    // Held down: the long press fires once, when the hold is long enough.
    longFired[slot] = true;
    if (_expanderLongAction[slot] != 0) performAction(_expanderLongAction[slot]);
  }
  return true;   // the core leaves this button alone
}

// The band the radio is allowed to use.
//
// The ESP32-C5 is a dual-band chip and it comes up in the automatic mode:
// both bands, and the network's name decides which one.  The 5 GHz network
// is the one that matters here, and a board that has to be told which band
// to use has that in its configuration.
void C5RemoteUsermod::applyWifiBand() {
  wifi_band_mode_t mode = WIFI_BAND_MODE_AUTO;
  const char *name = "both bands";
  if (_wifiBand == 1) { mode = WIFI_BAND_MODE_5G_ONLY; name = "5 GHz only"; }
  else if (_wifiBand == 2) { mode = WIFI_BAND_MODE_2G_ONLY; name = "2.4 GHz only"; }
  // The radio has to be up before it can be told: the framework's setter
  // says so and does nothing otherwise.
  if (!WiFi.STA.started() && !WiFi.AP.started()) return;
  if (WiFi.setBandMode(mode)) {
    USER_PRINTF("C5: the radio is on %s\n", name);
  }
}

void C5RemoteUsermod::connected() {
  // The band can only be set once the radio is up, which is here.
  applyWifiBand();
}

void C5RemoteUsermod::pollExpander() {
  if (!_ioexpOk) return;
  uint16_t v = 0;
  if (!_ioexp.readInput(v)) return;
  if (v == _ioInputs) {
    if (_ioStableCount < 3) _ioStableCount++;
    if (_ioStableCount >= 3) _ioInputsStable = v;
  } else {
    _ioInputs = v;
    _ioStableCount = 0;
  }
}

// ---------------------------------------------------------------------------
// The actions
// ---------------------------------------------------------------------------

void C5RemoteUsermod::noteAction(const char *what) {
  _lastAction = what;
  _lastActionMs = millis();
  USER_PRINTF("C5: %s\n", what);
  _redraw = true;
}

// The action is NOT done here.
//
// This runs in whatever task asked for it, and the tempo's actions make an
// HTTP request to the other board - and when the request came in over the
// network, that request would be made from the web server's own task,
// inside the handling of a request.  The HTTP client wants the network
// task, which is this one, so it waits for an answer that cannot come
// until it returns: the board falls over.
//
// So the action is remembered and the main loop does it.
bool C5RemoteUsermod::handleButtonAction(uint16_t action) {
  _pendingAction = action;
  _pendingActionSet = true;
  return true;
}

bool C5RemoteUsermod::performAction(uint16_t action) {
  switch (action) {
    // ---- this board's own ----
    case C5_ACT_PAGE_PREV:
      _page = (_page + C5_PAGE_COUNT - 1) % C5_PAGE_COUNT;
      _redraw = true;
      return true;
    case C5_ACT_PAGE_NEXT:
      _page = (_page + 1) % C5_PAGE_COUNT;
      _redraw = true;
      return true;

    // ---- the board with the tracker ----
    case WLED_ACT_TEMPO_DOWNBEAT:
      if (trackerPost("setDownbeat", true)) noteAction("downbeat set");
      else noteAction("downbeat: the board did not answer");
      return true;
    case WLED_ACT_TEMPO_CLEAR_DOWNBEAT:
      if (trackerPost("clearDownbeat", true)) noteAction("downbeat cleared");
      else noteAction("clear downbeat: the board did not answer");
      return true;
    case WLED_ACT_TEMPO_LOCK:
      if (trackerPost("lockTempo", true)) noteAction("tempo locked");
      else noteAction("lock: the board did not answer");
      return true;
    case WLED_ACT_TEMPO_UNLOCK:
      if (trackerPost("unlockTempo", true)) noteAction("tempo unlocked");
      else noteAction("unlock: the board did not answer");
      return true;
    case WLED_ACT_TEMPO_TOGGLE_LOCK:
      if (trackerPost(_hold ? "unlockTempo" : "lockTempo", true)) {
        noteAction(_hold ? "tempo unlocked" : "tempo locked");
      } else {
        noteAction("lock: the board did not answer");
      }
      return true;
    case WLED_ACT_TEMPO_RELOCK:
      if (trackerPost("relockTempo", true)) noteAction("re-locking");
      else noteAction("re-lock: the board did not answer");
      return true;
    case WLED_ACT_TEMPO_SYNC:
      if (trackerPost("syncTempo", true)) noteAction("grid synced");
      else noteAction("sync: the board did not answer");
      return true;
    case WLED_ACT_TEMPO_MODE_DJ:
    case WLED_ACT_TEMPO_MODE_PLAYLIST: {
      const bool playlist = (action == WLED_ACT_TEMPO_MODE_PLAYLIST);
      char body[64];
      snprintf_P(body, sizeof(body), PSTR("{\"AudioReactive\":{\"%s\":true}}"),
                 playlist ? "playlistMode" : "djSetMode");
      if (trackerPostRaw(body)) noteAction(playlist ? "mode: playlist" : "mode: dj set");
      else noteAction("mode: the board did not answer");
      return true;
    }
    case C5_ACT_PRESET_NEXT:
      if (trackerPreset(_preset + 1)) noteAction("next preset");
      else noteAction("preset: the board did not answer");
      return true;
    case C5_ACT_PRESET_PREV:
      if (trackerPreset(_preset > 0 ? _preset - 1 : 0)) noteAction("previous preset");
      else noteAction("preset: the board did not answer");
      return true;
    case C5_ACT_BRIGHT_UP:
      if (trackerBrightness(min(255, _p4Brightness + 16))) noteAction("brighter");
      else noteAction("brightness: the board did not answer");
      return true;
    case C5_ACT_BRIGHT_DOWN:
      if (trackerBrightness(max(0, _p4Brightness - 16))) noteAction("dimmer");
      else noteAction("brightness: the board did not answer");
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// The link to the board that has the tracker
// ---------------------------------------------------------------------------

bool C5RemoteUsermod::trackerPost(const char *key, bool value) {
  char body[80];
  snprintf_P(body, sizeof(body), PSTR("{\"AudioReactive\":{\"%s\":%s}}"),
             key, value ? "true" : "false");
  return trackerPostRaw(body);
}

bool C5RemoteUsermod::trackerPostRaw(const char *body) {
  if (_p4Host.length() == 0) return false;
  const String url = "http://" + _p4Host + ":" + String(_p4Port) + "/json";
  int status = 0;
  USER_PRINTF("C5: asking the board: %s\n", body);
  bool ok = c5HttpPost(url.c_str(), body, &status);
  if (!ok) {
    // The other board's web server has one JSON buffer, and a poll that is
    // still being answered holds it.  So: wait a moment and try again.
    delay(400);
    ok = c5HttpPost(url.c_str(), body, &status);
  }
  USER_PRINTF("C5: the board answered %d\n", status);
  if (!ok) {
    _p4Online = false;
    USER_PRINTF("C5: the board did not take it (status %d)\n", status);
    return false;
  }
  _p4Online = true;
  return true;
}

bool C5RemoteUsermod::trackerPreset(int preset) {
  if (preset < 0) preset = 0;
  char body[48];
  snprintf_P(body, sizeof(body), PSTR("{\"preset\":%d}"), preset);
  if (!trackerPostRaw(body)) return false;
  _preset = preset;
  return true;
}

bool C5RemoteUsermod::trackerBrightness(int value) {
  char body[48];
  snprintf_P(body, sizeof(body), PSTR("{\"bri\":%d}"), value);
  if (!trackerPostRaw(body)) return false;
  _p4Brightness = value;
  return true;
}

void C5RemoteUsermod::pollTracker() {
  const uint32_t now = millis();
  // Two seconds is often enough for a beat to move on the screen, and it
  // is slow enough not to be a chore on a small chip.
  if (now - _lastPoll < 3000) return;
  _lastPoll = now;

  if (_p4Host.length() == 0) return;

  // There is no point asking before the network is up.
  if (!WLED_CONNECTED) {
    _p4Online = false;
    return;
  }

  const String url = "http://" + _p4Host + ":" + String(_p4Port) + "/json/ar";
  const String stateUrl = "http://" + _p4Host + ":" + String(_p4Port) + "/json/state";
  String body;
  int status = 0;
  if (!c5HttpGet(url.c_str(), body, &status)) {
    _p4Online = false;
    _p4Failures++;
    // Say it once, not every two seconds.
    if (_p4Failures == 1 || (_p4Failures % 30) == 0) {
      USER_PRINTF("C5: the board at %s did not answer (%d)\n", _p4Host.c_str(), status);
    }
    return;
  }
  _p4Online = true;
  _p4Failures = 0;

  // The document is about 10 KB, and only the tempo is wanted.  The filter
  // skips the rest as it is read, and the document that holds what is
  // kept is a static one - so the poll allocates nothing.
  s_c5TempoDoc.clear();
  if (deserializeJson(s_c5TempoDoc, body, DeserializationOption::Filter(s_c5TempoFilter))) {
    _p4Status = "the board's answer did not parse";
    return;
  }

  JsonObject ar = s_c5TempoDoc["AudioReactive"];
  if (ar.isNull()) {
    _p4Status = "the board has no AudioReactive";
    return;
  }
  JsonObject tempo = ar["tempo"];
  if (!tempo.isNull()) {
    _tempoUsable = tempo["usable"] | false;
    _bpm         = tempo["bpm"] | 0.0f;
    _beatInBar   = tempo["beat_in_bar"] | 0;
    _beatProgress= tempo["beat_progress"] | 0.0f;
    _quality     = tempo["quality"] | 0.0f;
    _driftMs     = tempo["drift_ms"] | 0.0f;
    _hold        = (tempo["hold"] | 0) != 0;
    _downbeatSet = (tempo["downbeat_set"] | false);
    _modeName    = tempo["mode_name"] | "";
  }

  // The state is a small document (a few hundred bytes) and it is where the
  // brightness lives - which the brightness actions need to know about.
  {
    String state;
    int st = 0;
    if (c5HttpGet(stateUrl.c_str(), state, &st)) {
      StaticJsonDocument<2048> sdoc;
      if (!deserializeJson(sdoc, state)) {
        _p4Brightness = sdoc["bri"] | _p4Brightness;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Who is sending
// ---------------------------------------------------------------------------

String C5RemoteUsermod::senderName(uint32_t ip) {
  // WLED's own node list already has the boards that broadcast themselves
  // (the notifier's node info packet carries the name and the address),
  // so the name is usually already here.
  for (NodesMap::iterator it = Nodes.begin(); it != Nodes.end(); ++it) {
    if ((uint32_t)it->second.ip == ip) {
      if (it->second.nodeName.length() > 0) return it->second.nodeName;
    }
  }
  IPAddress addr(ip);
  return addr.toString();
}

// ---------------------------------------------------------------------------
// The Zigbee radio
// ---------------------------------------------------------------------------
#if defined(USERMOD_ZIGBEE)

void C5RemoteUsermod::setupZigbee() {
  // The tempo: an analog input, so that Home Assistant has a number to
  // show.  The description says what the number is; there is no unit for
  // "beats per minute" in the Zigbee clusters, and a wrong unit would be
  // worse than none.
  c5ZigbeeTempo.addAnalogInput();
  c5ZigbeeTempo.setAnalogInputDescription("BPM");
  c5ZigbeeTempo.setAnalogInputMinMax(40.0f, 220.0f);
  c5ZigbeeTempo.setAnalogInputResolution(0.1f);

  // Motion: an occupancy sensor, which is the one thing the Zigbee
  // clusters model well.  The IMU is what notices it.
  c5ZigbeeMotion.setManufacturerAndModel("Troy", "WLED C5 remote");

  Zigbee.addEndpoint(&c5ZigbeeTempo);
  Zigbee.addEndpoint(&c5ZigbeeMotion);

  if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
    USER_PRINTLN("C5: the Zigbee stack did not start");
    return;
  }
  _zigbeeStarted = true;
  USER_PRINTLN("C5: the Zigbee stack is up - looking for a network");
}

void C5RemoteUsermod::pollZigbee() {
  if (!_zigbeeStarted) return;

  // The tempo, every few seconds.
  const uint32_t now = millis();
  if (now - _zigbeeLastTempo > 5000) {
    _zigbeeLastTempo = now;
    if (Zigbee.connected() && _tempoUsable) {
      c5ZigbeeTempo.setAnalogInput(_bpm);
      c5ZigbeeTempo.reportAnalogInput();
    }
  }

  // Motion, from the IMU: the board was moved.
  if (_imuOk) {
    float acc[3] = {0, 0, 0}, gyr[3] = {0, 0, 0};
    if (_imu.read(acc, gyr)) {
      const float mag = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
      // A board lying still reads about one g.  A tenth of a g away from
      // that is the board being touched.
      const bool moved = fabsf(mag - 9.81f) > 1.0f;
      if (moved != _zigbeeMotion) {
        _zigbeeMotion = moved;
        if (Zigbee.connected()) {
          c5ZigbeeMotion.setOccupancy(moved);
          c5ZigbeeMotion.report();
        }
      }
    }
  }
}

#endif  // USERMOD_ZIGBEE

// ---------------------------------------------------------------------------
// The pages
// ---------------------------------------------------------------------------

void C5RemoteUsermod::drawHeader(const char *title, const char *right) {
  _panel.fillRect(0, 0, C5_PANEL_W, 14, C5_TITLE_BG);
  _panel.drawText(2, 3, title, C5_ACCENT, C5_TITLE_BG);
  if (right && *right) _panel.drawTextRight(C5_PANEL_W - 2, 3, right, C5_TEXT, C5_TITLE_BG);
}

void C5RemoteUsermod::drawFooter(const char *left, const char *right) {
  const int y = C5_PANEL_H - 12;
  _panel.fillRect(0, y, C5_PANEL_W, 12, C5_FOOT_BG);
  if (left && *left) _panel.drawText(2, y + 2, left, C5_DIM, C5_FOOT_BG);
  if (right && *right) _panel.drawTextRight(C5_PANEL_W - 2, y + 2, right, C5_DIM, C5_FOOT_BG);
}

// The sixteen bands, as bars.
void C5RemoteUsermod::drawBars(const uint8_t *bands, int x, int y, int w, int h) {
  const int n = 16;
  const int bw = w / n;
  for (int i = 0; i < n; i++) {
    const int v = bands ? bands[i] : 0;
    const int bh = (v * h) / 255;
    const uint16_t c = c5_rgb(255 - (v / 3), v, 40);
    _panel.fillRect(x + i * bw, y + h - bh, bw - 1, bh, c);
  }
}

void C5RemoteUsermod::drawSyncPage() {
  char line[64];

  // The page is wiped first: what was on it is not necessarily on it
  // now (a sender that went quiet leaves its line behind otherwise).
  _panel.clear(C5_BG);

  // The sender table is written in the network's task and read here, so it
  // is read under its lock.
  bool locked = (arSyncSenderLock != nullptr) &&
                (xSemaphoreTake(arSyncSenderLock, pdMS_TO_TICKS(5)) == pdTRUE);
  const uint8_t count = arSyncSenderCount;
  const bool live = (count > 0);

  snprintf_P(line, sizeof(line), PSTR("SYNC %s"), live ? "live" : "quiet");
  drawHeader(line, live ? "" : "no packets");

  // The senders.
  int y = 18;
  for (uint8_t i = 0; i < count && i < 4; i++) {
    const ArSyncSender &s = arSyncSenders[i];
    const uint32_t age = s.lastMs ? (millis() - s.lastMs) : 0;
    const String name = senderName(s.ip);

    // The name, or the address when the name is not known yet.
    _panel.drawText(2, y, name.c_str(), C5_TEXT, C5_BG);
    snprintf_P(line, sizeof(line), PSTR("%lu/s  %lums ago"), (unsigned long)s.rate, (unsigned long)age);
    _panel.drawTextRight(C5_PANEL_W - 2, y, line, age < 2500 ? C5_GOOD : C5_BAD, C5_BG);
    y += 10;

    // The address and the count, in a dimmer colour.
    IPAddress addr(s.ip);
    snprintf_P(line, sizeof(line), PSTR("%s  %lu pkts"), addr.toString().c_str(), (unsigned long)s.packets);
    _panel.drawText(2, y, line, C5_DIM, C5_BG);
    y += 12;
  }

  if (count == 0) {
    _panel.drawText(2, y, "waiting for a board to send", C5_DIM, C5_BG);
    _panel.drawText(2, y + 12, "the audio sync is on 239.0.0.1:11988", C5_DIM, C5_BG);
    y += 28;
  }

  // The meter: the sixteen bands of the last packet, from whichever board
  // sent most recently.
  uint8_t bands[16] = {0};
  uint8_t level = 0;
  bool haveBands = false;
  uint32_t newest = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (arSyncSenders[i].lastMs >= newest) {
      newest = arSyncSenders[i].lastMs;
      memcpy(bands, arSyncSenders[i].bands, sizeof(bands));
      level = arSyncSenders[i].level;
      haveBands = true;
    }
  }
  if (locked) xSemaphoreGive(arSyncSenderLock);

  const int meterTop = C5_PANEL_H - 12 - 96;
  _panel.fillRect(0, meterTop - 2, C5_PANEL_W, 98, C5_PANEL_BG);
  drawBars(haveBands ? bands : nullptr, 4, meterTop, C5_PANEL_W - 8, 92);

  // The level of the last packet.
  if (haveBands) {
    snprintf_P(line, sizeof(line), PSTR("level %d"), level);
    _panel.drawTextRight(C5_PANEL_W - 4, meterTop + 2, line, C5_DIM, C5_PANEL_BG);
  }

  // The footer: what the buttons do on this page.
  drawFooter("< prev   next >", "beats >");
}

void C5RemoteUsermod::drawBeatPage() {
  char line[64];

  // The page is wiped first: what was on it is not necessarily on it
  // now (a sender that went quiet leaves its line behind otherwise).
  _panel.clear(C5_BG);

  if (!_p4Online) {
    drawHeader("BEAT", "no board");
    _panel.drawText(2, 24, "the board with the tracker", C5_DIM, C5_BG);
    _panel.drawText(2, 34, "is not answering:", C5_DIM, C5_BG);
    _panel.drawText(2, 44, _p4Host.c_str(), C5_WARN, C5_BG);
    drawFooter("< prev   next >", "");
    return;
  }

  drawHeader("BEAT", _hold ? "LOCKED" : "free");

  // The rate.
  snprintf_P(line, sizeof(line), PSTR("%.1f BPM"), (double)_bpm);
  _panel.drawText(2, 20, line, _tempoUsable ? C5_GOOD : C5_DIM, C5_BG);

  // Where the beat is: the beat in the bar, and how far through it.
  snprintf_P(line, sizeof(line), PSTR("beat %d of 4"), _beatInBar);
  _panel.drawText(2, 34, line, C5_TEXT, C5_BG);
  snprintf_P(line, sizeof(line), PSTR("quality %.2f   drift %+.0f ms"), (double)_quality, (double)_driftMs);
  _panel.drawText(2, 44, line, C5_DIM, C5_BG);

  // The beat progress: a bar with the beat's place in it.
  const int barX = 2, barY = 58, barW = C5_PANEL_W - 4, barH = 16;
  _panel.fillRect(barX, barY, barW, barH, C5_PANEL_BG);
  const int beatW = (int)(_beatProgress * barW);
  _panel.fillRect(barX, barY, beatW, barH, C5_ACCENT);
  _panel.drawRect(barX, barY, barW, barH, C5_DIM);
  // The downbeat's mark, when there is one.
  if (_downbeatSet) _panel.fillRect(barX, barY, 2, barH, C5_WARN);

  // The mode, and whether the downbeat has been marked.
  snprintf_P(line, sizeof(line), PSTR("mode: %s"), _modeName.c_str());
  _panel.drawText(2, 80, line, C5_DIM, C5_BG);
  _panel.drawText(2, 90, _downbeatSet ? "downbeat: marked" : "downbeat: not marked",
                 _downbeatSet ? C5_GOOD : C5_DIM, C5_BG);
  // What the board is showing: the preset and the brightness.
  snprintf_P(line, sizeof(line), PSTR("preset %d   bri %d"), _preset, _p4Brightness);
  _panel.drawText(2, 104, line, C5_DIM, C5_BG);

  // What the buttons do here.
  drawFooter("BOOT: downbeat", "hold: lock");
}

void C5RemoteUsermod::drawBoardPage() {
  char line[64];

  // The page is wiped first: what was on it is not necessarily on it
  // now (a sender that went quiet leaves its line behind otherwise).
  _panel.clear(C5_BG);

  drawHeader("BOARD", _p4Online ? "linked" : "alone");

  // The network.
  // The network it is on, and the address it has there.
  //
  // The address comes from WLED's own network object: the Arduino's
  // WiFi.localIP() answers 0.0.0.0 on this chip, and the C5's core does
  // not report the SSID at all, so the network it was given is used
  // instead.
  _panel.drawText(2, 20, "wifi:", C5_DIM, C5_BG);
  _panel.drawText(38, 20, clientSSID, C5_TEXT, C5_BG);
  _panel.drawText(2, 30, "ip:", C5_DIM, C5_BG);
  _panel.drawText(38, 30, Network.localIP().toString().c_str(), C5_TEXT, C5_BG);

  // The uptime and the memory.
  const uint32_t up = millis() / 1000;
  snprintf_P(line, sizeof(line), PSTR("up %lu:%02lu   heap %lu"),
             (unsigned long)(up / 60), (unsigned long)(up % 60),
             (unsigned long)ESP.getFreeHeap());
  _panel.drawText(2, 42, line, C5_DIM, C5_BG);

  // The IMU.
  float acc[3] = {0, 0, 0}, gyr[3] = {0, 0, 0};
  if (_imuOk && _imu.read(acc, gyr)) {
    snprintf_P(line, sizeof(line), PSTR("acc %+.2f %+.2f %+.2f"), (double)acc[0], (double)acc[1], (double)acc[2]);
    _panel.drawText(2, 56, line, C5_DIM, C5_BG);
    snprintf_P(line, sizeof(line), PSTR("gyr %+.2f %+.2f %+.2f"), (double)gyr[0], (double)gyr[1], (double)gyr[2]);
    _panel.drawText(2, 66, line, C5_DIM, C5_BG);
  } else {
    _panel.drawText(2, 56, "no IMU", C5_DIM, C5_BG);
  }

  // The expander's battery reading and the sync counters.
  if (_ioexpOk) {
    snprintf_P(line, sizeof(line), PSTR("battery %u"), (unsigned)_ioexp.readAdc());
    _panel.drawText(2, 80, line, C5_DIM, C5_BG);
  }

  // The board with the tracker.
  _panel.drawText(2, 94, "board:", C5_DIM, C5_BG);
  _panel.drawText(38, 94, _p4Host.c_str(), _p4Online ? C5_TEXT : C5_DIM, C5_BG);

  // The last thing a button did.
  if (_lastAction.length() && (millis() - _lastActionMs) < 3000) {
    _panel.drawText(2, 110, _lastAction.c_str(), C5_ACCENT, C5_BG);
  }

  drawFooter("< prev   next >", "");
}

// ---------------------------------------------------------------------------
// The test page
// ---------------------------------------------------------------------------
//
// Colour bars, a grey ramp and some text.
//
// This is here because a panel that is not working shows as a flat grey
// with the backlight on, and that is indistinguishable from a panel that
// IS working and is showing grey.  With this page, one look answers the
// question: if the bars are there, the panel is being driven; if it is
// still flat grey, the panel never got its picture.

void C5RemoteUsermod::drawTestPage() {
  _panel.clear(C5_COL_BLACK);

  // The colour bars: red, green, blue, white, black, cyan, magenta, yellow.
  static const uint16_t bars[] = {
    C5_COL_RED, C5_COL_GREEN, C5_COL_BLUE, C5_COL_WHITE,
    C5_COL_BLACK, C5_COL_CYAN, C5_COL_MAGENTA, C5_COL_YELLOW
  };
  const int barW = C5_PANEL_W / 8;
  for (int i = 0; i < 8; i++) {
    _panel.fillRect(i * barW, 0, barW, 160, bars[i]);
  }

  // The grey ramp: sixteen steps from black to white.
  for (int i = 0; i < 16; i++) {
    const uint8_t g = (uint8_t)(i * 17);
    _panel.fillRect(i * 20, 160, 20, 40, c5_rgb(g, g, g));
  }

  // Some text, so the font is on the test too.
  _panel.drawText(4, 206, "C5 PANEL TEST", C5_COL_WHITE, C5_COL_BLACK);
  _panel.drawText(4, 220, "if you can read this,", C5_COL_GREY, C5_COL_BLACK);
  _panel.drawText(4, 232, "the panel works.", C5_COL_GREY, C5_COL_BLACK);

  // And a solid block of the background colour, which is what the other
  // pages are mostly made of.
  _panel.fillRect(0, 250, C5_PANEL_W, 70, C5_COL_DARKGREY);
  _panel.drawText(4, 262, "SYNC  BEAT  BOARD", C5_COL_WHITE, C5_COL_DARKGREY);
  _panel.drawText(4, 278, "next >", C5_COL_YELLOW, C5_COL_DARKGREY);
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
void C5RemoteUsermod::loop() {
  if (!_panel.ready()) return;

  const uint32_t now = millis();

  // The expander's buttons: read often enough to be quick, and rarely
  // enough not to be a chore.
  //
  // The interval is the debounce the core uses anyway (50 ms), and it is
  // also as slow as it can be: GPIO27 is the I2C SDA line AND a boot
  // strap on this chip, and the I2C master pulls it low while it talks.
  // A reset that lands in the middle of a transaction can strap the chip
  // low, and a chip strapped low cannot read its flash.  The less the
  // bus is used, the smaller that window is.
  static uint32_t lastExpander = 0;
  if (now - lastExpander >= 50) {
    lastExpander = now;
    pollExpander();
  }

#if defined(USERMOD_ZIGBEE)
  pollZigbee();
#endif

  // The IMU goes to the console, every few seconds, with the memory's
  // numbers beside it - this board has 327 KB of RAM and everything
  // counts.  The console is shared with the rest of WLED, and it is not
  // fast: a line a second fills it up.
  static uint32_t lastImu = 0;
  if (_imuOk && (now - lastImu >= 5000)) {
    lastImu = now;
    float acc[3] = {0, 0, 0}, gyr[3] = {0, 0, 0};
    if (_imu.read(acc, gyr)) {
      USER_PRINTF("C5: acc %+.2f %+.2f %+.2f m/s2   gyr %+.3f %+.3f %+.3f rad/s   heap %u\n",
                  (double)acc[0], (double)acc[1], (double)acc[2],
                  (double)gyr[0], (double)gyr[1], (double)gyr[2],
                  (unsigned)ESP.getFreeHeap());
    }
  }

  // An action that was asked for elsewhere.
  //
  // The action goes before the poll: both of them make a request, and two
  // requests in one turn of the loop is more waiting than the task
  // watchdog likes.
  if (_pendingActionSet) {
    _pendingActionSet = false;
    performAction(_pendingAction);
  } else {
    // The board that has the tracker.
    pollTracker();
  }

  // The screen. Ten times a second is plenty, and the live parts (the
  // meter, the beat) are worth a quicker look than that.
  if (_redraw || (now - _lastDraw >= 100)) {
    _lastDraw = now;
    _redraw = false;
    const int64_t drawStart = esp_timer_get_time();
    switch (_page) {
      case C5_PAGE_SYNC:  drawSyncPage(); break;
      case C5_PAGE_BEAT:  drawBeatPage(); break;
      case C5_PAGE_TEST:  drawTestPage(); break;
      default:            drawBoardPage(); break;
    }
    _drawMs = (uint32_t)((esp_timer_get_time() - drawStart) / 1000);
  }

  _panel.flush(25);

  // How long the screen takes to get its picture out, now and then.
  // The panel is the slow part of this board, so it is worth watching.
  static uint32_t lastPanelReport = 0;
  if (now - lastPanelReport >= 5000) {
    lastPanelReport = now;
    USER_PRINTF("C5: the screen: %u ms to draw, %u ms to send (%u bands), %u frames\n",
                (unsigned)_drawMs, (unsigned)_panel.lastFrameMs(),
                (unsigned)_panel.lastBandCount(), (unsigned)_panel.frameCount());
  }
}

// ---------------------------------------------------------------------------
// The JSON
// ---------------------------------------------------------------------------

void C5RemoteUsermod::addToJsonState(JsonObject &obj) {
  JsonObject me = obj.createNestedObject("C5Remote");
  me["page"]       = _page;
  me["host"]       = _p4Host;
  me["port"]       = _p4Port;
  me["online"]     = _p4Online;
  me["panel"]      = _panel.ready();
  me["expander"]   = _ioexpOk;
  me["imu"]        = _imuOk;
  me["expander_buttons_active_high"] = _ioActiveHigh;
}

void C5RemoteUsermod::readFromJsonState(JsonObject &obj) {
  JsonObject me = obj["C5Remote"];
  if (me.isNull()) return;
  if (me["host"].is<const char *>()) {
    const char *h = me["host"];
    if (h && strlen(h)) _p4Host = h;
  }
  if (me["port"].is<int>()) {
    const int p = me["port"];
    if (p > 0 && p < 65536) _p4Port = (uint16_t)p;
  }
  if (me["expander_buttons_active_high"].is<bool>()) {
    _ioActiveHigh = me["expander_buttons_active_high"];
  }
  if (me["wifi_band"].is<int>()) {
    const int b = me["wifi_band"];
    if (b >= 0 && b <= 2 && (uint8_t)b != _wifiBand) {
      _wifiBand = (uint8_t)b;
      applyWifiBand();
    }
  }
  if (me["page"].is<int>()) {
    const int p = me["page"];
    if (p >= 0 && p < C5_PAGE_COUNT) { _page = (uint8_t)p; _redraw = true; }
  }

  // An ACTION, by name.
  //
  // The same thing a button does, so that the remote can be asked to do it
  // over the network - and so that a test can press a button without
  // pressing one.
  if (me["action"].is<const char *>()) {
    const char *what = me["action"];
    if      (!strcmp(what, "downbeat")) handleButtonAction(WLED_ACT_TEMPO_DOWNBEAT);
    else if (!strcmp(what, "clear"))    handleButtonAction(WLED_ACT_TEMPO_CLEAR_DOWNBEAT);
    else if (!strcmp(what, "lock"))     handleButtonAction(WLED_ACT_TEMPO_LOCK);
    else if (!strcmp(what, "unlock"))   handleButtonAction(WLED_ACT_TEMPO_UNLOCK);
    else if (!strcmp(what, "toggle"))   handleButtonAction(WLED_ACT_TEMPO_TOGGLE_LOCK);
    else if (!strcmp(what, "relock"))   handleButtonAction(WLED_ACT_TEMPO_RELOCK);
    else if (!strcmp(what, "sync"))     handleButtonAction(WLED_ACT_TEMPO_SYNC);
    else if (!strcmp(what, "djset"))    handleButtonAction(WLED_ACT_TEMPO_MODE_DJ);
    else if (!strcmp(what, "playlist")) handleButtonAction(WLED_ACT_TEMPO_MODE_PLAYLIST);
    else if (!strcmp(what, "page"))     handleButtonAction(C5_ACT_PAGE_NEXT);
    else if (!strcmp(what, "preset"))   handleButtonAction(C5_ACT_PRESET_NEXT);
    else if (!strcmp(what, "bright"))   handleButtonAction(C5_ACT_BRIGHT_UP);
  }
}

void C5RemoteUsermod::addToJsonAr(JsonObject &obj) {
  JsonObject me = obj.createNestedObject("C5Remote");
  me["page"]    = _page;
  me["online"]  = _p4Online;
  me["host"]    = _p4Host;
  me["failures"]= (unsigned long)_p4Failures;
  // The tempo, as this board last heard it.
  JsonObject t = me.createNestedObject("tempo");
  t["bpm"]          = (double)_bpm;
  t["usable"]       = _tempoUsable;
  t["beat_in_bar"]  = _beatInBar;
  t["beat_progress"]= (double)_beatProgress;
  t["quality"]      = (double)_quality;
  t["drift_ms"]     = (double)_driftMs;
  t["hold"]         = _hold;
  t["downbeat_set"] = _downbeatSet;
  t["mode"]         = _modeName;
  // The board's own numbers.
  me["heap"]      = (unsigned long)ESP.getFreeHeap();
  me["uptime_ms"] = (unsigned long)millis();
  me["panel"]     = _panel.ready();
  me["expander"]  = _ioexpOk;
  me["imu"]       = _imuOk;
  me["expander_inputs"] = (unsigned)_ioInputsStable;
}

void C5RemoteUsermod::addToConfig(JsonObject &obj) {
  JsonObject top = obj.createNestedObject(FPSTR(_name));
  top[FPSTR("enabled")] = enabled;
  top[F("host")]   = _p4Host;
  top[F("port")]   = _p4Port;
  top[F("expander_buttons_active_high")] = _ioActiveHigh;
  top[F("wifi_band")] = _wifiBand;
}

bool C5RemoteUsermod::readFromConfig(JsonObject &obj) {
  JsonObject top = obj[FPSTR(_name)];
  if (top.isNull()) return false;
  bool ok = getJsonValue(top[FPSTR("enabled")], enabled);
  const char *host = top[F("host")] | "";
  if (host && strlen(host)) _p4Host = host;
  const int port = top[F("port")] | _p4Port;
  if (port > 0 && port < 65536) _p4Port = (uint16_t)port;
  getJsonValue(top[F("expander_buttons_active_high")], _ioActiveHigh);
  const int band = top[F("wifi_band")] | (int)_wifiBand;
  if (band >= 0 && band <= 2) _wifiBand = (uint8_t)band;
  return ok;
}

void C5RemoteUsermod::appendConfigData() {
  // The settings page builds its own fields from the config, so there is
  // nothing to add here. The entries in addToConfig() (the address of the
  // board with the tracker, its port, and which way round the expander's
  // buttons read) show up by themselves.
}
