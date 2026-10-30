#include "wled.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE)
#include "wled_hdmi.h"
#endif
#ifdef ARDUINO_ARCH_ESP32
#include "esp_ota_ops.h"
#endif

#define SERIAL_MAXTIME_MILLIS 500 // to avoid blocking other activities, do not spend more than 100ms with continuous reading
// at 115200 baud, 100ms is enough to send/receive 1280 chars

uint16_t currentBaud = 1152; //default baudrate 115200 (divided by 100)
uint32_t lastUpdate = 0;

void updateBaudRate(uint32_t rate){
  uint16_t rate100 = rate/100;
  if (rate100 == currentBaud || rate100 < 96) return;
  currentBaud = rate100;

  if (!pinManager.isPinAllocated(hardwareTX) || pinManager.getPinOwner(hardwareTX) == PinOwner::DebugOut){
    if (Serial) { Serial.print(F("Baud is now ")); Serial.println(rate); }
  }

  if (Serial) Serial.flush();
  #if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT != 0
    Serial.begin(rate);
  #else
    Serial.begin(rate, SERIAL_8N1, SOC_RX0, SOC_TX0, false, 20000UL, 120U);
  #endif
}

// RGB LED data return as JSON array. Slow, but easy to use on the other end.
void sendJSON(){
  if (!pinManager.isPinAllocated(hardwareTX) || pinManager.getPinOwner(hardwareTX) == PinOwner::DebugOut) {
    if (!Serial) return; // WLEDMM avoid writing to unconnected USB-CDC
    uint16_t used = strip.getLengthTotal();
    Serial.write('[');
    for (uint16_t i=0; i<used; i++) {
      Serial.print(strip.getPixelColor(i));
      if (i != used-1) Serial.write(',');
    }
    Serial.println("]");
  }
}

bool canUseSerial(void) {   // WLEDMM returns true if Serial can be used for debug output (i.e. not configured for other purpose)
  #if ARDUINO_USB_CDC_ON_BOOT && !defined(WLED_DEBUG_HOST)
  //  USB CDC blocks if disconnected! so check if Serial is active before printing to it.
  if (!Serial) return false;
  #endif
  if (pinManager.isPinAllocated(hardwareTX) && (pinManager.getPinOwner(hardwareTX) != PinOwner::DebugOut)) 
    return false;  // TX allocated to LEDs or other functions
  if ((realtimeMode == REALTIME_MODE_GENERIC)) 
    return false;  // Serial in use for other serial communication
  return true;
} // WLEDMM end

static um_data_t* getAudioData() {
  um_data_t *um_data;
  if (!usermods.getUMData(&um_data, USERMOD_ID_AUDIOREACTIVE)) {
    // add support for no audio
    um_data = simulateSound(SEGMENT.soundSim);
  }
  return um_data;
}

static float mapf(float x, float in_min, float in_max, float out_min, float out_max){
  if (in_max == in_min) return (out_min);  // WLEDMM avoid div/0
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

#include <algorithm> 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void task_list() {

  #define MAX_TASKS 30 // if you see "zero tasks" raise this number. If there's more tasks than this, you get NO tasks back.

  TaskStatus_t taskStatusArray[MAX_TASKS];
  UBaseType_t taskCount;
  uint32_t totalRunTime;

  taskCount = uxTaskGetSystemState(taskStatusArray, MAX_TASKS, &totalRunTime);

  // Sort tasks first by Core ID, then by descending Run Time (CPU usage)
  std::sort(taskStatusArray, taskStatusArray + taskCount, [](const TaskStatus_t& a, const TaskStatus_t& b) {
    // Primary sort: Core ID (Core 0, then Core 1, then unassigned)
    if (a.xCoreID != b.xCoreID) {
      return a.xCoreID < b.xCoreID;
    }
    // Secondary sort: Run Time (higher usage first)
    return a.ulRunTimeCounter > b.ulRunTimeCounter;
  });

  printf("Found %d tasks\n", taskCount);
  printf("Name\t\tState\tPrio\tStack\tRun Time\tCPU %%\tCore\n");

  for (UBaseType_t i = 0; i < taskCount; i++) {
    TaskStatus_t* ts = &taskStatusArray[i];

    const char* state;
    switch (ts->eCurrentState) {
      case eRunning:   state = "Running"; break;
      case eReady:     state = "Ready"; break;
      case eBlocked:   state = "Blocked"; break;
      case eSuspended: state = "Suspended"; break;
      case eDeleted:   state = "Deleted"; break;
      default:         state = "Unknown"; break;
    }

    char cpu_percent[32];
    snprintf(cpu_percent, sizeof(cpu_percent), "%5.2f%%", totalRunTime > 0 ? (100.0f * ts->ulRunTimeCounter) / totalRunTime : 0.0f);

    printf("%-12s %-10s %4u\t%5u\t%10lu\t%s\t%2d\n",
      ts->pcTaskName,
      state,
      ts->uxCurrentPriority,
      ts->usStackHighWaterMark,
      ts->ulRunTimeCounter,
      cpu_percent,
      ts->xCoreID == tskNO_AFFINITY ? -1 : ts->xCoreID);
  }
}

void serial_drain() {
  if (Serial.available()) {
    byte* garbage = (byte*)heap_caps_malloc_prefer(Serial.available() + 100, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
    USER_PRINTF("Throwing away %u bag%s of serial garbage.\n", Serial.available(), Serial.available() > 1 ? "s" : "");
    Serial.read(garbage, Serial.available());
    heap_caps_free(garbage);
  }
}

void handleSerial() {

  if (!Serial) return;  // arduino docs: `if (Serial)` indicates whether or not the USB CDC serial connection is open. For all non-USB CDC ports, this will always return true

  PinOwner srxo = pinManager.getPinOwner(hardwareRX);
  if (srxo != PinOwner::DebugOut && srxo != PinOwner::None) return;

  // if (Serial.available() > 1) { serial_drain(); return; } // I dunno, something was really giving me serial garbage.

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(WLEDMM_DISPLAY_MODE)
  // HDMI mode switch: type 'm' to open interactive menu, then enter a number + Enter.
  if (Serial.available() >= 1 && Serial.peek() == 'm') {
    Serial.read();  // consume 'm'
    // drain any trailing newline that arrived alongside the 'm'
    while (Serial.available() && (Serial.peek() == '\r' || Serial.peek() == '\n')) Serial.read();
    // print the mode menu
    hdmi_print_mode_menu();
    // wait up to 10 s for the user to type a number + Enter (3 s timeout after first digit)
    char buf[4] = {}; uint8_t len = 0;
    uint32_t deadline = millis() + 10000;
    while (millis() < deadline) {
      if (!Serial.available()) { delay(5); continue; }
      char c = Serial.read();
      if (c == '\n' || c == '\r') { if (len > 0) break; }
      else if (c >= '0' && c <= '9' && len < 3) { buf[len++] = c; deadline = millis() + 3000; }
    }
    if (len > 0) hdmi_switch_mode(atoi(buf));
    else Serial.println("HDMI: no mode selected.");
    return;
  }
#endif // CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE

  unsigned long startTime = millis();
  while ((Serial.available() > 0) && (millis() - startTime < SERIAL_MAXTIME_MILLIS)) {
    byte next = Serial.peek();
    if (next == 'v') {
      Serial.print("WLED"); Serial.write(' '); Serial.println(VERSION);
    } else if (next == 'F') {
      ArtNetSkipFrame = !ArtNetSkipFrame;
      RealtimeSkipFrame = !RealtimeSkipFrame;
      USER_PRINTF("Art-Net Skip-Frame is now %s.\n", ArtNetSkipFrame ? "ON" : "OFF");
    } else if (next == 'f') {
      USER_PRINTF("FPS: %d\n", strip.getFps());
    } else if (next == '^') {
      esp_err_t err;
      const esp_partition_t* boot_partition = esp_ota_get_boot_partition();
      const esp_partition_t* running_partition = esp_ota_get_running_partition();
      USER_PRINTF("Running on %s and we should have booted from %s. This %s\n", running_partition->label, boot_partition->label, (String(running_partition->label) == String(boot_partition->label)) ? "is what we expect." : "means OTA messed up!");
      if (String(running_partition->label) == String(boot_partition->label)) {
        esp_partition_iterator_t new_boot_partition_iterator = NULL;
        if (boot_partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
          new_boot_partition_iterator = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "app1");
        } else {
          new_boot_partition_iterator = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "app0");
        }
        const esp_partition_t* new_boot_partition = esp_partition_get(new_boot_partition_iterator);
        err = esp_ota_set_boot_partition(new_boot_partition);
        if (err == ESP_OK) {
          USER_PRINTF("Switching boot partitions from %s to %s in 3 seconds!\n", boot_partition->label, new_boot_partition->label);
          delay(3000);
          esp_restart();
        } else {
          USER_PRINTF("Looks like the other app partition (%s) is invalid. Ignoring.\n", new_boot_partition->label);
        }
      } else {
        USER_PRINTF("Looks like the other partion is invalid as we exepected %s but we booted failsafe to %s. Ignoring boot change.\n", boot_partition->label, running_partition->label);
      }
    } else if ((next >= '0' && next <= '9') || next == '+' || next == '-' || next == '*' || next == '/') {
      #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
        handleSerialInput(next);
      #endif
    } else if (next == 'X') { // WLEDMM - force reconnect via Serial
      USER_PRINTLN("handleSerial forcing reconnect");
      forceReconnect = true;
    } else if (next == 'C') { // WLEDMM - task list display for developers
      task_list();
    } else if (next == 'T') { // WLEDMM - task stack high water mark display for developers
      dumpAllTaskHWMs();
    } else if (next == 'R') { // WLEDMM - force reboot via Serial
      Serial.print("Rebooting ");
      for (int i = 0;i < 5;i++) {
        Serial.print(".");
        delay(200);
      }
      Serial.println(" now!");
      Serial.flush();
      Serial.end();
      ESP.restart();
    } else if (next == 'G') { // WLEDMM - "G"EQ via Serial
      um_data_t* um_data = getAudioData();
      uint8_t fftResult[16] = { 0 };
      if (um_data && um_data->u_data) {
        memcpy(fftResult, um_data->u_data[2], sizeof(fftResult));
      }
      const uint8_t scale = 16;
      uint8_t heights[16];
      for (int i = 0; i < 16; i++) {
        heights[i] = map8(fftResult[i], 0, scale);
      }
      USER_PRINTLN();
      for (int row = scale; row > 0; row--) {
        for (int col = 0; col < 16; col++) {
          if (heights[col] >= row) {
            USER_PRINT("████");
          } else {
            USER_PRINT("    ");
          }
        }
        USER_PRINTLN();
      }
      for (int i = 0; i < 16; i++) {
        USER_PRINTF("%4d", fftResult[i]);
      }
      USER_PRINTLN();
    } else if (next == 'd' || next == 'D') {
      // Walk the dB display trim in 1 dB steps. 'd' lowers it (bars shrink),
      // 'D' raises it (bars grow).
      //
      // The trim is applied to the band levels AFTER the auto-range has
      // normalised them, so it is a pure display adjustment: it does not
      // change what was measured, only where the bars sit. It used to be a
      // linear gain coefficient, which could only be turned one way before it
      // clipped and read as a negative number.
      TROYHACKS_DBREF += (next == 'D') ? 1.0f : -1.0f;
      if (TROYHACKS_DBREF < -24.0f) TROYHACKS_DBREF = -24.0f;
      if (TROYHACKS_DBREF >  24.0f) TROYHACKS_DBREF =  24.0f;
      USER_PRINTF("dB display trim now %+.1f dB\n", TROYHACKS_DBREF);
#ifdef USERMOD_AUDIOREACTIVE
    // ---------------------------------------------------------------------
    // The AudioReactive keys. Single characters, every one of them live -
    // they change a number while the display is being watched.
    //
    //   t        run the input chain's self-test (synthetic signals only)
    //   g        plot the 31-band RTA view on the console
    //   A        the display back to auto-range, and the trim back to its
    //            boot value
    //   b / B    the window's floor, down / up by 1 dB      (fixed window)
    //   u / U    the window's top, down / up by 1 dB        (fixed window)
    //   s / S    the auto window's span, narrower / wider by 1 dB
    //   o / O    the reference offset, down / up by 1 dB     (calibration)
    //   n / p    the band the trim applies to, next / previous
    //   y / Y    that band's trim, down / up by 1 dB         (calibration)
    //   k / K    the filter bank's detector, shorter / longer by 5 ms
    //   w / W    the auto-range's release, shorter / longer by 500 ms
    //
    // Lowercase is the lower number and uppercase the higher one, for every
    // pair. 'X' is NOT in this list: it is the force-reconnect key above.
    //
    // Two more are AudioReactive keys, but they are handled ABOVE this
    // block, with the other WLED-MM keys: they need the published bands and
    // not the usermod's internals.
    //
    //   G        plot the 16 display bands on the console
    //   d / D    the dB display trim, down / up by 1 dB
    // ---------------------------------------------------------------------
    } else if (next == 't') {
      // Run the input chain's self-test: it measures the chain as implemented
      // (impulse response and sine response) and compares both with the
      // analytic model. Uses synthetic signals only - the live path is not
      // disturbed.
      audioReactiveSelfTest();
    } else if (next == 'g') {
      // Plot the 31-band RTA view (1/3-octave ISO 266, integrated power).
      // This is the scale a hardware analyser uses, so the two can be
      // compared bar for bar. Pre-AGC, deliberately.
      audioReactiveRtaPlot();
    } else if (next == 'A') {
      // Back to auto-range mode: the auto-range in sole control.
      //
      // On top of the auto-range, the 'd'/'D' trim is a manual adjustment that
      // the auto-range knows nothing about: a trim of +10 dB pushes the
      // loudest band past the top of the window, and resetting the auto-range
      // alone cannot undo that.
      //
      // So this clears the auto-ranges AND puts the trim back to the value the
      // board booted with, which is the state the display was in before
      // anything was touched. It also puts the display back into auto-range
      // MODE, because the fixed window is a MODE and not an offset.
      //
      // The calibration - the reference offset and the per-band trim - is NOT
      // reset: those correct the measurement, and the measurement is not what
      // this command is about.
      TROYHACKS_DBREF = TROYHACKS_DBREF_BOOT;
      audioReactiveResetAutoRange();
      USER_PRINTF("Display back to auto-range: trim %+.1f dB\n",
                  (double)TROYHACKS_DBREF);
    } else if (next == 'b' || next == 'B' || next == 'u' || next == 'U' ||
               next == 's' || next == 'S' ||
               next == 'o' || next == 'O' || next == 'n' || next == 'p' ||
               next == 'y' || next == 'Y' || next == 'k' || next == 'K' ||
               next == 'w' || next == 'W') {
      // The window and the calibration: the floor and the top in absolute dB,
      // the auto window's span, the reference offset that ties our dB to the
      // analyser's, the per-band trim that makes the shapes match, the filter
      // bank's detector time constant - how fast the bars follow - and the
      // auto-range's release.
      audioReactiveRtaCommand(next);
#endif
    } else if (next == 0xB0) {
      updateBaudRate(115200);
    } else if (next == 0xB1) {
      updateBaudRate(230400);
    } else if (next == 0xB2) {
      updateBaudRate(460800);
    } else if (next == 0xB3) {
      updateBaudRate(500000);
    } else if (next == 0xB4) {
      updateBaudRate(576000);
    } else if (next == 0xB5) {
      updateBaudRate(921600);
    } else if (next == 0xB6) {
      updateBaudRate(1000000);
    } else if (next == 0xB7) {
      updateBaudRate(1500000);
    } else if (next == 'l') {
      sendJSON(); // Send LED data as JSON Array
    } else if (next == '{') { //JSON API
      bool verboseResponse = false;
      if (!requestJSONBufferLock(16)) {
        if (Serial) Serial.println(F("{\"error\":3}")); // ERR_NOBUF
        return;
      }
      Serial.setTimeout(100);
      DeserializationError error = deserializeJson(doc, Serial);
      if (error) {
        releaseJSONBufferLock();
        return;
      }
      verboseResponse = deserializeState(doc.as<JsonObject>());
      //only send response if TX pin is unused for other purposes
      if (verboseResponse && (!pinManager.isPinAllocated(hardwareTX) || pinManager.getPinOwner(hardwareTX) == PinOwner::DebugOut)) {
        doc.clear();
        JsonObject state = doc.createNestedObject("state");
        serializeState(state);
        JsonObject info = doc.createNestedObject("info");
        serializeInfo(info);
        serializeJson(doc, Serial);
        Serial.println();
      }
      releaseJSONBufferLock();
    }
    Serial.read(); //discard the byte
  }

  #ifdef WLED_DEBUG
  if ((millis() - startTime) > SERIAL_MAXTIME_MILLIS) { USER_PRINTLN(F("handleSerial(): need a break after >100ms of activity.")); }
  #endif

}
