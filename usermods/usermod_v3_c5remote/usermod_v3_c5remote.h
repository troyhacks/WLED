/*
 * usermod_v3_c5remote.h - the ESP32-C5 remote.
 *
 * The board is a WaveShare ESP32-C5-LCD-2.73: an ESP32-C5-WROOM-1-N32R8
 * with a 2.73" 320x320 ILI9488 panel, a CH32V003 I/O expander and a
 * QMI8658 IMU.  It runs WLED, like every other board here, and it is a
 * RECEIVER: the AudioReactive usermod is built in "network receive only"
 * mode, so the board needs no audio hardware at all.  It listens for the
 * audio sync packets on 239.0.0.1:11988 and shows who is sending them.
 *
 * WHAT IS ON THE SCREEN
 * ---------------------
 *   SYNC   the boards that are sending audio sync packets, how many packets
 *          each of them has sent, how fast, and how long ago - and the
 *          sixteen bands of the last packet as a meter.
 *   BEAT   the tempo of the board that has the tracker (the P4): the rate,
 *          where the beat is, how good the estimate is, and how far the
 *          grid has drifted.
 *   BOARD  this board: the network, the uptime, the memory, the IMU and the
 *          expander's battery reading.
 *
 * THE BUTTONS
 * -----------
 * Three on the chip (GPIO2 previous, GPIO3 next, GPIO28 the boot button)
 * and up to eleven on the IO expander, which are answered for by this
 * usermod so that the core's own button logic - the debounce, the long
 * press, the double press and the macros - does all of it.
 *
 * The macros are ACTIONS, not presets: a macro at or above
 * WLED_BTN_ACTION_BASE is offered to the usermods, and the one that owns
 * it does it.  The tempo's actions (mark the downbeat, lock the clock,
 * re-lock, change the mode) are forwarded to the board that has the
 * tracker, because this board has no tracker of its own.
 */

#pragma once

#include "wled.h"

#if defined(WLEDMM_C5_DISPLAY)

#include "c5_board.h"
#include "c5_panel.h"

// The Zigbee radio.
//
// The Arduino core ships the whole Zigbee stack for the ESP32-C5 - ZBOSS,
// the headers and the compiled libraries - so this is a build flag
// (-DZIGBEE_MODE_ED) and a partition scheme, and the endpoints below are
// the core's own Zigbee library.
//
// The board joins the network as an END DEVICE.  Espressif rates a router
// next to Wi-Fi as "supported but performance is unstable" - a router keeps
// unsynchronised links with its neighbours and wants to hear them all the
// time - and this board is serving WLED's Wi-Fi the whole while.
#if defined(USERMOD_ZIGBEE)

#define C5_ZIGBEE_EP_TEMPO   1   // the tempo, as an analog input
#define C5_ZIGBEE_EP_MOTION  2   // motion, as an occupancy sensor

#include "Zigbee.h"


// The endpoints. The tempo is an analog input (Home Assistant has a
// number to show), and the motion is an occupancy sensor (which is the one
// thing the Zigbee clusters model well).
static ZigbeeAnalog          c5ZigbeeTempo  = ZigbeeAnalog(C5_ZIGBEE_EP_TEMPO);
static ZigbeeOccupancySensor c5ZigbeeMotion = ZigbeeOccupancySensor(C5_ZIGBEE_EP_MOTION);
#endif

// The HTTP client.
//
// The Arduino HTTPClient is not in this board's build: its library declares
// the "esp32" architecture and the dependency finder leaves it out for the
// C5. So the client's own is used - ESP-IDF's esp_http_client, which is
// part of the chip's libraries and is there for every ESP32.
#include "esp_http_client.h"

// The actions this usermod owns, on top of the tempo's (which are in
// wled00/const.h, because the AudioReactive usermod owns those too).
#define C5_ACT_PAGE_PREV    (WLED_BTN_ACTION_BASE + 20)
#define C5_ACT_PAGE_NEXT    (WLED_BTN_ACTION_BASE + 21)
#define C5_ACT_PRESET_NEXT  (WLED_BTN_ACTION_BASE + 22)
#define C5_ACT_PRESET_PREV  (WLED_BTN_ACTION_BASE + 23)
#define C5_ACT_BRIGHT_UP    (WLED_BTN_ACTION_BASE + 24)
#define C5_ACT_BRIGHT_DOWN  (WLED_BTN_ACTION_BASE + 25)

// The pages
#define C5_PAGE_SYNC  0
#define C5_PAGE_BEAT  1
#define C5_PAGE_BOARD 2
#define C5_PAGE_TEST  3
#define C5_PAGE_COUNT 4

// The first button index the expander's buttons take.  The three on the
// chip come first.
//
// The expander's buttons are NOT given to the core: the core skips a
// button whose pin is negative, and a button that IS given a pin has
// that pin read - and neither works for a button that lives on the
// expander.  This usermod reads them itself.
#define C5_EXPANDER_BTN_FIRST 3

// How long a button on the expander has to be held down for its long
// press to fire.
#define C5_EXPANDER_LONG_MS 800

class C5RemoteUsermod : public Usermod {
  public:
    C5RemoteUsermod();
    void setup() override;
    void loop() override;

    // The expander's buttons: answered for here, so the core leaves them
    // alone.  The debounce and the long press are done here too, because
    // the core never gets that far with a button that has no pin.
    bool handleButton(uint8_t index) override;

    // The actions: the tempo's are forwarded to the board that has the
    // tracker, the rest are this board's own.
    bool handleButtonAction(uint16_t action) override;
    bool performAction(uint16_t action);

    void addToJsonAr(JsonObject &obj) override;
    void addToJsonState(JsonObject &obj) override;
    void readFromJsonState(JsonObject &obj) override;
    void addToConfig(JsonObject &obj) override;
    bool readFromConfig(JsonObject &obj) override;
    void appendConfigData() override;

    uint16_t getId() override { return USERMOD_ID_C5REMOTE; }

  private:
    // ---- the panel and the hardware ----
    C5Panel      _panel;
    C5IoExpander _ioexp;
    C5Imu        _imu;
    bool         _ioexpOk = false;
    bool         _imuOk = false;
    uint16_t     _ioInputs = 0;      // the expander's input register
    uint16_t     _ioInputsStable = 0; // and the same, once it has settled
    uint8_t      _ioStableCount = 0;
    bool         _ioActiveHigh = false;  // are the expander's buttons active high?
    // What each of the expander's buttons does: a short press, and a long
    // one.  The board's header has buttons on it, and what they do is the
    // owner's business - so they are empty until they are told.
    uint16_t     _expanderAction[C5_IOEXP_BTN_COUNT] = {0};
    uint16_t     _expanderLongAction[C5_IOEXP_BTN_COUNT] = {0};
    uint8_t     *_bmp = nullptr;         // the screen's picture, for the HTTP snapshot
    uint8_t      _wifiBand = 0;          // 0 = both bands, 1 = 5 GHz only, 2 = 2.4 GHz only

    // ---- the pages ----
    uint8_t _page = C5_PAGE_SYNC;
    bool    _redraw = true;
    uint32_t _lastDraw = 0;
    uint32_t _drawMs = 0;      // how long the last drawing took

    // ---- the board that has the tracker ----
    String   _p4Host = "192.168.6.249";
    uint16_t _p4Port = 80;
    uint32_t _lastPoll = 0;
    bool     _p4Online = false;
    String   _p4Status;
    uint32_t _p4Failures = 0;

    // What the board with the tracker says.
    bool    _tempoUsable = false;
    float   _bpm = 0.0f;
    int     _beatInBar = 0;
    float   _beatProgress = 0.0f;
    float   _quality = 0.0f;
    float   _driftMs = 0.0f;
    bool    _hold = false;
    bool    _downbeatSet = false;
    String  _modeName = "";
    int     _preset = 0;
    int     _p4Brightness = 0;
    String  _p4Name = "";

    // ---- what the buttons did ----
    String  _lastAction;
    uint32_t _lastActionMs = 0;
    uint16_t _pendingAction = 0;     // an action asked for elsewhere
    bool    _pendingActionSet = false;

    // ---- the Zigbee radio ----
#if defined(USERMOD_ZIGBEE)
    bool    _zigbeeStarted = false;
    bool    _zigbeeMotion = false;
    float   _zigbeeMotionMag = 0.0f;
    uint32_t _zigbeeLastTempo = 0;
    void    setupZigbee();
    void    pollZigbee();
#endif

    // ---- the helpers ----
    void setupButtons();
    void pollExpander();
    void pollTracker();
    void applyWifiBand();
    void connected() override;
    bool trackerPost(const char *key, bool value);
    bool trackerPostRaw(const char *body);
    bool trackerPreset(int preset);
    bool trackerBrightness(int value);
    String senderName(uint32_t ip);
    void drawSyncPage();
    void drawBeatPage();
    void drawBoardPage();
    void drawTestPage();
    void drawHeader(const char *title, const char *right);
    void drawFooter(const char *left, const char *right);
    void drawBars(const uint8_t *bands, int x, int y, int w, int h);
    void noteAction(const char *what);
};

#include "usermod_v3_c5remote.cpp"

#endif  // WLEDMM_C5_DISPLAY
