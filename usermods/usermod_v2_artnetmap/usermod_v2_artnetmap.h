#pragma once

/*
 * Art-Net Output Map Usermod
 *
 * Provides configuration for Art-Net output mapping with:
 * - Quick setup (X outputs × Y universes × Z LEDs)
 * - Per-output start universe and LED count
 * - Save/load presets to LittleFS
 * - Test mode for output identification
 *
 * Data model:
 *   startUniverse[i] = first universe for output i
 *   ledsPerOutput[i] = total LEDs on output i (spans ceil(leds/170) universes)
 *
 * Access at: http://[WLED_IP]/artnetmap
 */

#include "wled.h"

#ifndef ARTNETMAP_MAX_OUTPUTS
#define ARTNETMAP_MAX_OUTPUTS 1024
#endif

class ArtNetMapUsermod : public Usermod {

private:

  // Configuration
  uint16_t numOutputs = 0;
  uint16_t startUniverse[ARTNETMAP_MAX_OUTPUTS];
  uint32_t ledsPerOutput[ARTNETMAP_MAX_OUTPUTS];

  // Global settings
  char targetIP[16] = "255.255.255.255";
  uint16_t channelsPerUniverse = 510;
  uint8_t padMode = 0;  // 0=none, 1=black pixel, 2=full universe

  // State
  bool initDone = false;
  bool webInitDone = false;
  char currentPreset[32] = "";
  int16_t testingOutput = -1;
  unsigned long testStartTime = 0;
  unsigned long lastTestSend = 0;

  // String constants
  static const char _name[];
  static const char _enabled[];
  static const char _currentPreset[];

  // Calculate universes needed for LED count.
  // Assumes RGB (bpp=3). For RGBW the actual span is slightly larger; the
  // OUTPUT sender in udp.cpp is the authoritative source for emission.
  uint16_t calcUniverses(uint32_t leds) {
    if (leds == 0) return 0;
    return (uint16_t)((leds * 3ULL + channelsPerUniverse - 1) / channelsPerUniverse);
  }

  // Calculate end universe for output
  uint16_t endUniverse(uint16_t idx) {
    if (idx >= numOutputs) return 0;
    uint16_t unis = calcUniverses(ledsPerOutput[idx]);
    if (unis == 0) return startUniverse[idx];
    return startUniverse[idx] + unis - 1;
  }

  // Get total universe count (highest universe + 1)
  uint16_t getTotalUniverses() {
    uint16_t maxUni = 0;
    for (uint16_t i = 0; i < numOutputs; i++) {
      uint16_t end = endUniverse(i);
      if (end > maxUni) maxUni = end;
    }
    return numOutputs > 0 ? maxUni + 1 : 0;
  }

  // Generate sequential outputs
  void generateSequential(uint16_t count, uint16_t universesPerOutput, uint32_t leds) {
    numOutputs = min((uint16_t)ARTNETMAP_MAX_OUTPUTS, count);
    for (uint16_t i = 0; i < numOutputs; i++) {
      startUniverse[i] = i * universesPerOutput;
      ledsPerOutput[i] = leds;
    }
  }

  // Save preset to LittleFS
  bool savePreset(const char* name) {
    char filename[48];
    snprintf(filename, sizeof(filename), "/artnetmap_%s.json", name);

    File f = WLED_FS.open(filename, "w");
    if (!f) return false;

    f.printf("{\"n\":%d,\"ch\":%d,\"ip\":\"%s\",\"pad\":%d}\n",
      numOutputs, channelsPerUniverse, targetIP, padMode);

    // Write arrays
    f.print("[");
    for (uint16_t i = 0; i < numOutputs; i++) {
      if (i > 0) f.print(",");
      f.print(startUniverse[i]);
    }
    f.print("]\n[");
    for (uint16_t i = 0; i < numOutputs; i++) {
      if (i > 0) f.print(",");
      f.print(ledsPerOutput[i]);
    }
    f.print("]\n");

    f.close();
    strlcpy(currentPreset, name, sizeof(currentPreset));
    return true;
  }

  // Load preset from LittleFS
  bool loadPreset(const char* name) {
    char filename[48];
    snprintf(filename, sizeof(filename), "/artnetmap_%s.json", name);

    File f = WLED_FS.open(filename, "r");
    if (!f) return false;

    // Read metadata line
    String line = f.readStringUntil('\n');
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, line)) {
      f.close();
      return false;
    }

    // Tentative count from metadata; arrays may be shorter if the file is truncated
    uint16_t expected = min((uint16_t)ARTNETMAP_MAX_OUTPUTS, doc["n"] | (uint16_t)0);

    // Read startUniverse array - parse manually to avoid JSON memory limits
    line = f.readStringUntil('\n');
    uint16_t parsedStart = 0;
    if (line.length() > 2 && line[0] == '[') {
      char* ptr = (char*)line.c_str() + 1;  // Skip '['
      while (parsedStart < expected && *ptr && *ptr != ']') {
        startUniverse[parsedStart++] = strtoul(ptr, &ptr, 10);
        if (*ptr == ',') ptr++;  // Skip comma
      }
    }

    // Read ledsPerOutput array - parse manually
    line = f.readStringUntil('\n');
    uint16_t parsedLeds = 0;
    if (line.length() > 2 && line[0] == '[') {
      char* ptr = (char*)line.c_str() + 1;  // Skip '['
      while (parsedLeds < expected && *ptr && *ptr != ']') {
        ledsPerOutput[parsedLeds++] = strtoul(ptr, &ptr, 10);
        if (*ptr == ',') ptr++;  // Skip comma
      }
    }

    f.close();

    // Refuse the load if either array came up short: keeps existing state intact
    // rather than exposing partially-parsed entries under a misleading count.
    if (parsedStart < expected || parsedLeds < expected) {
      USER_PRINTF("ArtNetMap: Preset '%s' truncated (start %u/%u, leds %u/%u). Load aborted.\n",
                  name, parsedStart, expected, parsedLeds, expected);
      return false;
    }

    numOutputs = expected;
    channelsPerUniverse = doc["ch"] | 510;
    strlcpy(targetIP, doc["ip"] | "255.255.255.255", sizeof(targetIP));
    padMode = doc["pad"] | 0;
    strlcpy(currentPreset, name, sizeof(currentPreset));
    USER_PRINTF("ArtNetMap: Loaded preset '%s' with %d outputs, first LED count: %lu\n",
                name, numOutputs, ledsPerOutput[0]);
    return true;
  }

  // Delete preset
  bool deletePreset(const char* name) {
    char filename[48];
    snprintf(filename, sizeof(filename), "/artnetmap_%s.json", name);
    return WLED_FS.remove(filename);
  }

  // Serve the web page
  void servePage(AsyncWebServerRequest* request);

  // Handle API requests
  void handleApi(AsyncWebServerRequest* request);

  // Send one burst of Art-Net DMX packets for the output under test.
  // Fills each universe's DMX slots with white (0xFF) so the receiver lights up.
  void sendTestPacket();

public:

  // Get total LED count
  uint32_t getTotalLeds() {
    uint32_t total = 0;
    for (uint16_t i = 0; i < numOutputs; i++) {
      total += ledsPerOutput[i];
    }
    return total;
  }


  ArtNetMapUsermod(bool enabled) : Usermod("ArtNetMap", enabled) {
    // Initialize arrays
    for (uint16_t i = 0; i < ARTNETMAP_MAX_OUTPUTS; i++) {
      startUniverse[i] = 0;
      ledsPerOutput[i] = 0;
    }
  }

  // Getters for external Art-Net code
  inline bool isEnabled() { return enabled; }
  inline uint16_t getNumOutputs() { return numOutputs; }
  inline uint16_t getStartUniverse(uint16_t idx) { return idx < numOutputs ? startUniverse[idx] : 0; }
  inline uint32_t getLedsPerOutput(uint16_t idx) { return idx < numOutputs ? ledsPerOutput[idx] : 0; }
  inline uint16_t getUniversesForOutput(uint16_t idx) { return idx < numOutputs ? calcUniverses(ledsPerOutput[idx]) : 0; }
  inline const char* getTargetIP() { return targetIP; }
  inline uint16_t getChannelsPerUniverse() { return channelsPerUniverse; }
  inline uint8_t getPadMode() { return padMode; }

  // Direct array access
  inline uint16_t* getStartUniverseArray() { return startUniverse; }
  inline uint32_t* getLedsPerOutputArray() { return ledsPerOutput; }

  void setup() override {
    if (!enabled) return;
    USER_PRINTLN(F("ArtNetMap: Initializing..."));
    initDone = true;
  }

  void connected() override {
    // Nothing needed here
  }

  void loop() override {
    if (!enabled) return;

    // Register web handlers on first loop iteration (server is ready by now)
    if (!webInitDone) {
      initWeb();
    }

    // Drive test mode: send an Art-Net DMX burst to targetIP every ~25ms for 5 seconds
    if (testingOutput >= 0) {
      unsigned long now = millis();
      if (now - testStartTime > 5000) {
        testingOutput = -1;
      } else if (now - lastTestSend > 25) {
        lastTestSend = now;
        sendTestPacket();
      }
    }
  }

  void addToJsonInfo(JsonObject& root) override {
    if (!enabled) return;

    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

    String uiNameString = F("Art-Net Map");
    JsonArray infoArr = user.createNestedArray(uiNameString);

    String info = String(numOutputs) + F(" outputs, ") + String(getTotalLeds()) + F(" LEDs, ") + String(getTotalUniverses()) + F(" universes");
    infoArr.add(info);
  }

  void addToJsonState(JsonObject& root) override {
    // Not used
  }

  void readFromJsonState(JsonObject& root) override {
    // Not used
  }

  void appendConfigData() override {
    // Add link to the real config page
    oappend(SET_F("addInfo('ArtNetMap:enabled',1,'<br><a href=\"/artnetmap\" target=\"_blank\">Open Art-Net Map Configuration</a>');"));
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));

    // Only store enabled state and current preset name
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_currentPreset)] = currentPreset;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];

    if (top.isNull()) {
      USER_PRINT(FPSTR(_name));
      USER_PRINTLN(F(": No config found. (Using defaults.)"));
      return false;
    }

    enabled = top[FPSTR(_enabled)] | enabled;

    const char* preset = top[FPSTR(_currentPreset)] | "";
    if (strlen(preset) > 0) {
      strlcpy(currentPreset, preset, sizeof(currentPreset));
      // Auto-load the preset on boot
      if (loadPreset(currentPreset)) {
        USER_PRINTF("ArtNetMap: Auto-loaded preset '%s' (%d outputs).\n", currentPreset, numOutputs);
      }
    }

    USER_PRINT(FPSTR(_name));
    USER_PRINTLN(F(": Config loaded."));

    return !top[FPSTR(_enabled)].isNull();
  }

  uint16_t getId() override {
    return USERMOD_ID_ARTNETMAP;
  }

  // Register web server handlers
  void initWeb() {
    if (!enabled || webInitDone) return;

    // Use flat paths to avoid routing issues
    server.on("/artnetmap-api", HTTP_GET, [this](AsyncWebServerRequest* request) {
      handleApi(request);
      });

    server.on("/artnetmap", HTTP_GET, [this](AsyncWebServerRequest* request) {
      servePage(request);
      });

    webInitDone = true;
    USER_PRINTLN(F("ArtNetMap: Web handlers registered at /artnetmap and /artnetmap-api"));
  }
};

// String constants (inline to avoid multiple definition when header included in multiple TUs)
inline const char ArtNetMapUsermod::_name[]          PROGMEM = "ArtNetMap";
inline const char ArtNetMapUsermod::_enabled[]       PROGMEM = "enabled";
inline const char ArtNetMapUsermod::_currentPreset[] PROGMEM = "currentPreset";

#ifndef USERMOD_ID_ARTNETMAP
#define USERMOD_ID_ARTNETMAP 4200
#endif

// ============================================================================
// Web page implementation
// ============================================================================

inline void ArtNetMapUsermod::sendTestPacket() {
  if (testingOutput < 0 || (uint16_t)testingOutput >= numOutputs) return;

  // Static socket persists across loop iterations; reconnects only when target changes
  static AsyncUDP testUdp;
  static IPAddress lastTestDest((uint32_t)0);
  static uint8_t testSeq = 0;

  IPAddress dest;
  if (!dest.fromString(targetIP)) return;
  if ((uint32_t)dest != (uint32_t)lastTestDest) {
    testUdp.connect(dest, ARTNET_PORT);
    lastTestDest = dest;
  }

  uint16_t startUni = startUniverse[testingOutput];
  uint32_t leds = ledsPerOutput[testingOutput];
  uint16_t unis = calcUniverses(leds);
  if (unis == 0) return;

  uint16_t bytesPerUni = channelsPerUniverse;
  if (bytesPerUni > 512) bytesPerUni = 512;

  // ArtDmx packet: 8-byte ID "Art-Net\0", 2-byte OpDmx (LE 0x5000),
  // 2-byte ProtVer (LE 14), 1-byte Seq, 1-byte Physical,
  // 2-byte Universe (LE), 2-byte Length (LE), then DMX data.
  uint8_t packet[18 + 512];
  packet[0] = 0x41; packet[1] = 0x72; packet[2] = 0x74; packet[3] = 0x2d;
  packet[4] = 0x4e; packet[5] = 0x65; packet[6] = 0x74; packet[7] = 0x00;
  packet[8] = 0x00; packet[9] = 0x50;                          // OpDmx
  packet[10] = 0x0e; packet[11] = 0x00;                        // ProtVer 14
  packet[12] = ++testSeq;                                       // Sequence
  packet[13] = 0;                                               // Physical

  uint32_t channelsTotal = leds * 3;  // assumes RGB
  for (uint16_t u = 0; u < unis; u++) {
    uint16_t uni = startUni + u;
    uint32_t chOffset = (uint32_t)u * bytesPerUni;
    uint16_t len = bytesPerUni;
    if (chOffset >= channelsTotal) len = 0;
    else if (chOffset + len > channelsTotal) len = (uint16_t)(channelsTotal - chOffset);
    packet[14] = uni & 0xFF;
    packet[15] = (uni >> 8) & 0xFF;
    packet[16] = (len >> 8) & 0xFF;
    packet[17] = len & 0xFF;
    if (len > 0) {
      memset(packet + 18, 0xFF, len);  // white test pattern
      testUdp.write(packet, 18 + len);
    }
  }
}

inline void ArtNetMapUsermod::servePage(AsyncWebServerRequest* request) {
  AsyncResponseStream* response = request->beginResponseStream("text/html");

  response->print(F("<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Art-Net Output Map</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#1e1e1e;color:#e0e0e0;margin:0;padding:20px;line-height:1.5}"
    ".container{max-width:700px;margin:0 auto}"
    "h1{color:#ffcc00;font-size:1.4em;margin-bottom:5px}"
    ".subtitle{color:#888;font-size:0.9em;margin-bottom:25px}"
    ".panel{background:#252525;border-radius:10px;padding:20px;margin-bottom:20px}"
    ".panel-header{color:#ffcc00;font-size:0.85em;font-weight:600;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:15px}"
    ".row{display:flex;align-items:center;margin-bottom:12px;gap:10px;flex-wrap:wrap}"
    ".row label{min-width:120px;font-size:13px;color:#bbb}"
    "input,select{background:#1a1a1a;border:1px solid #444;color:#fff;padding:8px 12px;border-radius:5px;font-size:13px}"
    "input:focus,select:focus{border-color:#ffcc00;outline:none}"
    "input.narrow{width:70px}input.wide{width:180px}"
    "button{background:#ffcc00;color:#000;border:none;padding:8px 16px;border-radius:5px;font-size:13px;font-weight:600;cursor:pointer}"
    "button:hover{background:#ffe066}"
    "button.secondary{background:#3a3a3a;color:#ddd}"
    "button.secondary:hover{background:#4a4a4a}"
    "button.small{padding:5px 10px;font-size:11px}"
    ".stats{display:flex;gap:25px;padding:12px 16px;background:#1a1a1a;border-radius:6px;margin:15px 0}"
    ".stat{display:flex;flex-direction:column}"
    ".stat-label{font-size:10px;color:#888;text-transform:uppercase}"
    ".stat-value{font-size:1.3em;color:#ffcc00;font-weight:600}"
    ".quick-setup{background:#1a2a1a;border:1px solid #2a3a2a;border-radius:6px;padding:12px 15px;margin-bottom:15px}"
    ".quick-setup-header{font-size:11px;color:#6a6;font-weight:600;text-transform:uppercase;margin-bottom:10px}"
    ".quick-setup input{width:70px;text-align:center}"
    ".quick-setup input.leds-input{width:90px}"
    ".quick-setup .multiply{color:#666;font-size:18px}"
    ".table-container{max-height:350px;overflow-y:auto;border:1px solid #333;border-radius:6px}"
    "table{width:100%;border-collapse:collapse;font-size:12px}"
    "thead{position:sticky;top:0;background:#2a2a2a;z-index:1}"
    "th{text-align:left;padding:10px 8px;color:#ffcc00;font-weight:600;font-size:10px;text-transform:uppercase;border-bottom:2px solid #444}"
    "th.center{text-align:center}"
    "td{padding:6px 8px;border-bottom:1px solid #2a2a2a}"
    "tr:hover{background:#2a2a2a}"
    ".output-num{color:#666;font-weight:600;font-size:11px}"
    ".output-name{color:#aaa;font-size:11px}"
    "table input{padding:5px 8px;font-size:12px}"
    "table input.uni-input{width:65px;text-align:center}"
    "table input.led-input{width:90px;text-align:center}"
    ".uni-range{font-family:monospace;color:#888;font-size:11px}"
    ".test-btn{padding:3px 8px;font-size:10px;background:#333;color:#aaa}"
    ".test-btn:hover{background:#ffcc00;color:#000}"
    ".preset-row{display:flex;gap:8px;align-items:center;margin-top:15px;padding-top:15px;border-top:1px solid #333}"
    ".preset-row select{flex:1;max-width:180px}"
    ".footer-actions{display:flex;justify-content:space-between;align-items:center;margin-top:20px;padding-top:15px;border-top:1px solid #333}"
    ".help{font-size:11px;color:#666;margin-top:4px}"
    ".divider{height:1px;background:#333;margin:15px 0}"
    "</style></head><body>"
    "<div class='container'>"
    "<h1>Art-Net Output Map</h1>"
    "<p class='subtitle'>Configure universe mapping for Art-Net LED outputs</p>"));

  // Global Settings Panel
  response->print(F("<div class='panel'><div class='panel-header'>Global Settings</div>"
    "<div class='row'><label>Target IP:</label>"
    "<input type='text' id='targetIP' value='"));
  response->print(targetIP);
  response->print(F("' class='wide'></div>"
    "<p class='help'>Use .255 for broadcast, or specific IP for unicast</p>"
    "<div class='row'><label>Channels/Universe:</label>"
    "<select id='channelsPerUni'>"
    "<option value='510'"));
  if (channelsPerUniverse == 510) response->print(F(" selected"));
  response->print(F(">510 (RGB standard)</option><option value='512'"));
  if (channelsPerUniverse == 512) response->print(F(" selected"));
  response->print(F(">512 (RGBW / Advatek)</option></select></div>"
    "<div class='row'><label>Pad Universe Gaps:</label>"
    "<select id='padMode'>"
    "<option value='0'"));
  if (padMode == 0) response->print(F(" selected"));
  response->print(F(">None</option><option value='1'"));
  if (padMode == 1) response->print(F(" selected"));
  response->print(F(">Black pixel per universe</option><option value='2'"));
  if (padMode == 2) response->print(F(" selected"));
  response->print(F(">Full black universe</option></select></div>"
    "</div>"));

  // Multi-Output Panel
  response->print(F("<div class='panel'>"
    "<div class='panel-header'>Output Configuration</div>"
    "<div class='quick-setup'>"
    "<div class='quick-setup-header'>⚡ Quick Setup</div>"
    "<div class='row' style='margin-bottom:0'>"
    "<input type='number' id='qsOutputs' value='28' min='1' max='"));
  response->print(ARTNETMAP_MAX_OUTPUTS);
  response->print(F("'>"
    "<span class='multiply'>×</span>"
    "<input type='number' id='qsUniverses' value='6' min='1' max='32'>"
    "<span style='color:#888;font-size:12px'>universes</span>"
    "<span class='multiply'>@</span>"
    "<input type='number' id='qsLeds' value='1020' min='1' class='leds-input'>"
    "<span style='color:#888;font-size:12px'>LEDs</span>"
    "<button class='small' onclick='generate()'>Generate</button>"
    "<button class='small secondary' onclick='calcMax()'>Max LEDs</button>"
    "</div></div>"
    "<div class='stats'>"
    "<div class='stat'><span class='stat-label'>Outputs</span><span class='stat-value' id='statOutputs'>"));
  response->print(numOutputs);
  response->print(F("</span></div>"
    "<div class='stat'><span class='stat-label'>Universes</span><span class='stat-value' id='statUniverses'>"));
  response->print(getTotalUniverses());
  response->print(F("</span></div>"
    "<div class='stat'><span class='stat-label'>Total LEDs</span><span class='stat-value' id='statLeds'>"));
  response->print(getTotalLeds());
  response->print(F("</span></div></div>"
    "<div class='table-container'><table><thead><tr>"
    "<th style='width:35px'>#</th>"
    "<th>Name</th>"
    "<th style='width:75px' class='center'>Start</th>"
    "<th style='width:100px' class='center'>LEDs</th>"
    "<th style='width:90px'>Range</th>"
    "<th style='width:50px'></th>"
    "</tr></thead><tbody id='outputTable'>"));

  // Output rows
  for (uint16_t i = 0; i < numOutputs; i++) {
    uint16_t unis = calcUniverses(ledsPerOutput[i]);
    uint16_t endUni = startUniverse[i] + unis - 1;
    response->printf("<tr><td class='output-num'>%d</td>", i + 1);
    response->printf("<td class='output-name'>Output %d (%d-%d)</td>", i + 1, startUniverse[i], endUni);
    response->printf("<td><input type='number' class='uni-input' value='%d' onchange='updateRow(%d,this.value,null)'></td>", startUniverse[i], i);
    response->printf("<td><input type='number' class='led-input' value='%lu' onchange='updateRow(%d,null,this.value)'></td>", (unsigned long)ledsPerOutput[i], i);
    response->printf("<td class='uni-range'>U%d-%d</td>", startUniverse[i], endUni);
    response->printf("<td><button class='test-btn' onclick='testOutput(%d)'>Test</button></td></tr>", i);
  }

  response->print(F("</tbody></table></div>"));

  // Presets
  response->print(F("<div class='preset-row'><select id='presetSelect'><option value=''>— Load Preset —</option>"));

  // List presets from filesystem
  File root = WLED_FS.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (name.startsWith("artnetmap_") && name.endsWith(".json")) {
      String preset = name.substring(10, name.length() - 5);
      response->print(F("<option value='"));
      response->print(preset);
      response->print(F("'>"));
      response->print(preset);
      response->print(F("</option>"));
    }
    file = root.openNextFile();
  }

  response->print(F("</select>"
    "<button class='small secondary' onclick='loadPreset()'>Load</button>"
    "<span style='color:#444'>|</span>"
    "<input type='text' id='presetName' placeholder='Save as...' style='width:120px'>"
    "<button class='small' onclick='savePreset()'>Save</button>"
    "</div></div>"));

  // Footer
  response->print(F("<div class='footer-actions'>"
    "<div><button class='secondary' onclick='stopTest()'>Stop Test</button></div>"
    "<button onclick='apply()'>Apply Configuration</button>"
    "</div></div>"));

  // JavaScript
  response->print(F("<script>"
    "function calcMax(){"
    "var u=parseInt(document.getElementById('qsUniverses').value)||6;"
    "var ch=parseInt(document.getElementById('channelsPerUni').value)||510;"
    "document.getElementById('qsLeds').value=u*Math.floor(ch/3);}"
    "function generate(){"
    "var c=parseInt(document.getElementById('qsOutputs').value)||1;"
    "var u=parseInt(document.getElementById('qsUniverses').value)||6;"
    "var l=parseInt(document.getElementById('qsLeds').value)||170;"
    "fetch('/artnetmap-api?a=gen&c='+c+'&u='+u+'&l='+l).then(r=>r.json()).then(d=>{if(d.ok)location.reload();});}"
    "function updateRow(i,uni,leds){"
    "var q='a=upd&i='+i;"
    "if(uni!==null)q+='&u='+uni;"
    "if(leds!==null)q+='&l='+leds;"
    "fetch('/artnetmap-api?'+q).then(r=>r.json()).then(d=>{if(d.ok)location.reload();});}"
    "function testOutput(i){fetch('/artnetmap-api?a=test&i='+i);}"
    "function stopTest(){fetch('/artnetmap-api?a=stop');}"
    "function loadPreset(){"
    "var p=document.getElementById('presetSelect').value;"
    "if(!p)return;"
    "fetch('/artnetmap-api?a=load&n='+encodeURIComponent(p)).then(r=>r.json()).then(d=>{if(d.ok)location.reload();});}"
    "function savePreset(){"
    "var n=document.getElementById('presetName').value.trim();"
    "if(!n){alert('Enter name');return;}"
    "var ip=document.getElementById('targetIP').value;"
    "var ch=document.getElementById('channelsPerUni').value;"
    "var pad=document.getElementById('padMode').value;"
    "fetch('/artnetmap-api?a=save&n='+encodeURIComponent(n)+'&ip='+encodeURIComponent(ip)+'&ch='+ch+'&pad='+pad)"
    ".then(r=>r.json()).then(d=>{if(d.ok)location.reload();});}"
    "function apply(){"
    "var ip=document.getElementById('targetIP').value;"
    "var ch=document.getElementById('channelsPerUni').value;"
    "var pad=document.getElementById('padMode').value;"
    "fetch('/artnetmap-api?a=apply&ip='+encodeURIComponent(ip)+'&ch='+ch+'&pad='+pad)"
    ".then(r=>r.json()).then(d=>{if(d.ok)alert('Applied!');});}"
    "</script></body></html>"));

  request->send(response);
}

// ============================================================================
// API implementation
// ============================================================================

inline void ArtNetMapUsermod::handleApi(AsyncWebServerRequest* request) {
  String action = request->arg("a");

  StaticJsonDocument<256> doc;
  doc["ok"] = true;

  if (action == "gen") {
    uint16_t count = request->arg("c").toInt();
    uint16_t unis = request->arg("u").toInt();
    uint32_t leds = strtoul(request->arg("l").c_str(), NULL, 10);
    generateSequential(count, unis, leds);
    USER_PRINTF("ArtNetMap: Generated %d outputs with %lu LEDs each\n", count, leds);
  } else if (action == "upd") {
    uint16_t idx = request->arg("i").toInt();
    if (idx < numOutputs) {
      if (request->hasArg("u")) startUniverse[idx] = request->arg("u").toInt();
      if (request->hasArg("l")) ledsPerOutput[idx] = strtoul(request->arg("l").c_str(), NULL, 10);
    }
  } else if (action == "test") {
    testingOutput = request->arg("i").toInt();
    testStartTime = millis();
    USER_PRINTF("ArtNetMap: Testing output %d\n", testingOutput);
  } else if (action == "stop") {
    testingOutput = -1;
  } else if (action == "load") {
    String name = request->arg("n");
    if (!loadPreset(name.c_str())) {
      doc["ok"] = false;
      doc["err"] = "Load failed";
    } else {
      USER_PRINTF("ArtNetMap: Loaded preset '%s'\n", name.c_str());
      serializeConfig();  // Save currentPreset to WLED config
    }
  } else if (action == "save") {
    String name = request->arg("n");
    if (request->hasArg("ip")) strlcpy(targetIP, request->arg("ip").c_str(), sizeof(targetIP));
    if (request->hasArg("ch")) channelsPerUniverse = request->arg("ch").toInt();
    if (request->hasArg("pad")) padMode = request->arg("pad").toInt();
    if (!savePreset(name.c_str())) {
      doc["ok"] = false;
      doc["err"] = "Save failed";
    } else {
      USER_PRINTF("ArtNetMap: Saved preset '%s'\n", name.c_str());
      serializeConfig();  // Save currentPreset to WLED config
    }
  } else if (action == "apply") {
    if (request->hasArg("ip")) strlcpy(targetIP, request->arg("ip").c_str(), sizeof(targetIP));
    if (request->hasArg("ch")) channelsPerUniverse = request->arg("ch").toInt();
    if (request->hasArg("pad")) padMode = request->arg("pad").toInt();
    // The OUTPUT sender in udp.cpp reads getStartUniverse()/getLedsPerOutput() on every
    // realtime push, so in-memory edits take effect on the next frame. Only persistence
    // needs an explicit call here, so a reboot doesn't roll back to the old preset.
    serializeConfig();
    USER_PRINTLN(F("ArtNetMap: Configuration applied."));
  } else if (action == "get") {
    doc["n"] = numOutputs;
    doc["ip"] = targetIP;
    doc["ch"] = channelsPerUniverse;
    doc["pad"] = padMode;
    JsonArray uArr = doc.createNestedArray("u");
    JsonArray lArr = doc.createNestedArray("l");
    for (uint16_t i = 0; i < numOutputs; i++) {
      uArr.add(startUniverse[i]);
      lArr.add(ledsPerOutput[i]);
    }
  } else {
    doc["ok"] = false;
    doc["err"] = "Unknown action";
  }

  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
}