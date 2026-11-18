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
#define C5_PAGE_BUTTONS 3
#define C5_PAGE_PACKETS 4
#define C5_PAGE_TEST  5
#define C5_PAGE_COUNT 6

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

// The buttons, for the debug.
//
// The three on the chip are the board's own: KEY_L is GPIO2, KEY_R is
// GPIO3 and BOOT is GPIO28 - the board's schematic says so, and all three
// are pulled up with 10K and pull the pin down when they are pressed.
//
// The board's fourth button, PWR, is NOT on a GPIO of this chip: it goes
// to the chip that latches the board's power on, so there is nothing to
// read and nothing to show.
//
// The eleven on the IO expander (IO4..IO14) are on the header, so they are
// not buttons until something is wired to them.
#define C5_BTN_CHIP_COUNT 3
#define C5_BTN_TOTAL (C5_BTN_CHIP_COUNT + C5_IOEXP_BTN_COUNT)

// How many of the last events the log keeps.
#define C5_BTN_LOG_LEN 8

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
    C5SdCard     _sd;
    bool         _ioexpOk = false;
    bool         _imuOk = false;
    uint16_t     _ioInputs = 0;      // the expander's input register
    uint16_t     _ioInputsStable = 0; // and the same, once it has settled
    uint8_t      _ioStableCount = 0;
    bool         _ioPrimed = false;  // has the expander's answer been believed yet?
    bool         _ioActiveHigh = false;  // are the expander's buttons active high?
    // What each of the expander's buttons does: a short press, and a long
    // one.  The board's header has buttons on it, and what they do is the
    // owner's business - so they are empty until they are told.
    uint16_t     _expanderAction[C5_IOEXP_BTN_COUNT] = {0};
    uint16_t     _expanderLongAction[C5_IOEXP_BTN_COUNT] = {0};
    uint8_t     *_bmp = nullptr;         // the screen's picture, for the HTTP snapshot
    uint8_t      _wifiBand = 0;          // 0 = both bands, 1 = 5 GHz only, 2 = 2.4 GHz only

    // ---- how long it takes to come up ----
    //
    // The times are from the app's own start - millis() is zero when the
    // app starts - so they say what the board did with its own time.  The
    // time BEFORE that (the ROM, the bootloader) is only visible from
    // outside, and the difference between the two is the interesting
    // part.
    bool     _bandSet = false;  // has the radio's band been set?
    uint32_t _wifiMs = 0;     // the app's start to the Wi-Fi connected
    uint32_t _setupMs = 0;    // the app's start to the end of the setup
    bool     _rebootAsked = false;

    // ---- the pages ----
    uint8_t _page = C5_PAGE_SYNC;
    uint8_t _drawnPage = 0xFF;   // the page the screen last showed
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
    // The beat, as the board with the tracker last reported it, and WHEN.
    //
    // The answer is a few seconds old by the time it is on the screen, so
    // the beat is counted ON here from the answer and the rate: the beat's
    // period is known, so where in the beat we are can be worked out and
    // kept up to date between the polls.
    uint32_t _tempoAtMs = 0;       // when the answer arrived
    uint32_t _tempoBeatCount = 0;  // the beat count it gave
    int     _tempoBeatInBar = 0;   // the beat in the bar it gave
    float   _tempoProgress = 0.0f; // the place in the beat it gave
    int     _preset = 0;
    int     _p4Brightness = 0;
    String  _p4Name = "";

    // ---- what the buttons did ----
    String  _lastAction;
    uint32_t _lastActionMs = 0;
    uint16_t _pendingAction = 0;     // an action asked for elsewhere
    bool    _pendingActionSet = false;

    // ---- the button debug ----
    //
    // Every button is watched, not just the ones that do something: the
    // point of the debug is to see what the board sees.
    //
    // The three on the chip are read here as well as by the core - the
    // core does the actions, and this is what says what was pressed.  The
    // eleven on the expander are read here ALONE: the core skips a button
    // whose pin is negative, so it never gets as far as asking about one.
    struct C5BtnEvent {
      uint8_t  id = 0;      // 0..2 the chip's, 3.. the expander's
      uint8_t  kind = 0;    // 0 = down, 1 = up, 2 = held
      uint32_t heldMs = 0;  // how long it was held
      uint32_t ms = 0;      // when it happened
    };
    bool       _btnDown[C5_BTN_TOTAL] = {false};
    uint32_t   _btnDownSince[C5_BTN_TOTAL] = {0};
    bool       _btnHeldFired[C5_BTN_TOTAL] = {false};
    C5BtnEvent _btnLog[C5_BTN_LOG_LEN];
    uint8_t    _btnLogHead = 0;    // where the next one goes
    uint8_t    _btnLogCount = 0;  // how many there are
    uint32_t   _btnPresses = 0;    // and how many presses there have been
    uint8_t    _pageBeforeDebug = C5_PAGE_SYNC;

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
    // The newest sender: its packets, its bands, and when they arrived.
    //
    // The table is written in the network's task and read here, so it is
    // read under its lock.
    struct C5SenderInfo {
      uint32_t ip = 0;
      uint32_t packets = 0;
      uint32_t rate = 0;
      uint32_t lastMs = 0;
      uint8_t  level = 0;
      uint8_t  bands[16] = {0};
      uint8_t  timeCount = 0;
      uint32_t times[AR_SYNC_CADENCE_LEN] = {0};
    };
    bool readNewestSender(C5SenderInfo &out);
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
    void drawButtonPage();
    void drawPacketsPage();
    void clearPageArea(int y0 = 0, int y1 = 130);
    void drawHeader(const char *title, const char *right);
    void drawFooter(const char *left, const char *right);
    void drawBars(const uint8_t *bands, int x, int y, int w, int h);
    void noteAction(const char *what);
    void setPage(uint8_t p);
    // ---- the buttons ----
    void pollButtons();
    void noteButton(uint8_t id, bool pressed, uint32_t now);
    void logButton(uint8_t id, uint8_t kind, uint32_t heldMs, uint32_t now);
    void primeExpanderButtons(uint16_t inputs);
    // Where the beat is NOW, worked out from the last answer.
    void beatNow(float &progress, int &beatInBar, uint32_t &beatCount);
};

#include "usermod_v3_c5remote.cpp"

#endif  // WLEDMM_C5_DISPLAY
