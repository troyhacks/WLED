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
// The meter's place: the bottom of the SYNC page.  A page that has no
// meter has to cover it.
#define C5_METER_TOP (C5_PANEL_H - 12 - 98)
#define C5_METER_H   98
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

  // How long each step takes.  The setup is a fifth of the time the
  // board takes to come up, and it is worth knowing which part of it is
  // the time.
  uint32_t tStep = millis();
  #define C5_STEP(name) do { const uint32_t now = millis(); \
      USER_PRINTF("C5: setup: %s took %u ms", name, (unsigned)(now - tStep)); \
      USER_PRINTLN(""); tStep = now; } while (0)

  // The radio's band, as soon as the radio is up.
  //
  // The band is what makes the connection quick - the C5 is a dual-band
  // chip and the network it is after is a 5 GHz one - and it has to be
  // set BEFORE the scan that looks for the network.
  //
  // It cannot be set here: the radio comes up after the usermods' setup
  // (the log says so: "Starting WiFi STA mode" is after this).  So it
  // is set when the radio says it has started, which is before the
  // scan.
  // The radio's band.
  //
  // The band is what makes the connection quick - the C5 is a dual-band
  // chip and the network it is after is a 5 GHz one - and it has to be
  // set BEFORE the scan that looks for the network.
  //
  // It cannot be set here: the radio comes up after the usermods' setup
  // (the log says so: "Starting WiFi STA mode" is after this).  So the
  // loop tries, until the radio is there to be told.  The loop runs
  // before the radio starts, so the band is set before the scan.
  _bandSet = false;

  // The IO expander: the panel's reset and its backlight live there.
  _ioexpOk = _ioexp.begin(Wire);
  C5_STEP("the expander");
  if (!_ioexpOk) {
    USER_PRINTLN("C5: no IO expander at 0x24 - the panel's reset and backlight are not there");
  }

  // The panel.
  if (_ioexpOk && _panel.begin(_ioexp)) {
    _panel.clear(C5_BG);
  }
  C5_STEP("the panel");

  // The TF card.
  //
  // It is on the panel's SPI bus, so it is mounted NOW: the bus is up
  // (the panel's bring-up made it) and the panel is not sending anything
  // yet - the first picture goes out in the loop, after this.
  //
  // It is not touched again: the card and the panel cannot talk at the
  // same time, and the panel is what this board is for.
  _sd.begin();
  C5_STEP("the TF card");

  // The IMU. In a task of its own: the driver's calibration waits two
  // and a quarter seconds for the chip to finish, and the radio is
  // waiting for the network for longer than that - so the two waits
  // overlap instead of adding up.
  _imuOk = _imu.beginAsync();
  C5_STEP("the IMU");

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
  C5_STEP("the buttons");

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

  // A REBOOT, over the network.
  //
  // How long the board takes to come up is worth measuring, and the only
  // way to measure it is to start it from nothing - so the board has to
  // be able to be started from nothing.
  //
  // The answer goes first and the reboot happens in the loop, so the
  // request is answered and the board is down before the caller is
  // waiting for it.
  server.on("/c5reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "rebooting");
    _rebootAsked = true;
  });

  // How long the setup took: the app's start to here.  The web server
  // starts after this, so this is when the board is nearly up.
  _setupMs = millis();
  USER_PRINTF("C5: the setup took %u ms\n", (unsigned)_setupMs);

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

// The buttons.
//
// The core asks every usermod whether it owns a button.
//
// The three on the chip are the core's: it reads the pins, it debounces
// them and it does the macros.  But on the button debug page they are the
// ones being watched, and a "next" that left the page would take the
// press with it - so the core leaves them alone there.
//
// The eleven on the expander are never even asked about: the core skips a
// button whose pin is negative BEFORE it asks the usermods, and a pin is
// what the expander's buttons do not have.  They are read in
// pollButtons() instead, and their debounce, their long press and their
// macros are done there.
bool C5RemoteUsermod::handleButton(uint8_t index) {
  if (index >= C5_BTN_CHIP_COUNT) return false;   // the expander's are ours
  // One of the three on the chip.
  return (_page == C5_PAGE_BUTTONS);
}

// The buttons, read.
//
// The three on the chip are read here as well as by the core: the core
// does the actions, and this is what says what was pressed.
//
// The eleven on the IO expander are read here alone.  They are IO4..IO14
// on the header, so they are not buttons until something is wired to
// them - but they are watched all the same, because a button that is not
// seen is a button that cannot be debugged.
void C5RemoteUsermod::pollButtons() {
  const uint32_t now = millis();

  // The three on the chip: KEY_L is GPIO2, KEY_R is GPIO3 and BOOT is
  // GPIO28 - the board's schematic says so.  All three are pulled up with
  // 10K and pull the pin down when they are pressed.
  static const uint8_t chipPins[C5_BTN_CHIP_COUNT] = {
    C5_BTN_PREV, C5_BTN_NEXT, C5_BTN_BOOT
  };
  for (uint8_t i = 0; i < C5_BTN_CHIP_COUNT; i++) {
    noteButton(i, digitalRead(chipPins[i]) == LOW, now);
  }

  // The expander's - but only once its answer has been believed.  Until
  // then its input register reads as zero, and zero is every button
  // pressed (they are pulled up, and a press pulls them down).
  if (!_ioexpOk || !_ioPrimed) return;
  for (uint8_t io = C5_IOEXP_BTN_FIRST; io <= C5_IOEXP_BTN_LAST; io++) {
    const bool high = (_ioInputsStable & (uint16_t)(1U << io)) != 0;
    const bool pressed = _ioActiveHigh ? high : !high;
    noteButton((uint8_t)(C5_BTN_CHIP_COUNT + io - C5_IOEXP_BTN_FIRST), pressed, now);
  }
}

// One button, and what it is doing.
//
// The expander's buttons do their actions here: a short press fires when
// the button comes up, and a long press fires when it has been held down
// for long enough.  The three on the chip are the core's, and the core
// does those.
void C5RemoteUsermod::noteButton(uint8_t id, bool pressed, uint32_t now) {
  const bool was = _btnDown[id];
  if (pressed && !was) {
    // Just pressed.
    _btnDown[id] = true;
    _btnDownSince[id] = now;
    _btnHeldFired[id] = false;
    _btnPresses++;
    logButton(id, 0, 0, now);
  } else if (!pressed && was) {
    // Just let go.  A short press fires the action now; a long one
    // already fired on its own, and fires nothing now.
    _btnDown[id] = false;
    const uint32_t held = now - _btnDownSince[id];
    logButton(id, 1, held, now);
    if (id >= C5_BTN_CHIP_COUNT) {
      const uint8_t slot = (uint8_t)(id - C5_BTN_CHIP_COUNT);
      if (!_btnHeldFired[id] && _expanderAction[slot] != 0) {
        handleButtonAction(_expanderAction[slot]);
      }
    }
    // The debug page is left with BOOT, and on the way UP: the core never
    // saw the press (it was captured while the page was up), so it must
    // not see the release either - or leaving the page would mark the
    // downbeat on the other board.
    if (_page == C5_PAGE_BUTTONS && id == 2) {
      _page = _pageBeforeDebug;
      _redraw = true;
    }
  } else if (pressed && !_btnHeldFired[id] &&
             (now - _btnDownSince[id]) >= C5_EXPANDER_LONG_MS) {
    // Held down: the long press fires once, when the hold is long enough.
    _btnHeldFired[id] = true;
    const uint32_t held = now - _btnDownSince[id];
    logButton(id, 2, held, now);
    if (id >= C5_BTN_CHIP_COUNT) {
      const uint8_t slot = (uint8_t)(id - C5_BTN_CHIP_COUNT);
      if (_expanderLongAction[slot] != 0) {
        handleButtonAction(_expanderLongAction[slot]);
      }
    }
  }
}

// One event, into the log.
void C5RemoteUsermod::logButton(uint8_t id, uint8_t kind, uint32_t heldMs, uint32_t now) {
  C5BtnEvent &e = _btnLog[_btnLogHead];
  e.id = id;
  e.kind = kind;
  e.heldMs = heldMs;
  e.ms = now;
  _btnLogHead = (uint8_t)((_btnLogHead + 1) % C5_BTN_LOG_LEN);
  if (_btnLogCount < C5_BTN_LOG_LEN) _btnLogCount++;
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
  // The radio has to be up before it can be told: the driver has to be
  // there.  This is the IDF's own call rather than the framework's,
  // because the framework's setter wants the STA to be started and it
  // is not - the STA's mode is set later, and the scan that looks for
  // the network goes with it.
  if (esp_wifi_set_band_mode(mode) != ESP_OK) return;
  wifi_band_mode_t now = WIFI_BAND_MODE_AUTO;
  if (esp_wifi_get_band_mode(&now) == ESP_OK && now != mode) return;
  USER_PRINTF("C5: the radio is on %s\n", name);
  _bandSet = true;
}

void C5RemoteUsermod::connected() {
  // How long the radio took: the app's start to here.
  if (_wifiMs == 0) {
    _wifiMs = millis();
    USER_PRINTF("C5: the Wi-Fi is up: %u ms after the app started\n", (unsigned)_wifiMs);
  }
  // The band can only be set once the radio is up, which is here.
  applyWifiBand();
}

void C5RemoteUsermod::pollExpander() {
  if (!_ioexpOk) return;
  uint16_t v = 0;
  if (!_ioexp.readInput(v)) return;
  if (v == _ioInputs) {
    if (_ioStableCount < 3) _ioStableCount++;
    if (_ioStableCount >= 3) {
      // The first answer is the one that says what the buttons are doing.
      if (!_ioPrimed) {
        _ioPrimed = true;
        primeExpanderButtons(v);
      }
      _ioInputsStable = v;
    }
  } else {
    _ioInputs = v;
    _ioStableCount = 0;
  }
}

// The expander's first answer.
//
// Until the expander has answered, its input register reads as zero - and
// zero means every button is pressed, because they are pulled up and a
// press pulls them down.  So the first answer sets the buttons' states
// without saying anything about them: a board that has just come up has
// not had eleven button presses.
void C5RemoteUsermod::primeExpanderButtons(uint16_t inputs) {
  const uint32_t now = millis();
  for (uint8_t io = C5_IOEXP_BTN_FIRST; io <= C5_IOEXP_BTN_LAST; io++) {
    const bool high = (inputs & (uint16_t)(1U << io)) != 0;
    const bool pressed = _ioActiveHigh ? high : !high;
    const uint8_t id = (uint8_t)(C5_BTN_CHIP_COUNT + io - C5_IOEXP_BTN_FIRST);
    _btnDown[id] = pressed;
    _btnDownSince[id] = now;
    _btnHeldFired[id] = false;
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

// The page, and where the debug page came from.
//
// The debug page is left with BOOT, and BOOT puts it back where it was:
// the page before it is remembered when it is entered.
void C5RemoteUsermod::setPage(uint8_t p) {
  if (p >= C5_PAGE_COUNT || p == _page) return;
  if (p == C5_PAGE_BUTTONS) _pageBeforeDebug = _page;
  _page = p;
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
      setPage((uint8_t)((_page + C5_PAGE_COUNT - 1) % C5_PAGE_COUNT));
      return true;
    case C5_ACT_PAGE_NEXT:
      setPage((uint8_t)((_page + 1) % C5_PAGE_COUNT));
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
  const uint32_t asked = millis();
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
    // The beat, and when the answer was made.
    //
    // The answer is a few seconds old by the time it is on the screen, so
    // the beat is counted ON here: the period is known, so the place in
    // the beat is worked out from this answer and kept up to date.
    //
    // The answer was made somewhere between the asking and the getting,
    // so the middle of the two is when it was made - which is a better
    // guess than the moment it arrived.
    _tempoAtMs      = (asked + millis()) / 2;
    _tempoBeatCount = (uint32_t)(tempo["beat_count"] | 0);
    _tempoBeatInBar = _beatInBar;
    _tempoProgress  = _beatProgress;
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

// Where the beat is NOW.
//
// The board with the tracker is asked every few seconds, so what it says
// about the beat is a few seconds old by the time it is on the screen -
// and a beat that is three seconds old is not a beat.
//
// The beat's period is known - the rate is in the answer - so the place in
// the beat can be worked out from the answer and kept up to date here.
// The beat count and the beat in the bar follow from it.
//
// What this cannot do is follow a grid that is moving: the P4's own grid
// can drift, and this walks the last rate it was told about.  Over the
// three seconds between the answers that is a small error, and it is an
// error in the same direction every time.
void C5RemoteUsermod::beatNow(float &progress, int &beatInBar, uint32_t &beatCount) {
  progress = 0.0f;
  beatInBar = 0;
  beatCount = 0;
  if (!_tempoUsable || _bpm <= 0.0f) return;
  const float period = 60000.0f / _bpm;      // the beat, in ms
  if (period <= 0.0f) return;
  const float elapsed = (float)(millis() - _tempoAtMs);
  // The beats since the answer: the place in the beat it gave, plus the
  // time since.
  const float beats = _tempoProgress + (elapsed / period);
  const uint32_t whole = (uint32_t)beats;    // the whole beats
  progress  = beats - (float)whole;         // and the part of a beat
  beatCount = _tempoBeatCount + whole;
  beatInBar = (int)((((uint32_t)(_tempoBeatInBar > 0 ? _tempoBeatInBar : 1) - 1 + whole) % 4) + 1);
}

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

// Wipe the area the page uses.
//
// The whole screen is 200 KB of writes into PSRAM and it is the most
// expensive thing the board does - it takes longer than sending the
// picture.  The pages do not use all of it, so only the rows they use are
// wiped.
//
// The exception is a page CHANGE: the new page may draw where the old
// one did not, so then the whole screen is wiped.
void C5RemoteUsermod::clearPageArea(int y0, int y1) {
  if (_drawnPage != _page) {
    _drawnPage = _page;
    _panel.clear(C5_BG);
    return;
  }
  // The rows the page has to wipe before it draws.
  //
  // Only the rows that are NOT painted over again need wiping: a page
  // that fills its own panels covers them, and wiping them is the most
  // expensive thing this board does.
  _panel.clearRows(y0, y1, C5_BG);
  // The meter is at the bottom of the SYNC page.  A page that does not
  // have one has to cover it: a stale meter on another page is a lie.
  // (Unless the page's own wipe already covers it.)
  if (_page != C5_PAGE_SYNC && y1 < C5_METER_TOP) {
    _panel.fillRect(0, C5_METER_TOP, C5_PANEL_W, C5_METER_H, C5_BG);
  }
}

// The newest sender: its packets, its bands, and when they arrived.
//
// The table is written in the network's task and read here, so it is read
// under its lock.
bool C5RemoteUsermod::readNewestSender(C5SenderInfo &out) {
  if (arSyncSenderLock == nullptr) return false;
  if (xSemaphoreTake(arSyncSenderLock, pdMS_TO_TICKS(5)) != pdTRUE) return false;
  const ArSyncSender *s = nullptr;
  for (uint8_t i = 0; i < arSyncSenderCount; i++) {
    if (s == nullptr || arSyncSenders[i].lastMs > s->lastMs) s = &arSyncSenders[i];
  }
  if (s != nullptr) {
    out.ip = s->ip;
    out.packets = s->packets;
    out.rate = s->rate;
    out.lastMs = s->lastMs;
    out.level = s->level;
    memcpy(out.bands, s->bands, sizeof(out.bands));
    out.timeCount = s->timeCount;
    const uint8_t start = (uint8_t)((s->timeIdx + AR_SYNC_CADENCE_LEN - out.timeCount) % AR_SYNC_CADENCE_LEN);
    for (uint8_t k = 0; k < out.timeCount; k++) {
      out.times[k] = s->times[(start + k) % AR_SYNC_CADENCE_LEN];
    }
  }
  xSemaphoreGive(arSyncSenderLock);
  return s != nullptr;
}

void C5RemoteUsermod::drawSyncPage() {
  char line[64];

  // The page is wiped first: what was on it is not necessarily on it
  // now (a sender that went quiet leaves its line behind otherwise).
  clearPageArea();

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

  // The newest sender: the bands for the meter, and the times for the
  // cadence.
  C5SenderInfo newest;
  const bool haveSender = readNewestSender(newest);

  // The cadence of the packets.
  //
  // Not drawn here: the strip of ticks is a picture that changes every
  // frame, and that is drawing cycles the meter needs.  The PACKETS page
  // has it, and all this page says is whether there is anything to look
  // at - which is nothing, most of the time.
  uint32_t gapSum = 0, gapMin = 0xFFFFFFFF, gapMax = 0;
  uint8_t gapCount = 0;
  for (uint8_t k = 1; k < newest.timeCount; k++) {
    const uint32_t g = newest.times[k] - newest.times[k - 1];
    gapSum += g;
    if (g < gapMin) gapMin = g;
    if (g > gapMax) gapMax = g;
    gapCount++;
  }
  if (gapCount > 0) {
    const uint32_t mean = gapSum / gapCount;
    // Bunched: two packets within a few milliseconds of each other, or a
    // gap a long way bigger than the rest.
    if (gapMin < 4 || gapMax > mean * 3) {
      _panel.drawText(2, y, "packets in bunches - see PACKETS", C5_WARN, C5_BG);
      y += 12;
    }
  }

  const int meterTop = C5_PANEL_H - 12 - 96;
  _panel.fillRect(0, meterTop - 2, C5_PANEL_W, 98, C5_PANEL_BG);
  drawBars(haveSender ? newest.bands : nullptr, 4, meterTop, C5_PANEL_W - 8, 92);

  // The level of the last packet.
  if (haveSender) {
    snprintf_P(line, sizeof(line), PSTR("level %d"), newest.level);
    _panel.drawTextRight(C5_PANEL_W - 4, meterTop + 2, line, C5_DIM, C5_PANEL_BG);
  }

  // The footer: what the buttons do on this page.
  drawFooter("< prev   next >", "beats >");
}

// ---------------------------------------------------------------------------
// The packets page
// ---------------------------------------------------------------------------
//
// How the audio sync packets are spaced out.
//
// The audio sync goes out at the analysis rate, so the packets SHOULD be
// evenly spaced - one every ten milliseconds or so.  Whether they are is
// worth knowing: a sender that bunches them up, or leaves gaps between
// them, is a sender whose timing is off.
//
// The strip is the arrivals: the last quarter of a second, with a tick
// for each packet.  Evenly spaced packets are an even comb of ticks;
// bunches are ticks that crowd together.

void C5RemoteUsermod::drawPacketsPage() {
  char line[64];

  clearPageArea(0, 307);

  C5SenderInfo s;
  const bool have = readNewestSender(s);

  // The gaps between the packets.
  uint32_t gapSum = 0, gapMin = 0xFFFFFFFF, gapMax = 0;
  uint8_t gapCount = 0;
  for (uint8_t k = 1; k < s.timeCount; k++) {
    const uint32_t g = s.times[k] - s.times[k - 1];
    gapSum += g;
    if (g < gapMin) gapMin = g;
    if (g > gapMax) gapMax = g;
    gapCount++;
  }
  const uint32_t mean = gapCount ? (gapSum / gapCount) : 0;
  // Bunched: two packets within a few milliseconds of each other, or a
  // gap a long way bigger than the rest.
  const bool bunched = gapCount && (gapMin < 4 || gapMax > mean * 3);

  drawHeader("PACKETS", have ? (bunched ? "BUNCHED" : "even") : "no packets");

  if (!have) {
    _panel.drawText(2, 24, "waiting for a board to send", C5_DIM, C5_BG);
    drawFooter("< prev   next >", "");
    return;
  }

  // Who, and how fast.
  const String name = senderName(s.ip);
  _panel.drawText(2, 20, name.c_str(), C5_TEXT, C5_BG);
  snprintf_P(line, sizeof(line), PSTR("%lu/s   %lu pkts"), (unsigned long)s.rate, (unsigned long)s.packets);
  _panel.drawTextRight(C5_PANEL_W - 2, 20, line, C5_DIM, C5_BG);

  // The gaps themselves.
  if (gapCount > 0) {
    snprintf_P(line, sizeof(line), PSTR("%lu ms apart   %lu..%lu   %s"),
               (unsigned long)mean, (unsigned long)gapMin, (unsigned long)gapMax,
               bunched ? "BUNCHED" : "even");
    _panel.drawText(2, 32, line, bunched ? C5_WARN : C5_GOOD, C5_BG);
  } else {
    _panel.drawText(2, 32, "no packets yet", C5_DIM, C5_BG);
  }

  // The strip: the last quarter of a second, with a tick for each packet.
  const int stripTop = 46, stripH = 62;
  const uint32_t window = 250;   // ms
  _panel.fillRect(0, stripTop, C5_PANEL_W, stripH + 2, C5_PANEL_BG);
  _panel.drawHLine(4, stripTop + stripH, C5_PANEL_W - 8, C5_DIM);
  if (s.timeCount > 0) {
    const uint32_t startMs = (s.lastMs > window) ? (s.lastMs - window) : 0;
    for (uint8_t k = 0; k < s.timeCount; k++) {
      if (s.times[k] < startMs) continue;   // older than the window
      const int x = 4 + (int)(((s.times[k] - startMs) * (C5_PANEL_W - 8)) / window);
      _panel.fillRect(x, stripTop + 2, 2, stripH - 2, C5_ACCENT);
    }
  }
  _panel.drawText(2, stripTop + stripH + 6, "each tick is a packet, the last 250 ms",
                  C5_DIM, C5_PANEL_BG);

  // The gaps, one bar each: the shape of the spread.
  const int histTop = 140, histH = 56;
  _panel.fillRect(0, histTop, C5_PANEL_W, histH + 2, C5_PANEL_BG);
  if (gapCount > 0) {
    const int bars = gapCount;
    const int slot = (C5_PANEL_W - 8) / bars;
    for (uint8_t k = 0; k < gapCount; k++) {
      const uint32_t g = s.times[k + 1] - s.times[k];
      int h = (int)((uint32_t)histH * g / (gapMax ? gapMax : 1));
      if (h < 2) h = 2;
      if (h > histH) h = histH;
      _panel.fillRect(4 + k * slot, histTop + histH - h, slot - 2, h,
                      g < 4 ? C5_WARN : C5_ACCENT);
    }
  }
  _panel.drawText(2, histTop + histH + 6, "the gaps between the packets, the newest on the right",
                  C5_DIM, C5_PANEL_BG);

  drawFooter("< prev   next >", "");
}

void C5RemoteUsermod::drawBeatPage() {
  char line[64];

  // The page is wiped first: what was on it is not necessarily on it
  // now (a sender that went quiet leaves its line behind otherwise).
  clearPageArea();

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

  // Where the beat is.
  //
  // COUNTED HERE, not asked for: the answer is a few seconds old, and the
  // beat's period is known, so the place in the beat is worked out from
  // the answer and kept up to date.
  float progress = 0.0f;
  int beatInBar = 0;
  uint32_t beatCount = 0;
  beatNow(progress, beatInBar, beatCount);
  snprintf_P(line, sizeof(line), PSTR("beat %d of 4   #%lu"), beatInBar, (unsigned long)beatCount);
  _panel.drawText(2, 34, line, _tempoUsable ? C5_TEXT : C5_DIM, C5_BG);

  // How good the estimate is, and how far the grid has drifted.
  snprintf_P(line, sizeof(line), PSTR("quality %.2f   drift %+.0f ms"), (double)_quality, (double)_driftMs);
  _panel.drawText(2, 46, line, C5_DIM, C5_BG);

  // The mode, and whether the downbeat has been marked.
  snprintf_P(line, sizeof(line), PSTR("mode: %s"), _modeName.c_str());
  _panel.drawText(2, 62, line, C5_DIM, C5_BG);
  _panel.drawText(2, 74, _downbeatSet ? "downbeat: marked" : "downbeat: not marked",
                 _downbeatSet ? C5_GOOD : C5_DIM, C5_BG);
  // What the board is showing: the preset and the brightness.
  snprintf_P(line, sizeof(line), PSTR("preset %d   bri %d"), _preset, _p4Brightness);
  _panel.drawText(2, 90, line, C5_DIM, C5_BG);

  // What the buttons do here.
  drawFooter("BOOT: downbeat", "hold: lock");
}

void C5RemoteUsermod::drawBoardPage() {
  char line[64];

  // The page is wiped first: what was on it is not necessarily on it
  // now (a sender that went quiet leaves its line behind otherwise).
  clearPageArea();

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
  _panel.drawText(46, 94, _p4Host.c_str(), _p4Online ? C5_TEXT : C5_DIM, C5_BG);

  // The TF card.
  if (_sd.mounted()) {
    snprintf_P(line, sizeof(line), PSTR("tf: %lu MB  %u files"),
               (unsigned long)(_sd.sizeBytes() / (1024ULL * 1024ULL)),
               (unsigned)_sd.fileCount());
    _panel.drawText(2, 104, line, C5_GOOD, C5_BG);
  } else {
    snprintf_P(line, sizeof(line), PSTR("tf: %s"), _sd.status());
    _panel.drawText(2, 104, line, C5_DIM, C5_BG);
  }

  // The last thing a button did.
  if (_lastAction.length() && (millis() - _lastActionMs) < 3000) {
    _panel.drawText(2, 116, _lastAction.c_str(), C5_ACCENT, C5_BG);
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
  // The test page uses the whole screen, so it wipes all of it.
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
// The button debug page
// ---------------------------------------------------------------------------
//
// Every button the board has, and what it is doing.
//
// The three on the chip are the board's own - KEY_L, KEY_R and BOOT - and
// the eleven on the IO expander are the header's IO pins.  The board's
// fourth button, PWR, is the power latch: it goes to the chip that
// latches the board's power on, and not to a GPIO of this one, so there is
// nothing to read.
//
// The page watches the buttons instead of acting on them: the core's
// buttons are captured while this page is up, so that pressing "next" does
// not leave the page before the press can be seen.  BOOT leaves the page.

static void c5BtnName(uint8_t id, char *buf, size_t len) {
  if (id == 0)      snprintf(buf, len, "KEY_L");
  else if (id == 1) snprintf(buf, len, "KEY_R");
  else if (id == 2) snprintf(buf, len, "BOOT");
  else              snprintf(buf, len, "IO%u", (unsigned)(C5_IOEXP_BTN_FIRST + id - C5_BTN_CHIP_COUNT));
}

void C5RemoteUsermod::drawButtonPage() {
  char line[64];
  char name[8];

  // The page uses most of the screen, so it wipes most of it.
  clearPageArea(176, 307);

  // How many are down at this moment.
  uint8_t down = 0;
  for (uint8_t i = 0; i < C5_BTN_TOTAL; i++) if (_btnDown[i]) down++;

  drawHeader("BUTTONS", down ? "PRESSED" : "idle");

  // The three on the board.
  _panel.drawText(2, 18, "ON THE BOARD", C5_DIM, C5_BG);
  static const char *chipNames[C5_BTN_CHIP_COUNT] = {"KEY_L", "KEY_R", "BOOT"};
  static const char *chipPins[C5_BTN_CHIP_COUNT]  = {"GPIO2", "GPIO3", "GPIO28"};
  for (uint8_t i = 0; i < C5_BTN_CHIP_COUNT; i++) {
    const int x = 10 + i * 102;
    const bool down_ = _btnDown[i];
    const uint16_t bg = down_ ? (_btnHeldFired[i] ? C5_WARN : C5_GOOD) : C5_PANEL_BG;
    const uint16_t fg = down_ ? C5_BG : C5_TEXT;
    const uint16_t sub = down_ ? C5_BG : C5_DIM;
    _panel.fillRect(x, 30, 94, 30, bg);
    _panel.drawRect(x, 30, 94, 30, down_ ? bg : C5_DIM);
    _panel.drawTextCentre(x + 47, 34, chipNames[i], fg, bg);
    _panel.drawTextCentre(x + 47, 45, chipPins[i], sub, bg);
  }
  // The board's fourth button.  It is not on a GPIO of this chip, so there
  // is nothing here to read.
  _panel.drawText(2, 63, "PWR is the power latch - not on a GPIO", C5_DIM, C5_BG);

  // The expander's: IO4..IO14, on the header.
  _panel.drawText(2, 76, "ON THE EXPANDER (the header)", C5_DIM, C5_BG);
  for (uint8_t i = 0; i < C5_IOEXP_BTN_COUNT; i++) {
    const uint8_t id = (uint8_t)(C5_BTN_CHIP_COUNT + i);
    const int col = i % 4;
    const int row = i / 4;
    const int x = 8 + col * 77;
    const int y = 88 + row * 26;
    const bool down_ = _btnDown[id];
    const uint16_t bg = down_ ? (_btnHeldFired[id] ? C5_WARN : C5_GOOD) : C5_PANEL_BG;
    const uint16_t fg = down_ ? C5_BG : C5_TEXT;
    _panel.fillRect(x, y, 71, 24, bg);
    _panel.drawRect(x, y, 71, 24, down_ ? bg : C5_DIM);
    snprintf_P(name, sizeof(name), PSTR("IO%u"), (unsigned)(C5_IOEXP_BTN_FIRST + i));
    _panel.drawTextCentre(x + 35, y + 8, name, fg, bg);
  }
  // The expander's input register in one line: which pins are high.
  snprintf_P(line, sizeof(line), PSTR("the expander's inputs: 0x%04X"), (unsigned)_ioInputsStable);
  _panel.drawText(2, 168, line, C5_DIM, C5_BG);

  // The log: the last few things that happened.
  _panel.drawText(2, 178, "THE LOG", C5_DIM, C5_BG);
  for (uint8_t i = 0; i < C5_BTN_LOG_LEN && i < _btnLogCount; i++) {
    const uint8_t idx = (uint8_t)((_btnLogHead + C5_BTN_LOG_LEN - 1 - i) % C5_BTN_LOG_LEN);
    const C5BtnEvent &e = _btnLog[idx];
    c5BtnName(e.id, name, sizeof(name));
    const char *kind = (e.kind == 0) ? "down" : (e.kind == 1) ? "up" : "held";
    char dur[8];
    if (e.kind == 0) snprintf(dur, sizeof(dur), "-");
    else             snprintf(dur, sizeof(dur), "%lu ms", (unsigned long)e.heldMs);
    snprintf_P(line, sizeof(line), PSTR("%-6s %-5s %-6s %u:%02u.%u"),
               name, kind, dur,
               (unsigned)(e.ms / 60000), (unsigned)((e.ms / 1000) % 60),
               (unsigned)((e.ms / 100) % 10));
    _panel.drawText(2, 190 + i * 10, line, i == 0 ? C5_ACCENT : C5_DIM, C5_BG);
  }

  // The footer.
  snprintf_P(line, sizeof(line), PSTR("%lu presses"), (unsigned long)_btnPresses);
  drawFooter("BOOT leaves the page", line);
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
void C5RemoteUsermod::loop() {
  // A reboot that was asked for over the network.
  //
  // It is done here and not in the handler: the handler runs in the web
  // server's task, and a restart from there takes the server with it
  // before the answer is out.
  if (_rebootAsked) {
    _rebootAsked = false;
    USER_PRINTF("C5: rebooting on request\n");
    delay(100);
    ESP.restart();
    return;
  }

  if (!_panel.ready()) return;

  // The radio's band, until it has been set.
  //
  // The radio comes up after the setup, and the band has to be set before
  // the scan that looks for the network.
  if (!_bandSet) applyWifiBand();

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

  // The buttons.  Often enough to be quick, and no more often than the
  // expander is read - the expander's own inputs cannot change faster
  // than that, so reading them faster would only cost time.
  static uint32_t lastButtons = 0;
  if (now - lastButtons >= 10) {
    lastButtons = now;
    pollButtons();
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
  if (_redraw || (now - _lastDraw >= 50)) {
    _lastDraw = now;
    _redraw = false;
    const int64_t drawStart = esp_timer_get_time();
    switch (_page) {
      case C5_PAGE_SYNC:  drawSyncPage(); break;
      case C5_PAGE_BEAT:  drawBeatPage(); break;
      case C5_PAGE_BUTTONS: drawButtonPage(); break;
      case C5_PAGE_PACKETS: drawPacketsPage(); break;
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
    USER_PRINTF("C5: the screen: %u ms to draw (%u clear), %u ms to send (%u bands), %u frames\n",
                (unsigned)_drawMs, (unsigned)_panel.lastClearMs(),
                (unsigned)_panel.lastFrameMs(),
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
      // And it is remembered: the band is what the board comes up with,
      // and the band is what makes the connection quick.
      //
      // The flag, not serializeConfig() itself: the JSON handler is
      // holding the document's lock, and the save needs it.
      doSerializeConfig = true;
    }
  }
  if (me["page"].is<int>()) {
    const int p = me["page"];
    if (p >= 0 && p < C5_PAGE_COUNT) setPage((uint8_t)p);
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
  // The panel's timings.  The panel is the slow part of this board, and
  // these are what say so - and they are here rather than only on the
  // console because reading the console means resetting the board.
  me["panel_ms"]     = (unsigned long)_panel.lastFrameMs();
  me["panel_bands"]  = (unsigned long)_panel.lastBandCount();
  me["panel_frames"] = (unsigned long)_panel.frameCount();
  me["panel_clear_ms"] = (unsigned long)_panel.lastClearMs();
  me["draw_ms"]      = (unsigned long)_drawMs;
  // How long it took to come up.
  me["wifi_ms"]  = (unsigned long)_wifiMs;
  me["setup_ms"] = (unsigned long)_setupMs;
  me["wifi_band"] = _wifiBand;
  // The TF card.
  JsonObject sd = me.createNestedObject("sd");
  sd["mounted"] = _sd.mounted();
  sd["status"]  = _sd.status();
  sd["size_mb"] = (unsigned long)(_sd.sizeBytes() / (1024ULL * 1024ULL));
  sd["files"]   = _sd.fileCount();
  sd["listing"] = _sd.listing();
  // The buttons: what is pressed at this moment, and what has happened.
  //
  // The screen shows the same thing, but this is the version that can be
  // read from a desk.
  JsonObject bt = me.createNestedObject("buttons");
  uint8_t down = 0;
  for (uint8_t i = 0; i < C5_BTN_TOTAL; i++) if (_btnDown[i]) down++;
  bt["down"]    = down;
  bt["presses"] = (unsigned long)_btnPresses;
  bt["key_l"]   = _btnDown[0];
  bt["key_r"]   = _btnDown[1];
  bt["boot"]    = _btnDown[2];
  JsonArray bl = bt.createNestedArray("log");
  char name[8];
  char entry[40];
  for (uint8_t i = 0; i < C5_BTN_LOG_LEN && i < _btnLogCount; i++) {
    const uint8_t idx = (uint8_t)((_btnLogHead + C5_BTN_LOG_LEN - 1 - i) % C5_BTN_LOG_LEN);
    const C5BtnEvent &e = _btnLog[idx];
    c5BtnName(e.id, name, sizeof(name));
    const char *kind = (e.kind == 0) ? "down" : (e.kind == 1) ? "up" : "held";
    snprintf_P(entry, sizeof(entry), PSTR("%s %s %lu ms"), name, kind, (unsigned long)e.heldMs);
    bl.add(entry);
  }
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
