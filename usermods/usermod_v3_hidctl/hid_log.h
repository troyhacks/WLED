#pragma once
// The HID's own log: every line says when it happened.
//
// The HID's events are spread over two files - the USB host (the device, the
// pipe, the transfers) and the usermod (the profiles, the controls, the
// pictures) - and they are the events that have to be placed in time: a
// device that is plugged in at boot, a pipe that recovers, a report that
// arrives late, a picture that is a frame behind.  The board's own log lines
// carry no time on them, so these lines carry their own.
//
// The stamp is the milliseconds since boot, from millis().  It is an integer
// on purpose: the P4 has no double FPU, and a float here would cost more
// than the millisecond is worth.
//
// The four macros differ in where the line goes and in how loud it is:
//
//   HIDCTL_LOG   - the host's events.  Always on, to the console: they are
//                  wanted while the board is coming up, before there is a
//                  network to talk to.
//   HIDCTL_INFO  - the usermod's messages.  Always on, to the console, or to
//                  the network debug when that is on (the rule USER_PRINT
//                  follows).
//   HIDCTL_DEBUG - the HID's own debug.  It has a switch of its own, so the
//                  HID can be looked at without turning on every debug
//                  message in the firmware: WLED_DEBUG costs a full rebuild
//                  and says far too much besides.
//   HIDCTL_TRACE - the per-report lines, one for every report the device
//                  sends.  A device that talks at a kilohertz would fill
//                  the console, so these are for a full debug build only.
//
// The line ends with a newline of its own, so the format string does not
// carry one.

#include "wled.h"

// The HID's debug has a switch of its own.  It is on by default, because
// the HID is what is usually being worked on, and it can be turned off with
// -D WLED_DISABLE_HIDCTL_DEBUG when the console is read more often than the
// HID is looked at.
#if !defined(WLED_DISABLE_HIDCTL_DEBUG) && !defined(USERMOD_HIDCTL_DEBUG)
  #define USERMOD_HIDCTL_DEBUG 1
#endif

// The host's events, to the console and nowhere else.
#define HIDCTL_LOG(fmt, ...) do { if (canUseSerial()) { \
    Serial.printf("[%lums][HIDCTL] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); \
  } } while (0)

// The usermod's own messages.
//
// USER_PRINTF is a braced block of its own, so the do-block here is closed
// by ONE brace.  Two would close the function the call sits in as well, and
// everything after the call would be read as a member declaration.
#define HIDCTL_INFO(fmt, ...) do { \
    USER_PRINTF("[%lums][HIDCTL] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); \
  } while (0)

#if defined(USERMOD_HIDCTL_DEBUG) || defined(WLED_DEBUG)
  #define HIDCTL_DEBUG(fmt, ...) do { \
    USER_PRINTF("[%lums][HIDCTL] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); \
  } while (0)
#else
  #define HIDCTL_DEBUG(fmt, ...) do { } while (0)
#endif

#if defined(WLED_DEBUG)
  #define HIDCTL_TRACE(fmt, ...) do { \
    USER_PRINTF("[%lums][HIDCTL] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); \
  } while (0)
#else
  #define HIDCTL_TRACE(fmt, ...) do { } while (0)
#endif
