# Provenance of the files in this component

This component carries files that were not written for WLED-MM-P4.  They are
here because the Arduino API is spelled in them - the WLED sources and the
Arduino libraries (AsyncTCP, ESPAsyncWebServer, NeoPixelBus, LovyanGFX) use
those names and those shapes.  They are used unmodified, except where a file
had to include something other than `<Arduino.h>` now that the Arduino layer
is a separate component.

## The ESP32 Arduino core

Taken from the ESP32 Arduino core (`cores/esp32/` and `variants/`), which is
**LGPL-2.1-or-later**:

| file | from |
| --- | --- |
| `vendor/WString.{h,cpp}` | `cores/esp32/WString.{h,cpp}` |
| `vendor/Print.{h,cpp}` | `cores/esp32/Print.{h,cpp}` |
| `vendor/Stream.{h,cpp}` | `cores/esp32/Stream.{h,cpp}` |
| `vendor/StreamString.{h,cpp}` | `cores/esp32/StreamString.{h,cpp}` |
| `vendor/Printable.h` | `cores/esp32/Printable.h` |
| `vendor/WCharacter.h` | `cores/esp32/WCharacter.h` |
| `vendor/IPAddress.{h,cpp}` | `cores/esp32/IPAddress.{h,cpp}` |
| `vendor/MD5Builder.{h,cpp}` | `cores/esp32/MD5Builder.{h,cpp}` |
| `vendor/HEXBuilder.{h,cpp}` | `cores/esp32/HEXBuilder.{h,cpp}` |
| `vendor/HashBuilder.h` | `cores/esp32/HashBuilder.h` |
| `vendor/fs/{FS,FSImpl,LittleFS,vfs_api}.{h,cpp}` | `cores/esp32/fs/` |
| `vendor/pgmspace.h` | `cores/esp32/pgmspace.h` |
| `vendor/stdlib_noniso.h` | `cores/esp32/stdlib_noniso.h` |

The core's own version of the file is in the framework package this project
builds against, so the two can be compared directly.

The LGPL requires that the derived work can be relinked against a modified
version of these files.  That is what `components/wled_compat` is for: it is
the Arduino-shaped façade, and it can be replaced.

## libb64

`libb64/` is the core's copy of the base64 encoder, which the web server's
basic authentication uses.  It is a **public domain dedication** (see
`libb64/LICENSE`).

## What is ours

Everything else in this component - `include/arduino_types.h`,
`include/arduino_clock.h`, `include/arduino_log.h`, `src/wled_time.cpp`,
`src/arduino_stdlib.cpp`, `src/arduino_log.cpp` - is WLED-MM-P4's own, written
against ESP-IDF.
