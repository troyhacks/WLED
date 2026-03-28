#include "wled.h"
#ifdef USERMOD_ARTNETMAP
#include "../usermods/usermod_v2_artnetmap/usermod_v2_artnetmap.h"
#endif

/*
 * cfg_api.cpp — JSON settings API: GET/POST /json/cfg?p=N
 *
 * Parallel to xml.cpp (getSettingsJS) and set.cpp (handleSettingsSet),
 * but uses clean JSON instead of dynamic JS injection + URL-encoded form POST.
 * cfg.json / wsec.json / presets.json file formats are UNCHANGED.
 *
 * GET  /json/cfg?p=N  → returns current settings for page N as JSON
 * POST /json/cfg?p=N  → accepts JSON body, applies settings (mirrors set.cpp logic)
 *
 * Passwords are returned as {"len": N} sentinels (never plaintext).
 * Sending {"len": N} back means "unchanged"; sending a string updates the value.
 */

// ─── GPIO metadata ────────────────────────────────────────────────────────────
// Builds the _gpio metadata object used by the frontend for pin selectors.
// Must be called while the JSON buffer lock is already held by the caller.
// Does NOT call requestJSONBufferLock itself.

static void buildGPIOInfo(JsonObject gpio) {
  // Pins currently allocated (for conflict detection in frontend)
  JsonArray um_p = gpio.createNestedArray(F("um_p"));
  um_p.add(-1); // sentinel (matches original d.um_p=[-1,...] pattern)
  if (i2c_sda > -1) um_p.add(i2c_sda);
  if (i2c_scl > -1) um_p.add(i2c_scl);
  if (spi_mosi > -1) um_p.add(spi_mosi);
  if (spi_miso > -1) um_p.add(spi_miso);
  if (spi_sclk > -1) um_p.add(spi_sclk);
  // Add all other allocated pins (bus pins, IR, relay, buttons, etc.)
  for (int p = 0; p < WLED_NUM_PINS; p++) {
    if (pinManager.isPinAllocated(p)) {
      bool alreadyAdded = false;
      for (JsonVariant v : um_p) { if (v.as<int>() == p) { alreadyAdded = true; break; } }
      if (!alreadyAdded) um_p.add(p);
    }
  }

  // Reserved (can't use) and read-only pins
  JsonArray rsvd    = gpio.createNestedArray(F("rsvd"));
  JsonArray ro_gpio = gpio.createNestedArray(F("ro_gpio"));
  for (int p = 0; p < WLED_NUM_PINS; p++) {
    if (!pinManager.isPinOk(p, false) || pinManager.getPinOwner(p) == PinOwner::DebugOut) {
      rsvd.add(p);
    } else if (!pinManager.isPinOk(p, true)) {
      ro_gpio.add(p);
    }
  }

  // Max GPIO number
#if defined(ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
  gpio[F("max_gpio")] = NUM_DIGITAL_PINS - 1;
#else
  gpio[F("max_gpio")] = NUM_DIGITAL_PINS;
#endif

  // Digital pin labels (D0-D8 on ESP8266)
  JsonArray dt = gpio.createNestedArray(F("dt_pins"));
#if defined(ESP8266) && !defined(ARDUINO_ESP8266_ESP01)
  const int dt_arr[] = { D0, D1, D2, D3, D4, D5, D6, D7, D8, hardwareRX, hardwareTX };
#else
  const int dt_arr[] = { PM_NO_PIN, PM_NO_PIN, PM_NO_PIN, PM_NO_PIN, PM_NO_PIN,
                         PM_NO_PIN, PM_NO_PIN, PM_NO_PIN, PM_NO_PIN, hardwareRX, hardwareTX };
#endif
  for (int i = 0; i < 11; i++) dt.add(dt_arr[i]);

  // Analog-capable pins
  JsonArray a = gpio.createNestedArray(F("a_pins"));
  for (int i = 0; i < 11; i++) a.add(pinManager.getADCPin(PM_ADC1, i));

#ifdef SOC_PARLIO_SUPPORTED
  gpio[F("max_parlio")] = SOC_PARLIO_RX_UNIT_MAX_DATA_WIDTH;
#ifdef PARLIO_PINS
  {
    constexpr uint8_t parlio_default_pins[] = { PARLIO_PINS };
    JsonArray parlio = gpio.createNestedArray(F("parlio_pins"));
    for (size_t i = 0; i < sizeof(parlio_default_pins); i++) parlio.add(parlio_default_pins[i]);
  }
#endif
#endif

#ifdef USERMOD_ARTNETMAP
  {
    ArtNetMapUsermod* am = (ArtNetMapUsermod*)usermods.lookup(USERMOD_ID_ARTNETMAP);
    gpio[F("artnetMap_enabled")] = (am && am->isEnabled()) ? 1 : 0;
  }
#else
  gpio[F("artnetMap_enabled")] = 0;
#endif
}

// ─── GET /json/cfg?p=N ────────────────────────────────────────────────────────

void handleCfgGet(AsyncWebServerRequest *request) {
  if (!correctPIN) { request->send(403, "application/json", F("{\"error\":3}")); return; }

  // Parse ?p=N from query string
  byte subPage = request->arg("p").toInt();
  if (subPage < 1 || subPage > 10) { request->send(400, "application/json", F("{\"error\":1}")); return; }

  if (!requestJSONBufferLock(20)) { request->send(503, "application/json", F("{\"error\":5}")); return; }

  JsonObject root = doc.to<JsonObject>();

  // ── Page 1: WiFi ──────────────────────────────────────────────────────────
  if (subPage == 1) {
    root[F("CS")] = clientSSID;
    { JsonObject s = root.createNestedObject(F("CP")); s[F("len")] = strlen(clientPass); }
    JsonArray I = root.createNestedArray(F("I"));
    JsonArray G = root.createNestedArray(F("G"));
    JsonArray S = root.createNestedArray(F("S"));
    for (int i = 0; i < 4; i++) { I.add(staticIP[i]); G.add(staticGateway[i]); S.add(staticSubnet[i]); }
    root[F("CM")] = cmDNS;
    root[F("AB")] = apBehavior;
    root[F("AS")] = apSSID;
    root[F("AH")] = (bool)apHide;
    { JsonObject s = root.createNestedObject(F("AP")); s[F("len")] = strlen(apPass); }
    root[F("AC")] = apChannel;
    root[F("FG")] = (bool)force802_3g;
    root[F("WS")] = (bool)noWifiSleep;
#ifndef WLED_DISABLE_ESPNOW
    root[F("RE")] = (bool)enable_espnow_remote;
    root[F("RMAC")] = linked_remote;
#endif
#ifdef WLED_USE_ETHERNET
    root[F("ETH")] = ethernetType;
    root[F("ETHO")] = (bool)ethernetOnly;
#endif
    if (Network.isConnected()) {
      char s[32]; IPAddress ip = Network.localIP();
      sprintf(s, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
      if (Network.isEthernet()) strncat(s, " (Ethernet)", sizeof(s) - strlen(s) - 1);
#endif
      root[F("_ip")] = s;
    }
#ifndef WLED_DISABLE_ESPNOW
    if (last_signal_src[0] != 0) root[F("_rlid")] = last_signal_src;
#endif
  }

  // ── Page 2: LEDs ──────────────────────────────────────────────────────────
  if (subPage == 2) {
    JsonObject limits = root.createNestedObject(F("_limits"));
    limits[F("maxB")] = WLED_MAX_BUSSES;
    limits[F("maxV")] = WLED_MIN_VIRTUAL_BUSSES;
    limits[F("maxM")] = MAX_LED_MEMORY;
    limits[F("maxL")] = MAX_LEDS;
#ifdef SOC_PARLIO_SUPPORTED
    limits[F("maxPins")] = SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH;
#else
    limits[F("maxPins")] = 5;
#endif
    buildGPIOInfo(root.createNestedObject(F("_gpio")));

    root[F("MS")]  = (bool)autoSegments;
    root[F("CCT")] = (bool)correctWB;
    root[F("CR")]  = (bool)cctFromRgb;
    root[F("CB")]  = strip.cctBlending;
    root[F("FR")]  = strip.getTargetFps();
    root[F("AW")]  = Bus::getGlobalAWMode();
    root[F("LD")]  = (bool)strip.useLedsArray;

    JsonArray buses = root.createNestedArray(F("buses"));
    for (uint8_t s = 0; s < busses.getNumBusses(); s++) {
      Bus* bus = busses.getBus(s);
      if (!bus) continue;
      JsonObject b = buses.createNestedObject();
#ifdef SOC_PARLIO_SUPPORTED
      uint8_t pins[SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH];
#else
      uint8_t pins[5];
#endif
      uint8_t nPins = bus->getPins(pins);
      JsonArray pa = b.createNestedArray(F("pins"));
      for (uint8_t i = 0; i < nPins; i++) pa.add(pins[i] == 255 ? -1 : (int)pins[i]);
      b[F("len")]  = bus->getLength();
      b[F("type")] = bus->getType();
      b[F("co")]   = bus->getColorOrder() & 0x0F;
      b[F("wo")]   = bus->getColorOrder() >> 4;
      b[F("start")]= bus->getStart();
      b[F("rev")]  = (bool)bus->reversed;
      b[F("skip")] = bus->skippedLeds();
      b[F("rf")]   = (bool)bus->isOffRefreshRequired();
      b[F("aw")]   = bus->getAutoWhiteMode();
      b[F("ao")]   = bus->get_outputs();
      b[F("al")]   = bus->get_leds_per_output();
      b[F("af")]   = bus->get_fps_limit();
      b[F("maxp")] = bus->getMaxPixels();
      uint16_t spd = bus->getFrequency();
      if (bus->getType() > TYPE_ONOFF && bus->getType() < 48) {
        switch (spd) { case WLED_PWM_FREQ/3: spd=0; break; case WLED_PWM_FREQ/2: spd=1; break;
                       case WLED_PWM_FREQ*2: spd=3; break; case WLED_PWM_FREQ*3: spd=4; break; default: spd=2; }
      } else {
        switch (spd) { case  1000: spd=0; break; case  2000: spd=1; break;
                       case 10000: spd=3; break; case 20000: spd=4; break;
                       case 40000: spd=5; break; case 60000: spd=6; break; default: spd=2; }
      }
      b[F("sp")] = spd;
    }

    JsonArray com_arr = root.createNestedArray(F("com"));
    const ColorOrderMap& com = busses.getColorOrderMap();
    for (uint8_t s = 0; s < com.count(); s++) {
      const ColorOrderMapEntry* e = com.get(s);
      if (!e) break;
      JsonObject ce = com_arr.createNestedObject();
      ce[F("start")] = e->start; ce[F("len")] = e->len; ce[F("co")] = e->colorOrder;
    }

    root[F("MA")]  = strip.ablMilliampsMax;
    root[F("LA")]  = strip.milliampsPerLed;
    if (strip.currentMilliamps) root[F("_mA")] = strip.currentMilliamps;
    root[F("CA")]  = briS;
    root[F("BO")]  = (bool)turnOnAtBoot;
    root[F("BP")]  = bootPreset;
    root[F("GB")]  = (bool)gammaCorrectBri;
    root[F("GC")]  = (bool)gammaCorrectCol;
    root[F("GCP")] = (bool)gammaCorrectPreview;
    root[F("GV")]  = gammaCorrectVal;
    root[F("TF")]  = (bool)fadeTransition;
    root[F("TD")]  = transitionDelayDefault;
    root[F("PF")]  = (bool)strip.paletteFade;
    root[F("TP")]  = randomPaletteChangeTime;
    root[F("BF")]  = briMultiplier;
    root[F("TB")]  = nightlightTargetBri;
    root[F("TL")]  = nightlightDelayMinsDefault;
    root[F("TW")]  = nightlightMode;
    root[F("PB")]  = strip.paletteBlend;
    root[F("RL")]  = rlyPin;
    root[F("RM")]  = (bool)rlyMde;
    root[F("IP")]  = (bool)disablePullUp;
    root[F("TT")]  = touchThreshold;
    root[F("IR")]  = irPin;
    root[F("IT")]  = irEnabled;
    root[F("MSO")] = (bool)!irApplyToAllSelected;
    JsonArray btns = root.createNestedArray(F("buttons"));
    for (uint8_t i = 0; i < WLED_MAX_BUTTONS; i++) {
      JsonObject btn = btns.createNestedObject();
      btn[F("pin")]  = btnPin[i];
      btn[F("type")] = buttonType[i];
    }
  }

  // ── Page 3: UI ────────────────────────────────────────────────────────────
  if (subPage == 3) {
    root[F("DS")] = serverDescription;
    root[F("ST")] = (bool)syncToggleReceive;
#ifdef WLED_ENABLE_SIMPLE_UI
    root[F("SU")] = (bool)simplifiedUI;
#endif
  }

  // ── Page 4: Sync ──────────────────────────────────────────────────────────
  if (subPage == 4) {
    root[F("UP")] = udpPort;       root[F("U2")] = udpPort2;
    root[F("GS")] = syncGroups;    root[F("GR")] = receiveGroups;
    root[F("RB")] = (bool)receiveNotificationBrightness;
    root[F("RC")] = (bool)receiveNotificationColor;
    root[F("RX")] = (bool)receiveNotificationEffects;
    root[F("SO")] = (bool)receiveSegmentOptions;
    root[F("SG")] = (bool)receiveSegmentBounds;
    root[F("SD")] = (bool)notifyDirectDefault;
    root[F("SB")] = (bool)notifyButton;
    root[F("SA")] = (bool)notifyAlexa;
    root[F("SH")] = (bool)notifyHue;
    root[F("SM")] = (bool)notifyMacro;
    root[F("UR")] = udpNumRetries;
    root[F("NL")] = (bool)nodeListEnabled;
    root[F("NB")] = (bool)nodeBroadcastEnabled;
    root[F("RD")] = (bool)receiveDirect;
    root[F("MO")] = (bool)useMainSegmentOnly;
    root[F("ES")] = (bool)e131SkipOutOfSequence;
    root[F("EM")] = (bool)e131Multicast;
    root[F("EP")] = e131Port;      root[F("EU")] = e131Universe;
    root[F("DA")] = DMXAddress;    root[F("XX")] = DMXSegmentSpacing;
    root[F("PY")] = e131Priority;  root[F("DM")] = DMXMode;
    root[F("ET")] = realtimeTimeoutMs;
    root[F("FB")] = (bool)arlsForceMaxBri;
    root[F("RG")] = (bool)arlsDisableGammaCorrection;
    root[F("WO")] = arlsOffset;    root[F("BD")] = serialBaud;
#ifdef WLED_ENABLE_DMX_INPUT
    root[F("IDMT")] = dmxInputTransmitPin;  root[F("IDMR")] = dmxInputReceivePin;
    root[F("IDME")] = dmxInputEnablePin;    root[F("IDMP")] = dmxInputPort;
#endif
    root[F("AL")] = (bool)alexaEnabled;
    root[F("AI")] = alexaInvocationName;
    root[F("AP")] = alexaNumPresets;
    // Feature flags — frontend uses these to show/hide optional sections
    { JsonObject ff = root.createNestedObject(F("_f"));
#ifdef WLED_ENABLE_MQTT
      ff[F("mqtt")] = true;
#else
      ff[F("mqtt")] = false;
#endif
#ifdef WLED_ENABLE_HUE
      ff[F("hue")] = true;
#else
      ff[F("hue")] = false;
#endif
#ifdef WLED_DEBUG_HOST
      ff[F("netDebug")] = true;
#else
      ff[F("netDebug")] = false;
#endif
#ifdef WLED_ENABLE_DMX_INPUT
      ff[F("dmxInput")] = true;
#else
      ff[F("dmxInput")] = false;
#endif
#ifdef WLED_ENABLE_DMX
      ff[F("dmxOut")] = true;
#else
      ff[F("dmxOut")] = false;
#endif
    }
  }

  // ── Page 5: Time ──────────────────────────────────────────────────────────
  if (subPage == 5) {
    root[F("NT")] = (bool)ntpEnabled;
    root[F("NS")] = ntpServerName;
    root[F("CF")] = (bool)!useAMPM;
    root[F("TZ")] = currentTimezone;  root[F("UO")] = utcOffsetSecs;
    root[F("LN")] = longitude;        root[F("LT")] = latitude;
    root[F("OL")] = overlayCurrent;
    root[F("O1")] = overlayMin;       root[F("O2")] = overlayMax;
    root[F("OM")] = analogClock12pixel;
    root[F("O5")] = (bool)analogClock5MinuteMarks;
    root[F("OS")] = (bool)analogClockSecondsTrail;
    root[F("CE")] = (bool)countdownMode;
    root[F("CY")] = countdownYear;  root[F("CI")] = countdownMonth;
    root[F("CD")] = countdownDay;   root[F("CH")] = countdownHour;
    root[F("CM")] = countdownMin;   root[F("CS")] = countdownSec;
    root[F("A0")] = macroAlexaOn;   root[F("A1")] = macroAlexaOff;
    root[F("MC")] = macroCountdown; root[F("MN")] = macroNl;
    JsonArray bm = root.createNestedArray(F("btnMacros"));
    for (uint8_t i = 0; i < WLED_MAX_BUTTONS; i++) {
      JsonObject bo = bm.createNestedObject();
      bo[F("s")] = macroButton[i]; bo[F("l")] = macroLongPress[i]; bo[F("d")] = macroDoublePress[i];
    }
    JsonArray timers = root.createNestedArray(F("timers"));
    for (int i = 0; i < 10; i++) {
      JsonObject t = timers.createNestedObject();
      t[F("N")] = timerMinutes[i]; t[F("T")] = timerMacro[i]; t[F("W")] = timerWeekday[i];
      if (i < 8) {
        t[F("H")] = timerHours[i];
        t[F("M")] = (timerMonth[i] >> 4) & 0x0F;
        t[F("P")] = timerMonth[i] & 0x0F;
        t[F("D")] = timerDay[i]; t[F("E")] = timerDayEnd[i];
      }
    }
  }

  // ── Page 6: Security ──────────────────────────────────────────────────────
  if (subPage == 6) {
    { JsonObject s = root.createNestedObject(F("PIN")); s[F("len")] = strlen(settingsPIN); }
    root[F("NO")] = (bool)otaLock;
    root[F("OW")] = (bool)wifiLock;
    root[F("AO")] = (bool)aOtaEnabled;
    { JsonObject s = root.createNestedObject(F("OP")); s[F("len")] = strlen(otaPass); }
    char ver[64];
    snprintf(ver, sizeof(ver), "WLEDMM %s (build %d)", versionString, VERSION);
    root[F("_ver")] = ver;
    root[F("_sd")]  = serverDescription;
  }

  // ── Page 7: DMX ───────────────────────────────────────────────────────────
#ifdef WLED_ENABLE_DMX
  if (subPage == 7) {
    root[F("PU")] = e131ProxyUniverse;
    root[F("CN")] = DMXChannels; root[F("CG")] = DMXGap;
    root[F("CS")] = DMXStart;   root[F("SL")] = DMXStartLED;
    JsonArray ch = root.createNestedArray(F("CH"));
    for (int i = 0; i < 15; i++) ch.add(DMXFixtureMap[i]);
  }
#endif

  // ── Page 8: Usermods ──────────────────────────────────────────────────────
  if (subPage == 8) {
    buildGPIOInfo(root.createNestedObject(F("_gpio")));
    JsonObject iface  = root.createNestedObject(F("if"));
    JsonObject i2cObj = iface.createNestedObject(F("i2c"));
    i2cObj[F("SDA")] = i2c_sda; i2cObj[F("SCL")] = i2c_scl;
    JsonObject spiObj = iface.createNestedObject(F("spi"));
    spiObj[F("MOSI")] = spi_mosi; spiObj[F("MISO")] = spi_miso; spiObj[F("SCLK")] = spi_sclk;
    // Current usermod config values
    JsonObject um = root.createNestedObject(F("um"));
    usermods.addToConfig(um);
    // Schema (usermods that implement addToSettingsSchema get rich field metadata)
    JsonObject schema = root.createNestedObject(F("_schema"));
    usermods.addToSettingsSchema(schema);
  }

  // ── Page 10: 2D Matrix ────────────────────────────────────────────────────
  if (subPage == 10) {
    root[F("SOMP")] = strip.isMatrix;
#ifndef WLED_DISABLE_2D
    root[F("maxPanels")] = WLED_MAX_PANELS;
    if (strip.isMatrix) {
      if (strip.panels > 0) { root[F("PW")] = strip.panel[0].width; root[F("PH")] = strip.panel[0].height; }
      root[F("MPC")] = strip.panels;  root[F("BA")]  = strip.bOrA;
      root[F("MPH")] = strip.panelsH; root[F("MPV")] = strip.panelsV;
      root[F("PB")]  = strip.matrix.bottomStart; root[F("PR")] = strip.matrix.rightStart;
      root[F("PV")]  = strip.matrix.vertical;    root[F("PS")] = (bool)strip.matrix.serpentine;
      root[F("PBL")] = strip.panelO.bottomStart; root[F("PRL")] = strip.panelO.rightStart;
      root[F("PVL")] = strip.panelO.vertical;    root[F("PSL")] = (bool)strip.panelO.serpentine;
      uint32_t lc = 0;
      for (int8_t b = 0; b < busses.getNumBusses(); b++) lc += busses.getBus(b)->getLength();
      root[F("LC")] = lc;
      JsonArray panels = root.createNestedArray(F("panels"));
      for (uint8_t i = 0; i < strip.panels; i++) {
        JsonObject p = panels.createNestedObject();
        p[F("B")] = strip.panel[i].bottomStart; p[F("R")] = strip.panel[i].rightStart;
        p[F("V")] = strip.panel[i].vertical;    p[F("S")] = (bool)strip.panel[i].serpentine;
        p[F("X")] = strip.panel[i].xOffset;     p[F("Y")] = strip.panel[i].yOffset;
        p[F("W")] = strip.panel[i].width;       p[F("H")] = strip.panel[i].height;
      }
    }
#endif
  }

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->addHeader(F("Cache-Control"), F("no-cache"));
  serializeJson(doc, *response);
  releaseJSONBufferLock();
  request->send(response);
}

// ─── POST /json/cfg?p=N ───────────────────────────────────────────────────────
// Called from the AsyncCallbackJsonWebHandler after JSON is already deserialized
// into `root` (which lives inside the globally-locked `doc` buffer).

void handleCfgSet(AsyncWebServerRequest *request, JsonObject root) {
  if (!correctPIN) { request->send(403, "application/json", F("{\"error\":3}")); return; }

  // Parse ?p=N from query string
  byte subPage = request->arg("p").toInt();
  if (subPage < 1 || subPage > 10) { request->send(400, "application/json", F("{\"error\":1}")); return; }

  // Helper: {"len": N} means "keep existing password unchanged"
  auto isSentinel = [](JsonVariant v) -> bool { return v.is<JsonObject>(); };

  // ── Page 1: WiFi ──────────────────────────────────────────────────────────
  if (subPage == 1) {
    if (root[F("CS")].is<const char*>()) strlcpy(clientSSID, root[F("CS")], 33);
    if (!isSentinel(root[F("CP")]) && root[F("CP")].is<const char*>())
      strlcpy(clientPass, root[F("CP")], 65);
    if (root[F("CM")].is<const char*>()) strlcpy(cmDNS, root[F("CM")], 33);
    if (!root[F("AB")].isNull()) apBehavior = root[F("AB")].as<int>();
    if (root[F("AS")].is<const char*>()) strlcpy(apSSID, root[F("AS")], 33);
    if (!root[F("AH")].isNull()) apHide = root[F("AH")].as<bool>();
    if (!isSentinel(root[F("AP")]) && root[F("AP")].is<const char*>()) {
      String ap = root[F("AP")].as<String>();
      if (ap.length() == 0 || ap.length() > 7) strlcpy(apPass, ap.c_str(), 65);
    }
    if (!root[F("AC")].isNull()) { int t = root[F("AC")]; if (t > 0 && t < 14) apChannel = t; }
    if (!root[F("FG")].isNull()) force802_3g  = root[F("FG")].as<bool>();
    if (!root[F("WS")].isNull()) noWifiSleep   = root[F("WS")].as<bool>();
    JsonArray I = root[F("I")], G = root[F("G")], S = root[F("S")];
    for (int i = 0; i < 4; i++) {
      if (!I.isNull() && I.size() > (size_t)i) staticIP[i]      = I[i].as<int>();
      if (!G.isNull() && G.size() > (size_t)i) staticGateway[i] = G[i].as<int>();
      if (!S.isNull() && S.size() > (size_t)i) staticSubnet[i]  = S[i].as<int>();
    }
#ifndef WLED_DISABLE_ESPNOW
    if (!root[F("RE")].isNull()) enable_espnow_remote = root[F("RE")].as<bool>();
    if (root[F("RMAC")].is<const char*>()) {
      strlcpy(linked_remote, root[F("RMAC")], 13);
      strlcpy(linked_remote, strlwr(linked_remote), 13);
    }
#endif
#ifdef WLED_USE_ETHERNET
    if (!root[F("ETH")].isNull())  ethernetType = root[F("ETH")].as<int>();
    if (!root[F("ETHO")].isNull()) ethernetOnly = root[F("ETHO")].as<bool>();
#endif
  }

  // ── Page 2: LEDs ──────────────────────────────────────────────────────────
  if (subPage == 2) {
    if (rlyPin >= 0 && pinManager.isPinAllocated(rlyPin, PinOwner::Relay))
      pinManager.deallocatePin(rlyPin, PinOwner::Relay);
    if (irPin >= 0 && pinManager.isPinAllocated(irPin, PinOwner::IR))
      pinManager.deallocatePin(irPin, PinOwner::IR);
    for (uint8_t s = 0; s < WLED_MAX_BUTTONS; s++) {
      if (btnPin[s] >= 0 && pinManager.isPinAllocated(btnPin[s], PinOwner::Button))
        pinManager.deallocatePin(btnPin[s], PinOwner::Button);
    }

    if (!root[F("MS")].isNull())  autoSegments     = root[F("MS")].as<bool>();
    if (!root[F("CCT")].isNull()) correctWB         = root[F("CCT")].as<bool>();
    if (!root[F("CR")].isNull())  cctFromRgb        = root[F("CR")].as<bool>();
    if (!root[F("CB")].isNull())  { strip.cctBlending = root[F("CB")]; Bus::setCCTBlend(strip.cctBlending); }
    if (!root[F("AW")].isNull())  Bus::setGlobalAWMode(root[F("AW")]);
    if (!root[F("FR")].isNull())  strip.setTargetFps(root[F("FR")]);
    if (!root[F("LD")].isNull())  strip.useLedsArray = root[F("LD")].as<bool>();

#ifdef SOC_PARLIO_SUPPORTED
    const uint8_t nPins = SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH;
#else
    const uint8_t nPins = 5;
#endif
    bool busesChanged = false;
    JsonArray buses = root[F("buses")];
    if (!buses.isNull()) {
      uint8_t s = 0; int t = 0;
      for (JsonObject bo : buses) {
        if (s >= WLED_MAX_BUSSES + WLED_MIN_VIRTUAL_BUSSES) break;
        uint8_t pins[nPins];
        for (uint8_t i = 0; i < nPins; i++) pins[i] = 255;
        JsonArray pa = bo[F("pins")];
        if (!pa.isNull()) for (uint8_t i = 0; i < nPins && i < pa.size(); i++) {
          int pv = pa[i].as<int>(); pins[i] = (pv < 0) ? 255 : (uint8_t)pv;
        }
        uint8_t  type     = bo[F("type")] | 22;
        uint32_t length   = bo[F("len")]  | 1;
        uint32_t start    = bo.containsKey(F("start")) ? (uint32_t)bo[F("start")] : (uint32_t)t;
        t += length;
        uint8_t  colorOrder  = bo[F("co")]   | 0;
        uint8_t  channelSwap = Bus::hasWhite(type) ? (bo[F("wo")] | 0) : 0;
        uint32_t skip        = bo[F("skip")] | 0;
        uint8_t  awmode      = bo[F("aw")]   | 0;
        bool     reversed    = bo[F("rev")]  | false;
        bool     offRefresh  = bo[F("rf")]   | false;
        uint32_t outputs     = bo[F("ao")]   | 1;
        uint32_t lpo         = bo.containsKey(F("al")) ? (uint32_t)bo[F("al")] : length;
        uint8_t  fpsLim      = bo.containsKey(F("af")) ? (uint8_t)bo[F("af")] : (uint8_t)(33333/length);
        uint16_t sp_idx      = bo[F("sp")]   | 2;
        uint16_t freqHz = 0;
        if (type > TYPE_ONOFF && type < 49) {
          switch (sp_idx) { case 0: freqHz=WLED_PWM_FREQ/3; break; case 1: freqHz=WLED_PWM_FREQ/2; break;
                            case 3: freqHz=WLED_PWM_FREQ*2; break; case 4: freqHz=WLED_PWM_FREQ*3; break;
                            default: freqHz=WLED_PWM_FREQ; }
        } else if (type > 48 && type < 64) {
          switch (sp_idx) { case 0: freqHz=1000;  break; case 1: freqHz=2000;  break;
                            case 3: freqHz=10000; break; case 4: freqHz=20000; break;
                            case 5: freqHz=40000; break; case 6: freqHz=60000; break;
                            default: freqHz=5000; }
        }
        if (offRefresh) type |= 0x80;
        if (busConfigs[s]) delete busConfigs[s];
        busConfigs[s] = new BusConfig(type, pins, start, length,
                          colorOrder | (channelSwap << 4), reversed, skip, awmode,
                          freqHz, outputs, lpo, fpsLim);
        busesChanged = true; s++;
      }
    }

    JsonArray com_arr = root[F("com")];
    if (!com_arr.isNull()) {
      ColorOrderMap com = {};
      for (JsonObject ce : com_arr) {
        uint32_t cs = ce[F("start")] | 0, cl = ce[F("len")] | 0; uint8_t co = ce[F("co")] | 0;
        if (cl > 0) com.add(cs, cl, co);
      }
      busses.updateColorOrderMap(com);
    }

    int hw_ir = root.containsKey(F("IR")) ? root[F("IR")].as<int>() : -1;
    if (pinManager.allocatePin(hw_ir, false, PinOwner::IR)) irPin = hw_ir; else irPin = -1;
    if (!root[F("IT")].isNull())  irEnabled = root[F("IT")].as<int>();
    if (!root[F("MSO")].isNull()) irApplyToAllSelected = !root[F("MSO")].as<bool>();

    int hw_rly = root.containsKey(F("RL")) ? root[F("RL")].as<int>() : -1;
    if (pinManager.allocatePin(hw_rly, true, PinOwner::Relay)) rlyPin = hw_rly; else rlyPin = -1;
    if (!root[F("RM")].isNull()) rlyMde = root[F("RM")].as<bool>();

    if (!root[F("IP")].isNull()) disablePullUp = root[F("IP")].as<bool>();
    if (!root[F("TT")].isNull()) touchThreshold = root[F("TT")];
    JsonArray btns = root[F("buttons")];
    if (!btns.isNull()) {
      for (uint8_t i = 0; i < WLED_MAX_BUTTONS && i < btns.size(); i++) {
        JsonObject btn = btns[i];
        int hw_btn = btn[F("pin")] | -1;
        if (pinManager.allocatePin(hw_btn, false, PinOwner::Button)) {
          btnPin[i] = hw_btn; buttonType[i] = btn[F("type")] | BTN_TYPE_NONE;
#ifdef ARDUINO_ARCH_ESP32
          if ((buttonType[i]==BTN_TYPE_ANALOG||buttonType[i]==BTN_TYPE_ANALOG_INVERTED)
              && digitalPinToAnalogChannel(btnPin[i]) < 0) {
            btnPin[i] = -1; pinManager.deallocatePin(hw_btn, PinOwner::Button);
          } else
#endif
          {
#ifdef ESP32
            pinMode(btnPin[i], buttonType[i]==BTN_TYPE_PUSH_ACT_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
#else
            pinMode(btnPin[i], disablePullUp ? INPUT : INPUT_PULLUP);
#endif
          }
        } else { btnPin[i] = -1; buttonType[i] = BTN_TYPE_NONE; }
      }
    }

    if (!root[F("MA")].isNull()) strip.ablMilliampsMax = max(0L, (long)root[F("MA")].as<int>());
    if (!root[F("LA")].isNull()) strip.milliampsPerLed  = max(0L, (long)root[F("LA")].as<int>());
    if (!root[F("CA")].isNull()) briS = root[F("CA")];
    if (!root[F("BO")].isNull()) turnOnAtBoot = root[F("BO")].as<bool>();
    if (!root[F("BP")].isNull()) { int t = root[F("BP")]; if (t <= 250) bootPreset = t; }
    if (!root[F("GB")].isNull()) gammaCorrectBri     = root[F("GB")].as<bool>();
    if (!root[F("GC")].isNull()) gammaCorrectCol     = root[F("GC")].as<bool>();
    if (!root[F("GCP")].isNull()) gammaCorrectPreview = root[F("GCP")].as<bool>();
    if (!root[F("GV")].isNull()) {
      float gv = root[F("GV")].as<float>();
      gammaCorrectVal = gv;
      if (gv > 1.0f && gv <= 3.0f) calcGammaTable(gv);
      else { gammaCorrectVal=1.0f; gammaCorrectBri=false; gammaCorrectCol=false; gammaCorrectPreview=false; }
    }
    if (!root[F("TF")].isNull()) fadeTransition = root[F("TF")].as<bool>();
    if (!root[F("TD")].isNull()) { int t = root[F("TD")]; if (t >= 0) transitionDelayDefault = t; }
    if (!root[F("PF")].isNull()) strip.paletteFade = root[F("PF")].as<bool>();
    if (!root[F("TP")].isNull()) randomPaletteChangeTime = MIN(255, MAX(1, (int)root[F("TP")]));
    if (!root[F("TB")].isNull()) nightlightTargetBri = root[F("TB")];
    if (!root[F("TL")].isNull()) { int t = root[F("TL")]; if (t > 0) { nightlightDelayMinsDefault = t; nightlightDelayMins = t; } }
    if (!root[F("TW")].isNull()) nightlightMode = root[F("TW")];
    if (!root[F("PB")].isNull()) { int t = root[F("PB")]; if (t >= 0 && t < 4) strip.paletteBlend = t; }
    if (!root[F("BF")].isNull()) { int t = root[F("BF")]; if (t > 0) briMultiplier = t; }

    onload_map_loaded = false;
    doInitBusses = busesChanged;
  }

  // ── Page 3: UI ────────────────────────────────────────────────────────────
  if (subPage == 3) {
    if (root[F("DS")].is<const char*>()) strlcpy(serverDescription, root[F("DS")], 33);
    if (!root[F("ST")].isNull()) syncToggleReceive = root[F("ST")].as<bool>();
#ifdef WLED_ENABLE_SIMPLE_UI
    if (!root[F("SU")].isNull()) {
      if (simplifiedUI ^ root[F("SU")].as<bool>()) cacheInvalidate++;
      simplifiedUI = root[F("SU")].as<bool>();
    }
#endif
    strip.enumerateLedmaps();
    strip.loadCustomPalettes();
  }

  // ── Page 4: Sync ──────────────────────────────────────────────────────────
  if (subPage == 4) {
    if (!root[F("UP")].isNull()) { int t=root[F("UP")]; if(t>0) udpPort=t; }
    if (!root[F("U2")].isNull()) { int t=root[F("U2")]; if(t>0) udpPort2=t; }
    if (!root[F("GS")].isNull()) syncGroups    = root[F("GS")];
    if (!root[F("GR")].isNull()) receiveGroups = root[F("GR")];
    if (!root[F("RB")].isNull()) receiveNotificationBrightness = root[F("RB")].as<bool>();
    if (!root[F("RC")].isNull()) receiveNotificationColor      = root[F("RC")].as<bool>();
    if (!root[F("RX")].isNull()) receiveNotificationEffects    = root[F("RX")].as<bool>();
    if (!root[F("SO")].isNull()) receiveSegmentOptions         = root[F("SO")].as<bool>();
    if (!root[F("SG")].isNull()) receiveSegmentBounds          = root[F("SG")].as<bool>();
    receiveNotifications = receiveNotificationBrightness || receiveNotificationColor
                        || receiveNotificationEffects    || receiveSegmentOptions;
    if (!root[F("SD")].isNull()) { notifyDirectDefault = root[F("SD")].as<bool>(); notifyDirect = notifyDirectDefault; }
    if (!root[F("SB")].isNull()) notifyButton = root[F("SB")].as<bool>();
    if (!root[F("SA")].isNull()) notifyAlexa  = root[F("SA")].as<bool>();
    if (!root[F("SH")].isNull()) notifyHue    = root[F("SH")].as<bool>();
    if (!root[F("SM")].isNull()) notifyMacro  = root[F("SM")].as<bool>();
    if (!root[F("UR")].isNull()) { int t=root[F("UR")]; if(t>=0&&t<30) udpNumRetries=t; }
    if (!root[F("NL")].isNull()) { nodeListEnabled = root[F("NL")].as<bool>(); if (!nodeListEnabled) Nodes.clear(); }
    if (!root[F("NB")].isNull()) nodeBroadcastEnabled = root[F("NB")].as<bool>();
    if (!root[F("RD")].isNull()) receiveDirect     = root[F("RD")].as<bool>();
    if (!root[F("MO")].isNull()) useMainSegmentOnly = root[F("MO")].as<bool>();
    if (!root[F("ES")].isNull()) e131SkipOutOfSequence = root[F("ES")].as<bool>();
    if (!root[F("EM")].isNull()) e131Multicast         = root[F("EM")].as<bool>();
    if (!root[F("EP")].isNull()) { int t=root[F("EP")]; if(t>0) e131Port=t; }
    if (!root[F("EU")].isNull()) { int t=root[F("EU")]; if(t>=0&&t<=63999) e131Universe=t; }
    if (!root[F("DA")].isNull()) { int t=root[F("DA")]; if(t>=0&&t<=510) DMXAddress=t; }
    if (!root[F("XX")].isNull()) { int t=root[F("XX")]; if(t>=0&&t<=150) DMXSegmentSpacing=t; }
    if (!root[F("PY")].isNull()) { int t=root[F("PY")]; if(t>=0&&t<=200) e131Priority=t; }
    if (!root[F("DM")].isNull()) { int t=root[F("DM")]; if(t>=DMX_MODE_DISABLED&&t<=DMX_MODE_PRESET) DMXMode=t; }
    if (!root[F("ET")].isNull()) { int t=root[F("ET")]; if(t>99&&t<=65000) realtimeTimeoutMs=t; }
    if (!root[F("FB")].isNull()) arlsForceMaxBri         = root[F("FB")].as<bool>();
    if (!root[F("RG")].isNull()) arlsDisableGammaCorrection = root[F("RG")].as<bool>();
    if (!root[F("WO")].isNull()) { int t=root[F("WO")]; if(t>=-255&&t<=255) arlsOffset=t; }
    if (!root[F("BD")].isNull()) { int t=root[F("BD")]; if(t>=96&&t<=15000) { serialBaud=t; updateBaudRate(serialBaud*100); } }
#ifdef WLED_ENABLE_DMX_INPUT
    if (!root[F("IDMT")].isNull()) dmxInputTransmitPin = root[F("IDMT")];
    if (!root[F("IDMR")].isNull()) dmxInputReceivePin  = root[F("IDMR")];
    if (!root[F("IDME")].isNull()) dmxInputEnablePin   = root[F("IDME")];
    if (!root[F("IDMP")].isNull()) { int t=root[F("IDMP")]; if(t<=0||t>2) t=2; dmxInputPort=t; }
#endif
    if (!root[F("AL")].isNull()) alexaEnabled = root[F("AL")].as<bool>();
    if (root[F("AI")].is<const char*>()) strlcpy(alexaInvocationName, root[F("AI")], 33);
    if (!root[F("AP")].isNull()) { int t=root[F("AP")]; if(t>=0&&t<=9) alexaNumPresets=t; }
  }

  // ── Page 5: Time ──────────────────────────────────────────────────────────
  if (subPage == 5) {
    if (!root[F("NT")].isNull()) ntpEnabled = root[F("NT")].as<bool>();
    if (root[F("NS")].is<const char*>()) strlcpy(ntpServerName, root[F("NS")], 33);
    if (!root[F("CF")].isNull()) useAMPM = !root[F("CF")].as<bool>();
    if (!root[F("TZ")].isNull()) currentTimezone = root[F("TZ")];
    if (!root[F("UO")].isNull()) utcOffsetSecs   = root[F("UO")];
    ntpLastSyncTime = NTP_NEVER;
    if (!root[F("LN")].isNull()) longitude = root[F("LN")].as<float>();
    if (!root[F("LT")].isNull()) latitude  = root[F("LT")].as<float>();
    calculateSunriseAndSunset();
    if (!root[F("OL")].isNull()) overlayCurrent      = root[F("OL")].as<bool>() ? 1 : 0;
    if (!root[F("O1")].isNull()) overlayMin           = root[F("O1")];
    if (!root[F("O2")].isNull()) overlayMax           = root[F("O2")];
    if (!root[F("OM")].isNull()) analogClock12pixel   = root[F("OM")];
    if (!root[F("O5")].isNull()) analogClock5MinuteMarks  = root[F("O5")].as<bool>();
    if (!root[F("OS")].isNull()) analogClockSecondsTrail  = root[F("OS")].as<bool>();
    if (!root[F("CE")].isNull()) countdownMode = root[F("CE")].as<bool>();
    if (!root[F("CY")].isNull()) countdownYear  = root[F("CY")];
    if (!root[F("CI")].isNull()) countdownMonth = root[F("CI")];
    if (!root[F("CD")].isNull()) countdownDay   = root[F("CD")];
    if (!root[F("CH")].isNull()) countdownHour  = root[F("CH")];
    if (!root[F("CM")].isNull()) countdownMin   = root[F("CM")];
    if (!root[F("CS")].isNull()) countdownSec   = root[F("CS")];
    setCountdown();
    if (!root[F("A0")].isNull()) macroAlexaOn  = root[F("A0")];
    if (!root[F("A1")].isNull()) macroAlexaOff = root[F("A1")];
    if (!root[F("MC")].isNull()) macroCountdown = root[F("MC")];
    if (!root[F("MN")].isNull()) macroNl        = root[F("MN")];
    JsonArray bm = root[F("btnMacros")];
    if (!bm.isNull()) for (uint8_t i = 0; i < WLED_MAX_BUTTONS && i < bm.size(); i++) {
      JsonObject bo = bm[i];
      macroButton[i]      = bo[F("s")] | 0;
      macroLongPress[i]   = bo[F("l")] | 0;
      macroDoublePress[i] = bo[F("d")] | 0;
    }
    JsonArray timers = root[F("timers")];
    if (!timers.isNull()) for (int i = 0; i < 10 && i < (int)timers.size(); i++) {
      JsonObject t = timers[i];
      timerMinutes[i] = t[F("N")] | 0; timerMacro[i] = t[F("T")] | 0; timerWeekday[i] = t[F("W")] | 0;
      if (i < 8) {
        timerHours[i] = t[F("H")] | 0;
        timerMonth[i] = (((int)(t[F("M")] | 0) & 0x0F) << 4) | ((int)(t[F("P")] | 0) & 0x0F);
        timerDay[i]    = t[F("D")] | 0;
        timerDayEnd[i] = t[F("E")] | 0;
      }
    }
  }

  // ── Page 6: Security ──────────────────────────────────────────────────────
  if (subPage == 6) {
    if (root[F("RS")].as<bool>()) {
      WLED_FS.format();
      doReboot = true;
      request->send(200, "application/json", F("{\"success\":true,\"reboot\":true}"));
      return;
    }
    if (root[F("PIN")].is<const char*>()) {
      const char* pin = root[F("PIN")];
      uint8_t pl = strlen(pin);
      if (pl == 4 || pl == 0) {
        uint8_t nz = 0; for (uint8_t i = 0; i < pl; i++) nz += (pin[i]=='0');
        if (nz < pl || pl == 0) strlcpy(settingsPIN, pin, 5);
        settingsPIN[4] = 0;
      }
    }
    bool pwdOk = !otaLock;
    if (!isSentinel(root[F("OP")]) && root[F("OP")].is<const char*>()) {
      const char* op = root[F("OP")];
      if (otaLock && strcmp(otaPass, op) == 0 && millis() - lastEditTime > 3000) pwdOk = true;
      if (!otaLock && strlen(op) > 0) strlcpy(otaPass, op, 33);
    }
    if (pwdOk) {
      if (!root[F("NO")].isNull()) otaLock     = root[F("NO")].as<bool>();
      if (!root[F("OW")].isNull()) wifiLock    = root[F("OW")].as<bool>();
      if (!root[F("AO")].isNull()) aOtaEnabled = root[F("AO")].as<bool>();
    }
  }

  // ── Page 7: DMX ───────────────────────────────────────────────────────────
#ifdef WLED_ENABLE_DMX
  if (subPage == 7) {
    if (!root[F("PU")].isNull()) { int t=root[F("PU")]; if(t>=0&&t<=63999) e131ProxyUniverse=t; }
    if (!root[F("CN")].isNull()) { int t=root[F("CN")]; if(t>0&&t<16) DMXChannels=t; }
    if (!root[F("CS")].isNull()) { int t=root[F("CS")]; if(t>0&&t<513) DMXStart=t; }
    if (!root[F("CG")].isNull()) { int t=root[F("CG")]; if(t>0&&t<513) DMXGap=t; }
    if (!root[F("SL")].isNull()) { int t=root[F("SL")]; if(t>=0&&t<MAX_LEDS) DMXStartLED=t; }
    JsonArray ch = root[F("CH")];
    if (!ch.isNull()) for (int i = 0; i < 15 && i < (int)ch.size(); i++) DMXFixtureMap[i] = ch[i];
  }
#endif

  // ── Page 8: Usermods ──────────────────────────────────────────────────────
  if (subPage == 8) {
    JsonObject iface = root[F("if")];
    int8_t hw_sda = -2, hw_scl = -2, hw_mosi = -2, hw_miso = -2, hw_sclk = -2;
    if (!iface.isNull()) {
      JsonObject i2c = iface[F("i2c")];
      if (!i2c.isNull()) {
        if (!i2c[F("SDA")].isNull()) hw_sda = i2c[F("SDA")].as<int>();
        if (!i2c[F("SCL")].isNull()) hw_scl = i2c[F("SCL")].as<int>();
      }
      JsonObject spi = iface[F("spi")];
      if (!spi.isNull()) {
        if (!spi[F("MOSI")].isNull()) hw_mosi = spi[F("MOSI")].as<int>();
        if (!spi[F("MISO")].isNull()) hw_miso = spi[F("MISO")].as<int>();
        if (!spi[F("SCLK")].isNull()) hw_sclk = spi[F("SCLK")].as<int>();
      }
    }
    PinManagerPinType i2cPins[2] = { {hw_sda, true}, {hw_scl, true} };
    if (hw_sda >= 0 && hw_scl >= 0 && pinManager.allocateMultiplePins(i2cPins, 2, PinOwner::HW_I2C)) {
      i2c_sda = hw_sda; i2c_scl = hw_scl;
#ifdef ESP32
      Wire.setPins(i2c_sda, i2c_scl);
#endif
    } else {
      if (hw_sda == -1 || hw_scl == -1) { i2c_sda = -1; i2c_scl = -1; }
      uint8_t tmp[2] = { (uint8_t)i2c_scl, (uint8_t)i2c_sda };
      pinManager.deallocateMultiplePins(tmp, 2, PinOwner::HW_I2C);
    }
    PinManagerPinType spiPins[3] = { {hw_mosi, true}, {hw_miso, true}, {hw_sclk, true} };
    if (hw_mosi >= 0 && hw_sclk >= 0 && pinManager.allocateMultiplePins(spiPins, 3, PinOwner::HW_SPI)) {
      spi_mosi = hw_mosi; spi_miso = hw_miso; spi_sclk = hw_sclk;
    } else {
      if (hw_mosi == -1 || hw_sclk == -1) { spi_mosi = hw_mosi; spi_miso = hw_miso; spi_sclk = hw_sclk; }
      uint8_t tmp[3] = { (uint8_t)spi_mosi, (uint8_t)spi_miso, (uint8_t)spi_sclk };
      pinManager.deallocateMultiplePins(tmp, 3, PinOwner::HW_SPI);
    }
    JsonObject um = root[F("um")];
    if (!um.isNull()) usermods.readFromConfig(um);
  }

  // ── Page 10: 2D Matrix ────────────────────────────────────────────────────
  if (subPage == 10) {
    if (!root[F("SOMP")].isNull()) strip.isMatrix = root[F("SOMP")].as<int>();
#ifndef WLED_DISABLE_2D
    if (strip.isMatrix) {
      if (!root[F("MPC")].isNull()) strip.panels   = root[F("MPC")];
      if (!root[F("BA")].isNull())  strip.bOrA     = root[F("BA")];
      if (!root[F("MPH")].isNull()) strip.panelsH  = root[F("MPH")];
      if (!root[F("MPV")].isNull()) strip.panelsV  = root[F("MPV")];
      if (!root[F("PB")].isNull())  strip.matrix.bottomStart = root[F("PB")];
      if (!root[F("PR")].isNull())  strip.matrix.rightStart  = root[F("PR")];
      if (!root[F("PV")].isNull())  strip.matrix.vertical    = root[F("PV")];
      if (!root[F("PS")].isNull())  strip.matrix.serpentine  = root[F("PS")].as<bool>();
      if (!root[F("PBL")].isNull()) strip.panelO.bottomStart = root[F("PBL")];
      if (!root[F("PRL")].isNull()) strip.panelO.rightStart  = root[F("PRL")];
      if (!root[F("PVL")].isNull()) strip.panelO.vertical    = root[F("PVL")];
      if (!root[F("PSL")].isNull()) strip.panelO.serpentine  = root[F("PSL")].as<bool>();
      JsonArray panels = root[F("panels")];
      if (!panels.isNull()) for (uint8_t i = 0; i < strip.panels && i < panels.size(); i++) {
        JsonObject p = panels[i];
        strip.panel[i].bottomStart = p[F("B")] | 0; strip.panel[i].rightStart = p[F("R")] | 0;
        strip.panel[i].vertical    = p[F("V")] | 0; strip.panel[i].serpentine = p[F("S")] | false;
        strip.panel[i].xOffset     = p[F("X")] | 0; strip.panel[i].yOffset   = p[F("Y")] | 0;
        strip.panel[i].width       = p[F("W")] | 8; strip.panel[i].height    = p[F("H")] | 8;
      }
    }
    strip.setUpMatrix();
    strip.makeAutoSegments(true);
#endif
  }

  doSerializeConfig = true;
  lastEditTime = millis();
  request->send(200, "application/json", F("{\"success\":true}"));
}
