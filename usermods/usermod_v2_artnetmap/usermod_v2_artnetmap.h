#pragma once

/*
 * Art-Net Output Map Usermod
 *
 * Provides configuration for Art-Net output mapping with:
 * - Quick setup (X outputs × Y universes × Z LEDs)
 * - Per-output start universe and LED count
 * - All config stored in cfg.json (no external files)
 *
 * Data model:
 *   startUniverse[i] = first universe for output i
 *   ledsPerOutput[i] = total LEDs on output i (spans ceil(leds/170) universes)
 *
 * Settings page at: http://[WLED_IP]/settings_artnetmap
 *
 * CONFIG_FIELDS: enabled=false, targetIP="255.255.255.255", channelsPerUniverse=510, padMode=0, outputs=[]
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
  int16_t testingOutput = -1;
  unsigned long testStartTime = 0;

  // String constants
  static const char _name[];
  static const char _enabled[];
  static const char _targetIP[];
  static const char _channelsPerUniverse[];
  static const char _padMode[];
  static const char _outputs[];
  static const char _startUniverse[];
  static const char _leds[];

  // Calculate universes needed for LED count
  uint16_t calcUniverses(uint32_t leds) {
    if (leds == 0) return 0;
    uint32_t ledsPerUni = channelsPerUniverse / 3;
    return (leds + ledsPerUni - 1) / ledsPerUni;
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

  // Generate sequential outputs
  void generateSequential(uint16_t count, uint16_t universesPerOutput, uint32_t leds) {
    numOutputs = min((uint16_t)ARTNETMAP_MAX_OUTPUTS, count);
    for (uint16_t i = 0; i < numOutputs; i++) {
      startUniverse[i] = i * universesPerOutput;
      ledsPerOutput[i] = leds;
    }
  }

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

    // Handle test timeout (5 seconds)
    if (testingOutput >= 0) {
      if (millis() - testStartTime > 5000) {
        testingOutput = -1;
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

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));

    // Store all settings
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_targetIP)] = targetIP;
    top[FPSTR(_channelsPerUniverse)] = channelsPerUniverse;
    top[FPSTR(_padMode)] = padMode;

    // Store outputs array
    JsonArray outputsArr = top.createNestedArray(FPSTR(_outputs));
    for (uint16_t i = 0; i < numOutputs; i++) {
      JsonObject out = outputsArr.createNestedObject();
      out[FPSTR(_startUniverse)] = startUniverse[i];
      out[FPSTR(_leds)] = ledsPerOutput[i];
    }
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];

    if (top.isNull()) {
      USER_PRINT(FPSTR(_name));
      USER_PRINTLN(F(": No config found. (Using defaults.)"));
      return false;
    }

    enabled = top[FPSTR(_enabled)] | enabled;

    // Load global settings
    const char* ip = top[FPSTR(_targetIP)] | "255.255.255.255";
    strlcpy(targetIP, ip, sizeof(targetIP));
    channelsPerUniverse = top[FPSTR(_channelsPerUniverse)] | 510;
    padMode = top[FPSTR(_padMode)] | 0;

    // Load outputs array
    JsonArray outputsArr = top[FPSTR(_outputs)];
    if (!outputsArr.isNull()) {
      numOutputs = outputsArr.size();
      if (numOutputs > ARTNETMAP_MAX_OUTPUTS) numOutputs = ARTNETMAP_MAX_OUTPUTS;
      for (uint16_t i = 0; i < numOutputs; i++) {
        JsonObject out = outputsArr[i];
        if (!out.isNull()) {
          startUniverse[i] = out[FPSTR(_startUniverse)] | (i * 16 + 1);
          ledsPerOutput[i] = out[FPSTR(_leds)] | 170;
        }
      }
    }

    USER_PRINT(FPSTR(_name));
    USER_PRINTLN(F(": Config loaded."));

    return !top[FPSTR(_enabled)].isNull();
  }

  uint16_t getId() override {
    return USERMOD_ID_ARTNETMAP;
  }
};

// String constants (inline to avoid multiple definition when header included in multiple TUs)
inline const char ArtNetMapUsermod::_name[] PROGMEM = "ArtNetMap";
inline const char ArtNetMapUsermod::_enabled[] PROGMEM = "enabled";
inline const char ArtNetMapUsermod::_targetIP[] PROGMEM = "targetIP";
inline const char ArtNetMapUsermod::_channelsPerUniverse[] PROGMEM = "channelsPerUniverse";
inline const char ArtNetMapUsermod::_padMode[] PROGMEM = "padMode";
inline const char ArtNetMapUsermod::_outputs[] PROGMEM = "outputs";
inline const char ArtNetMapUsermod::_startUniverse[] PROGMEM = "startUniverse";
inline const char ArtNetMapUsermod::_leds[] PROGMEM = "leds";

#ifndef USERMOD_ID_ARTNETMAP
#define USERMOD_ID_ARTNETMAP 4200
#endif
