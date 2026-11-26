#pragma once
// WLEDMM: one console for every board.
//
// Which console a board has is a property of the *board*, not of the build:
//   - the built-in USB (USB-Serial-JTAG) - the "Serial+JTAG" boards
//   - UART0 on GPIO37/38 - the boards with a plain serial header
// The Arduino core picks one of them at compile time (ARDUINO_USB_CDC_ON_BOOT),
// so one build can only ever serve one kind of board: with the USB flags on,
// a board without USB has no console at all (and every write to it can block),
// and with them off, a "Serial+JTAG" board is silent.
//
// This class holds both and writes to whichever is there.  One firmware then
// serves both boards - and a board with neither, where nothing waits for a
// reader that is not there and nothing is lost but the bytes.
//
// The Arduino core's "Serial" is a macro, so this header takes the name over:
// every file that includes wled.h (or this header) gets the console below.

#include <Arduino.h>
#include <HardwareSerial.h>

#if defined(SOC_USB_SERIAL_JTAG_SUPPORTED) && ARDUINO_USB_MODE && !WLEDMM_NO_USB_CONSOLE
#include <HWCDC.h>
#define WLEDMM_CONSOLE_USB 1
// The core's USB-Serial-JTAG instance exists only when it was built with
// ARDUINO_USB_CDC_ON_BOOT.  When it does not, the console brings its own -
// the class is compiled whenever the SoC has the peripheral.  Only one of
// them may exist: the class keeps its buffers in file statics.
#if ARDUINO_USB_CDC_ON_BOOT
extern HWCDC HWCDCSerial;   // the core's instance
#define WLEDMM_USB_CDC (&HWCDCSerial)
#else
extern HWCDC wledUsbCdc;    // ours
#define WLEDMM_USB_CDC (&wledUsbCdc)
#endif
#endif

class WledConsole : public Stream {
public:
  void begin(uint32_t baud = 115200);
  void end();

  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buffer, size_t size) override;
  int available() override;
  int read() override;
  int peek() override;
  size_t read(uint8_t *buffer, size_t size);  // bulk read, from whichever device has data
  void flush() override;

  void setTxTimeoutMs(uint32_t timeout);  // USB CDC only, the UART never waits
  void setDebugOutput(bool en);           // kernel/ROM output follows the console

  operator bool() const { return _open; }

  static bool usbConnected();  // a host is on the USB right now
  static bool uartUsable();    // UART0's TX pin is ours to write on

private:
  bool _open = false;
};

#undef Serial                 // the Arduino core's macro gives way to this one
extern WledConsole Serial;
