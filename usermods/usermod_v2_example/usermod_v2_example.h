#pragma once

/*
 * Example Usermod - Settings Page Demo
 *
 * This usermod demonstrates the auto-discovery settings page system.
 * It provides a minimal example of:
 * - Adding a settings page at /settings_example
 * - Declaring expected config fields in C++ with defaults
 * - Loading/saving settings via cfg.json
 * - Validating config fields on the settings page
 *
 * CONFIG_FIELDS: enabled=false, debugMode=false, myColor="#ff0000", mySpeed=100
 */

#include "wled.h"

#ifndef EXAMPLE_MAX_STRING
#define EXAMPLE_MAX_STRING 32
#endif

class ExampleUsermod : public Usermod {

private:

  // Config fields
  bool enabled = false;
  bool debugMode = false;
  char myColor[EXAMPLE_MAX_STRING] = "#ff0000";
  uint16_t mySpeed = 100;

  // String constants - must match CONFIG_FIELDS
  static const char _name[];
  static const char _enabled[];
  static const char _debugMode[];
  static const char _myColor[];
  static const char _mySpeed[];

public:

  ExampleUsermod(bool enabled) : Usermod("Example", enabled) {}

  // Getters
  inline bool isEnabled() { return enabled; }
  inline bool isDebugMode() { return debugMode; }
  inline const char* getMyColor() { return myColor; }
  inline uint16_t getMySpeed() { return mySpeed; }

  void setup() override {
    if (!enabled) return;
    USER_PRINTLN(F("ExampleUsermod: Initialized"));
  }

  void loop() override {
    if (!enabled) return;
    // Your loop code here
  }

  void addToJsonInfo(JsonObject& root) override {
    if (!enabled) return;
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    user["Example"] = "Speed: " + String(mySpeed) + " | Color: " + String(myColor);
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;
    top[FPSTR(_debugMode)] = debugMode;
    top[FPSTR(_myColor)] = myColor;
    top[FPSTR(_mySpeed)] = mySpeed;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) {
      USER_PRINT(FPSTR(_name));
      USER_PRINTLN(F(": No config found."));
      return false;
    }

    enabled = top[FPSTR(_enabled)] | enabled;
    debugMode = top[FPSTR(_debugMode)] | debugMode;

    const char* color = top[FPSTR(_myColor)] | "#ff0000";
    strlcpy(myColor, color, sizeof(myColor));

    mySpeed = top[FPSTR(_mySpeed)] | mySpeed;

    USER_PRINT(FPSTR(_name));
    USER_PRINTLN(F(": Config loaded."));
    return !top[FPSTR(_enabled)].isNull();
  }

  uint16_t getId() override {
    return USERMOD_ID_EXAMPLE;
  }
};

// String constants
inline const char ExampleUsermod::_name[] PROGMEM = "Example";
inline const char ExampleUsermod::_enabled[] PROGMEM = "enabled";
inline const char ExampleUsermod::_debugMode[] PROGMEM = "debugMode";
inline const char ExampleUsermod::_myColor[] PROGMEM = "myColor";
inline const char ExampleUsermod::_mySpeed[] PROGMEM = "mySpeed";

#ifndef USERMOD_ID_EXAMPLE
#define USERMOD_ID_EXAMPLE 4201
#endif
