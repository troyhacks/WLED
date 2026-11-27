#include "wled.h"
#include "wled_console.h"

#if defined(SOC_USB_SERIAL_JTAG_SUPPORTED)
#include "driver/usb_serial_jtag.h"
#endif

// The one true console.  The Arduino core's "Serial" is a macro, so this
// object takes the name over (see wled_console.h).
WledConsole Serial;

#if defined(WLEDMM_CONSOLE_USB) && !ARDUINO_USB_CDC_ON_BOOT
// The core did not build its USB-Serial-JTAG instance (no
// ARDUINO_USB_CDC_ON_BOOT), so the console brings its own.
HWCDC wledUsbCdc;
#endif

bool WledConsole::usbConnected() {
#if WLEDMM_CONSOLE_USB
  return usb_serial_jtag_is_connected();
#else
  return false;
#endif
}

bool WledConsole::uartUsable() {
  // If the board gave UART0's TX pin to something else (a LED strip, say),
  // writing to it would corrupt that output - so we do not write there.
  return !pinManager.isPinAllocated((uint8_t)SOC_TX0) ||
         pinManager.getPinOwner((uint8_t)SOC_TX0) == PinOwner::DebugOut;
}

void WledConsole::begin(uint32_t baud) {
  if (_open) {
    // Already up: only the UART has a baud rate, and the USB CDC is a packet
    // pipe that does not care.  (The baud can be changed at runtime from the
    // serial console.)
    Serial0.begin(baud, SERIAL_8N1, SOC_RX0, SOC_TX0, false, 20000UL, 120U);
    return;
  }
#if WLEDMM_CONSOLE_USB
  // The IDF owns the USB-Serial-JTAG as its secondary console, and it keeps
  // it: two drivers on one peripheral fight over the same FIFOs - the fork
  // has already seen what that looks like: "invalid header: 0x..." spam and
  // a watchdog reset.
  //
  // The console writes through that driver rather than taking the
  // peripheral over.  Uninstalling it left the IDF's console holding a
  // handle to a driver that was gone, and its log writes landed on freed
  // memory - which is how the web server's task got its stack overwritten
  // with a USB-Serial-JTAG register address.
  //
  // A bigger TX buffer means the boot log survives a monitor that opens
  // late - but the buffer belongs to the driver, so it only takes effect
  // when this class installs it itself.
  WLEDMM_USB_CDC->setTxBufferSize(4096);
  WLEDMM_USB_CDC->setRxBufferSize(1024);
  WLEDMM_USB_CDC->begin();
  WLEDMM_USB_CDC->setTxTimeoutMs(0);  // never wait for a host that is not there
#endif
  // UART0: GPIO37 (TX) / GPIO38 (RX) on the P4, GPIO58 / GPIO59 on the S31
  // (the pins are SOC_TX0 / SOC_RX0 - the compat layer's table has them).
  // A ring buffer for TX, so a board with nothing on the other end of the
  // wire drops output instead of stalling the app on it.
  Serial0.setRxBufferSize(1024);
  Serial0.setTxBufferSize(1024);
  Serial0.begin(baud, SERIAL_8N1, SOC_RX0, SOC_TX0, false, 20000UL, 120U);
  // NO pinMode() on the RX pin here!  The Arduino core's pinMode() calls
  // perimanClearPinBus(), which detaches the peripheral that owns the pin -
  // and that is UART0's receive line.  The pin belongs to the UART from here
  // on, so touching it would silently stop the console from receiving.
  // The pull is therefore set directly: a floating RX line has to read as
  // silence, not as random commands.
  gpio_set_pull_mode((gpio_num_t)SOC_RX0, GPIO_PULLDOWN_ONLY);
  _open = true;
}

void WledConsole::end() {
  // The USB CDC is the Arduino core's business - leaving it alone keeps the
  // port usable for a reflash.
  Serial0.end();
  _open = false;
}

size_t WledConsole::write(uint8_t c) {
  return write(&c, 1);
}

size_t WledConsole::write(const uint8_t *buffer, size_t size) {
  if (!buffer || !size) return 0;
#if WLEDMM_CONSOLE_USB
  // The USB CDC drops what does not fit; with the TX timeout at 0 it never
  // waits for a host that is not there.
  WLEDMM_USB_CDC->write(buffer, size);
#endif
  if (uartUsable()) {
    // The UART gets what fits in its TX buffer, the rest is dropped: nobody
    // is required to be listening.
    int room = Serial0.availableForWrite();
    if (room > 0) {
      size_t n = ((size_t)room < size) ? (size_t)room : size;
      Serial0.write(buffer, n);
    }
  }
  return size;
}

int WledConsole::available() {
  int n = 0;
#if WLEDMM_CONSOLE_USB
  n = WLEDMM_USB_CDC->available();
  if (n < 0) n = 0;
#endif
  return n + Serial0.available();
}

int WledConsole::read() {
#if WLEDMM_CONSOLE_USB
  // A host on the USB is the one typing at us - the UART may just be noise.
  int c = WLEDMM_USB_CDC->read();
  if (c >= 0) return c;
#endif
  return Serial0.read();
}

int WledConsole::peek() {
#if WLEDMM_CONSOLE_USB
  int c = WLEDMM_USB_CDC->peek();
  if (c >= 0) return c;
#endif
  return Serial0.peek();
}

size_t WledConsole::read(uint8_t *buffer, size_t size) {
  if (!buffer) return 0;
  size_t n = 0;
  while (n < size) {
    int c = read();
    if (c < 0) break;
    buffer[n++] = (uint8_t)c;
  }
  return n;
}

void WledConsole::flush() {
#if WLEDMM_CONSOLE_USB
  WLEDMM_USB_CDC->flush();
#endif
  // Serial0.flush() waits for the UART to go idle, which it never does if the
  // TX buffer still holds data and nobody is reading - so we wait a little
  // and then let go.
  uint32_t start = millis();
  while (Serial0.availableForWrite() < 1024 && (millis() - start) < 100) {
    delay(1);
  }
}

void WledConsole::setTxTimeoutMs(uint32_t timeout) {
#if WLEDMM_CONSOLE_USB
  WLEDMM_USB_CDC->setTxTimeoutMs(timeout);
#endif
}

void WledConsole::setDebugOutput(bool en) {
  if (!en) return;  // leave the kernel output where it is
#if WLEDMM_CONSOLE_USB
  // The kernel/ROM output follows the console: to the USB if a host is there,
  // and to UART0 otherwise.
  if (usb_serial_jtag_is_connected()) {
    WLEDMM_USB_CDC->setDebugOutput(true);
    Serial0.setDebugOutput(false);
  } else {
    Serial0.setDebugOutput(true);
  }
#else
  Serial0.setDebugOutput(en);
#endif
}
