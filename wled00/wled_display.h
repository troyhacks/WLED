#pragma once
// The display subsystem.
//
// The P4's MIPI-DSI connector takes a panel, or an LT8912B bridge to HDMI.
// Which one is attached is not known at build time - it is worked out at run
// time instead: the settings page picks it, and the board can also look at
// the I2C bus and suggest one.
//
// Everything about an output - its geometry, its timing, its touch - comes
// out of the table in wled_display_panels.h.  The picture is scaled into
// whatever frame the output has, in ONE pass, and centred.

#if defined(CONFIG_IDF_TARGET_ESP32P4)

#include "src/dependencies/json/ArduinoJson-v6.h"

// What kind of output is attached.
enum display_kind_t : uint8_t {
  DISPLAY_NONE = 0,   // nothing is there
  DISPLAY_DSI,        // a MIPI-DSI panel on the connector
  DISPLAY_HDMI        // an LT8912B bridge, and a monitor behind it
};

// Everything the GUI needs to know about the display, in one place.
struct display_status_t {
  display_kind_t kind;
  const char    *name;      // the panel's name, or the HDMI mode's
  uint16_t       width;
  uint16_t       height;
  bool           present;   // detected, and working
  bool           interface_shown;
  uint8_t        rotation;  // 0, 90, 180 or 270
  bool           can_rotate;
  uint8_t        buttons;   // the on-screen buttons
  uint8_t        button_cols;
};

// The picture the display shows.  A rectangle in the panel's own pixels.
struct display_rect_t {
  int x, y, w, h;
};

// ── The lifecycle ────────────────────────────────────────────────────────────
void displaySetup();                 // boot: the defaults, before the config is read
void displayStart();                 // boot: bring the output up, once the config is known
void displayLoop();                  // every frame: the picture, the interface, the touch
void displayReconfigure();           // apply a change that was asked for
bool displayReconfigurePending();    // is there one waiting?

// ── What the output should be ────────────────────────────────────────────────
void displaySetPanel(const char* name);   // "" = work it out from the bus
void displaySetHdmiMode(const char* name);
void displaySetView(uint8_t view);
void displaySetInterface(bool on);        // the on-screen interface, on or off
void displaySetRotation(uint8_t degrees);
void displayRotateStep();                 // 0 -> 90 -> 180 -> 270
void displaySetButtons(uint8_t count, uint8_t cols);

// ── Pointing at the screen ───────────────────────────────────────────────────
// A mouse (over the HID usermod) and a finger (on the touch panel) both end
// up here, so a click behaves the same either way.
void displayPointerMove(int x, int y);
void displayPointerButton(bool down);
void displayPointerRelease();

// ── What the GUI asks ────────────────────────────────────────────────────────
void displayGetStatus(display_status_t& out);
void displayStatusJson(JsonObject& obj);   // the /json and the websocket frame
uint16_t displayWidth();
uint16_t displayHeight();

// ── What the boot needs to know ──────────────────────────────────────────────
// Is there anything on the MIPI-DSI connector?  The answer decides who owns
// LDO channel 3: the display's stack needs it at 2.5V for the MIPI DSI PHY,
// while a board without a display wants the 3.3V rail for the high GPIOs.
bool displayNeedsLdo();

// Is there a touch controller on the bus?  Every panel in the table has one,
// so this is how a panel is spotted.
bool displayTouchControllerPresent();

#endif // CONFIG_IDF_TARGET_ESP32P4
