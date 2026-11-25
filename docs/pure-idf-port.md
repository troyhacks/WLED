# WLED-MM-P4: moving to a pure ESP-IDF build

This is the plan for taking the ESP32-P4 firmware off the Arduino framework and
onto ESP-IDF, with no Arduino core in the firmware at all.

It started as a question - *can this be done, and how?* - and turned into an
implementation.  This document is the reasoning behind the implementation: what
was already IDF, what had to change, and what MoonLight taught us.

## The short version

The WLED-MM-P4 firmware is already mostly ESP-IDF.  The Ethernet, the PPA and
HDMI output, the USB host, the audio capture and the C6 coprocessor link all
use IDF drivers directly.  What was left of Arduino was:

1. **the Arduino core** - the `String` class, `Print`/`Stream`, `millis()`,
   GPIO and LEDC and ADC, `Serial`, `Wire`, `SPI`, the filesystem, mDNS; and
2. **the Arduino web server** - `ESPAsyncWebServer` and `AsyncTCP`; and
3. **the build system** - PlatformIO's `framework = arduino`, with a forked
   Arduino core underneath it.

The port replaces all three.

## What MoonLight is, and is not

MoonLight (`C:\Users\troys\GitHub\MoonLight`) is the closest relative: the
same domain, the same hardware, the same author.  It is **not** a pure
ESP-IDF build - its `platformio.ini` says so in as many words:

```ini
[env]
framework = arduino ;espidf will not work as libs rely on arduino (e.g. PhysicHTTP, ...)
```

So MoonLight cannot be used as a template for the build.  What it *is* good
for:

| From MoonLight | What we took |
| --- | --- |
| `firmware/*.ini` - one ini per board, composed with `extra_configs` | `platformio/idf/esp32p4.ini` - the IDF envs live in their own file, so the Arduino envs stay untouched |
| `lib/PsychicHttp` - IDF's `esp_http_server` behind an Arduino-shaped API | the shape to remember for a future replacement of `ESPAsyncWebServer`: IDF-native core, familiar surface |
| `pal.h` / `pal_espidf.cpp` - a platform abstraction layer, with the goal "platform Arduino can be removed in the future" | the same idea, but done the other way round: instead of a new PAL that WLED adopts piecemeal, an Arduino-API layer that IDF backs, so nothing above it changes |

What we did **not** take: `framework = arduino`, the stray `src/idf_component.yml`
(vestigial - it sits in `src/`, where the component manager never reads it),
and the monolithic single ini.

## The build

PlatformIO stays the driver; the framework becomes ESP-IDF.

```
[env:esp32p4_idf]
framework = espidf
src_dir  = ./main
```

PlatformIO's pioarduino builder turns that into a real ESP-IDF project: it
runs CMake over the tree, then compiles the result with SCons.  Two details
matter:

* `is_proper_idf_project()` looks for `<project>/CMakeLists.txt` **and**
  `<src_dir>/CMakeLists.txt`.  We supply both, so ours are used.
* `src_dir` is added to `EXTRA_COMPONENT_DIRS` when it is not `<project>/main`,
  so `main/` is the IDF main component and `wled00/` is the WLED component.

The `pio-scripts/` toolchain keeps working: `build-html.py` regenerates the
UI headers, `output_bins.py` still merges the factory image (the espidf
builder fills `FLASH_EXTRA_IMAGES` with the bootloader, the partition table
and `ota_data_initial.bin` just as the Arduino builder did), and the flash
loop on COM4 is unchanged.

`lib_compat_mode` has to be `off`.  PlatformIO drops a library whose declared
frameworks do not intersect the environment's, in **both** `soft` and `strict`
mode - and `AsyncTCP`, `ESPAsyncWebServer`, `NeoPixelBus` and `LovyanGFX` all
say `"frameworks": "arduino"`.

The layout:

```
CMakeLists.txt              the IDF project root
wled00/                     the WLED application component - and the app
                            component, so app_main() lives here
components/wled_platform/   the platform layer: the clock, the IP address
                            class, the filesystem, and the portable C++ the
                            Arduino API is made of
components/wled_compat/     the Arduino API layer: the hardware names
sdkconfig.defaults          the IDF configuration
partitions.csv              the partition table
wled00/idf_component.yml    the managed components
platformio/idf/esp32p4.ini  the IDF environments
```

`wled00` is the app component because PlatformIO's `src_dir` is a project-wide
option, and it already points at `wled00` for the Arduino build.  The builder
adds `src_dir` to `EXTRA_COMPONENT_DIRS` and expects the app component to be
there; a separate `main/` directory would collide with it.

### The platform layer and the Arduino API layer

The Arduino API is in two components, because half of it is not about hardware
at all:

* **`components/wled_platform`** - the clock, `IPAddress`, the filesystem, and
  the portable C++ classes the Arduino core is made of (`String`, `Print`,
  `Stream`).  They are portable: no HAL, no peripheral, nothing but ESP-IDF
  underneath.  WLED's own code includes them by name.
* **`components/wled_compat`** - the hardware names: `Arduino.h`, the GPIO,
  LEDC, ADC, UART, I2C, SPI, Ethernet and USB-Serial-JTAG drivers, `Wire`,
  `SPI`, `ETH`, `Update`, and the console's `HardwareSerial`/`HWCDC`.  This is
  what the four third-party Arduino libraries need, and it is what will go
  when they are replaced.

The split follows the dependency, not the naming: `fs::File` derives from
`Stream` and `IPAddress` derives from `Printable`, so the filesystem and the IP
class cannot leave the Arduino layer without the portable classes coming with
them.  What cannot cross the other way is anything that touches a peripheral.

`Arduino.h` re-exports the platform's headers, so the libraries that expect
the core's API find it exactly where they expect it - and WLED's own code can
name the platform's headers directly, which is what it does.

The Arduino | IDF table:

| Arduino | IDF |
| --- | --- |
| `String`, `Print`, `Stream`, `IPAddress`, `FS` | the ESP32 Arduino core's own files, vendored (they are portable C++ with no HAL in them) - **the platform layer's** |
| `AsyncUDP` | the Arduino core's own file, vendored (it is already lwIP) |
| `libb64` | the Arduino core's own copy, vendored - the web server's basic auth needs it |
| `esp32-hal.h` | a shim, and it has to stay C - WLED includes it from inside an `extern "C"` block |
| `PROGMEM`, `PSTR`, `pgm_read_byte`, `F()` | identity - on the P4 the flash is memory mapped, exactly as on the Arduino core |
| `millis`, `micros`, `delay` | `esp_timer` and the FreeRTOS tick - **the platform layer's** |
| `pinMode`, `digitalWrite`, `digitalRead` | `driver/gpio.h` |
| `digitalPinToAnalogChannel` and the rest | the P4's own table: ADC1 is GPIO 16..23, ADC2 is GPIO 49..54 |
| `analogWrite` | `driver/ledc.h` |
| `analogRead` | `driver/adc_oneshot.h` with the calibration curve |
| `Serial`, `Serial0` | `driver/uart.h` |
| `HWCDC` | `driver/usb_serial_jtag.h` |
| `Wire` | `driver/i2c_master.h` |
| `SPI` | `driver/spi_master.h` |
| `LittleFS`, `FS`, `File` | `esp_vfs_littlefs` - **the platform layer's** |
| `WiFiUDP` | lwIP sockets |
| `NetworkClient` | lwIP sockets |
| `MDNS` | the `mdns` component |
| `EEPROM` | NVS, in the "eeprom" namespace, where the Arduino core keeps it |
| `xTaskCreateUniversal` | `xTaskCreatePinnedToCore` |
| the network events | IDF's Ethernet and IP events, translated into the Arduino's `arduino_event_id_t` and handed to WLED's `WiFiEvent()` |

Two of these are worth calling out, because the Arduino core does them and
nothing else would:

* **The network events.**  The core called WLED's global `WiFiEvent()` when
  the link came up, and WLED does the board's Ethernet bookkeeping there -
  the static address, the hostname, the reconnect.  Without the core there
  would be nobody to call it, so the compatibility layer does what the core
  did.
* **`ETH.config()`.**  WLED asks for a static address with it, and passes
  `INADDR_NONE` to mean "use DHCP".  The compatibility layer has to
  recognise that and leave the DHCP client alone; setting the address to
  255.255.255.255 and stopping DHCP loses the lease.


Two things are worth calling out:

* **`String` is vendored, not rewritten.**  On ESP32 the core's `F(s)` is not
  the identity - it yields a `const __FlashStringHelper *`, and `String` and
  `Print` have overloads for it.  A hand-rolled `String` would break 2 567
  `F()` call sites and every `ESPAsyncWebServer` call that takes a
  `const String &`.  The core's `WString` is portable C++ with no HAL in it,
  so it is taken as it is (Apache/LGPL, with its headers kept).
* **`Wire` joins the board's bus.**  There is one I2C bus per pin pair on the
  P4, and the display bridge, the I2C scan and the audio codec all sit on it.
  The bus registry moved into `wled_compat` and WLED's `globalI2cBusEnsure()`
  now forwards to it, so `Wire` and the rest of WLED share one bus instead of
  fighting over the pins.

### The web server

Two options:

* keep `ESPAsyncWebServer` and `AsyncTCP`, compiled against the compatibility
  layer;
* replace them with an IDF-native server - `esp_http_server`, wrapped the way
  MoonLight's `PsychicHttp` wraps it.

The first is the lower-risk path: the UI, `/json`, the WebSocket live view and
the XML API keep compiling unchanged, and the Arduino layer that has to exist
anyway (`String`, `Print`, `FS`) is already there for it.

The second removes `AsyncTCP` - a library that speaks raw BSD sockets - from
the build, and it is the one MoonLight's own history points at.  But
`PsychicHttp` is *not* free of the Arduino API either: it includes
`Arduino.h`, `FS.h`, `Print.h` and `MD5Builder.h`.  So switching servers does
not remove the compatibility layer; it adds an API mismatch on top of it, and
it means rewriting the web server, the XML API, the JSON API and the live
view all at once.

So this port keeps `ESPAsyncWebServer` - the "shim the WLED one" half of the
choice - and treats the `esp_http_server` port as the follow-on, once this one
is stable.  When it happens, the shape to follow is MoonLight's: an IDF-native
core behind an Arduino-flavoured API, so that WLED's call sites do not move.

## sdkconfig

The Arduino build passed a lot of IDF options as `-D CONFIG_*` build flags.
Under `framework = espidf` PlatformIO disables its own `custom_sdkconfig`
handling, so all of it moves into `sdkconfig.defaults`, seeded from the
Arduino core's own P4 `sdkconfig` so the silicon settings stay the same.

The Arduino-only options go away: `CONFIG_ARDUINO_RUNNING_CORE`,
`CONFIG_ARDUINO_EVENT_RUNNING_CORE`, `CONFIG_OTA_VERSION_FORCE_SLAVEFW`.
`CONFIG_ASYNC_TCP_RUNNING_CORE` was never an IDF option - it is a plain
`#ifndef` in `AsyncTCP.h` - so it stays a compile definition.

## The things that can go wrong

* **P4 fixes below the Arduino layer.**  The build uses
  `troyhacks/arduino-esp32#feature/esp32p4` and `troyhacks/esp32-arduino-libs`.
  If the P4's Ethernet, SDIO or hosted fixes live in the *IDF* rather than in
  the Arduino layer, a stock IDF will not have them.  This is the first thing
  to check, and the Ethernet link has to be verified before anything is built
  on top of it.
* **The P4's memory layout.**  The silicon changed in rev 3, and the layout
  the bootloader is linked for has to be the one the chip is.  It is the one
  line `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` in `sdkconfig.defaults`, and it
  wants to be flipped when the rev 3 board arrives.  The partition table sits
  between the bootloader and the app, so the bootloader has to fit in the
  gap - which is what `CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_SIZE`, the
  error-level bootloader log and the skipped deep-sleep validation are for.
  They are the Arduino build's settings, so both builds agree.
* **`-flto`.**  PlatformIO compiles every IDF component with the project's
  flags, and LTO across the whole of IDF is not IDF-clean.  It stays off until
  the port is green.
* **The partition label.**  The filesystem partition is labelled `spiffs`,
  because that is the label the Arduino build mounts by and the label is what
  finds the partition.  Renaming it would orphan the data on every deployed
  board.  The table itself is WLED's own -
  `tools/WLED_ESP32_8MB_1500KB_FS.csv` - and both builds use it, so a board
  can be flashed with either and keep its data.
* **The environment macros.**  `AsyncTCP` only includes `IPAddress.h` under
  `#ifdef ARDUINO`, and `ESPAsyncWebServer` only recognises the ESP32 under
  `#ifdef ESP32`.  The Arduino core is gone, but the macros still decide which
  code path those libraries take, so they are defined explicitly.
* **`esp32-hal.h` has to stay C.**  `wled00/pin_manager.h` includes it from
  inside an `extern "C"` block.  The Arduino core's version is a C header;
  ours has to be too, or the standard library's templates come out with C
  linkage and every `std::list` in the web server goes missing with them.
* **The project directory is on the include line.**  A stray file in the
  repository root can shadow a standard header - an empty file called `list`
  was picked up by `#include <list>` and took the web server's `std::list`
  with it.
* **The partition table comes from the board.**  The ESP-IDF builder reads
  `build.partitions` from the board JSON, not `board_build.partitions` from
  the environment.  The stock P4 board would have used IDF's one-megabyte
  single-app table, so there is a board of our own now.
* **The console.**  The Arduino core and the IDF both want the
  USB-Serial-JTAG peripheral, and two drivers on one peripheral fight over the
  same FIFOs.  WLED's console already handles this by uninstalling the IDF
  driver first; in the pure build the firmware owns the peripheral outright.
* **"No static address" is `INADDR_NONE`.**  WLED asks for a static address
  with `ETH.config()`, and passes `INADDR_NONE` to mean "use DHCP".  The
  compatibility layer has to recognise that and leave the DHCP client alone -
  setting the address to 255.255.255.255 and stopping DHCP loses the lease.
* **The generated linker scripts go stale.**  PlatformIO's rules for the
  memory and sections scripts depend on the *source* script, not on the
  sdkconfig, so a configuration change does not regenerate them.  When the
  P4's layout changes, the generated `memory.ld` has to be deleted (and the
  build cache entry with it) or the old layout is linked in.
* **A component's `CMakeLists.txt` does not trigger a CMake re-run.**  The
  espidf builder re-runs CMake only when the *project's* `CMakeLists.txt`
  files change - a component's is not watched.  So adding a component, or
  changing what one is made of, leaves the codemodel stale and the build goes
  on looking for the sources where they used to be ("Source ... not found,
  needed by target").  `pio-scripts/touch_cmake.py` touches the app
  component's `CMakeLists.txt` when a component's is newer than the CMake
  cache, which is the condition the builder checks for.
* **Adding an environment costs a full rebuild of every environment.**
  Before the build PlatformIO compares a checksum of the project
  configuration with the one in `.pio/build/project.checksum`, and if they
  differ it deletes the whole build tree.  So a new env in `platformio.ini`,
  or one changed `extra_scripts` line, throws away every environment's
  objects.  Do the environment changes in one go, and expect the rebuild.
* **The watchdog watches both cores.**  WLED's `enableWatchdog()`
  reconfigures the task watchdog to watch the idle task on every core.  The
  AudioReactive's analysis task lives on the second core, so the watchdog's
  idle check on that core has to be off - which is how the Arduino build has
  it.  With the check on, the board resets itself every half minute.
* **The USB console's buffer.**  The console's own ring buffer holds the
  USB-Serial-JTAG packets between two reads, and its head has to move.  If it
  does not, the count of the bytes waiting comes out as the whole buffer, the
  console reads stale bytes, and a stray `t` among them runs the
  AudioReactive's self-test over and over.
* **The USB host's hub support.**  The Arduino build has
  `CONFIG_USB_HOST_HUBS_SUPPORTED=y`; the IDF build did not, and the option
  is off by default.  Without it the host does not initialise a hub
  ("External Hubs support disabled, Hub device was not initialized"), so
  everything behind the hub is invisible - the F1 was plugged in, the hub
  enumerated as an unknown device, and the F1 never appeared at all.  The
  FIFO bias is the other way round: the Arduino build uses "Periodic OUT",
  but WLED's own USB host code insists on "Balanced" and says so
  (`wled.cpp`).
* **`uart_set_pin()` takes the transmit pin first.**  The IDF's order is
  `uart_set_pin(uart, tx, rx, rts, cts)`, and the Arduino core's `begin()`
  takes them the other way round (`rx, tx`).  Passing the compatibility
  layer's arguments straight through swapped them - and on the P4 that is not
  a mistake the UART can absorb.  UART0's pins are fixed by the IOMUX, so the
  swap left GPIO 37 driving the UART's transmit line *and* routing that same
  pad back into its receive line: **the chip read back everything it
  printed.**  The console then executed its own log as commands - every line
  that began with a command letter became one - which is how "RTA trim band
  1 ..." became a reboot and "the display is in the fixed window ..." became
  a window command.  The Arduino core routes the pins itself and never calls
  `uart_set_pin()` at all, so it never had the chance to get this wrong.



## The rev 3 environment

The ESP32-P4 came in two silicon revisions: the "ES pre rev.300" chip and the
rev 3 (the board reports `revision v3.2`).  They are **not** the same silicon
under a different label - the SRAM map changed, and ESP-IDF's support for the
two is mutually exclusive.  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` is the switch,
and it is off by default.

So the rev 3 needs its own environment, and it differs in three places:

| what | where | why |
| --- | --- | --- |
| `chip_variant` | `boards/esp32-p4-evboard-wled-idf-rev3.json` | the builder picks the **bootloader's** linker script from the board's chip variant, not from Kconfig: `chip_variant: esp32p4` gets `bootloader.rev3.ld`, `esp32p4_es` gets the old `bootloader.ld` |
| the sdkconfig delta | `sdkconfig.esp32p4_idf_rev3.defaults` | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` off is what puts the **application** on the rev 3 memory map - the IDF's own `memory.ld.in` switches on it, and `pio-scripts/env_sdkconfig.py` puts the file after the shared `sdkconfig.defaults`, so the shared settings still apply |
| the partition table | the board JSON | the same WLED table, so a board can be flashed with either build and keep its data |

The board JSON is a copy of the rev 1 one with `chip_variant` changed.  That one
field is what picks the script the bootloader is linked for; leaving it as
`esp32p4_es` links the rev 3 bootloader with the old script, and the board dies
before the application starts.

One thing the rev 3 does not do by itself, and the application has to do:

* **PSRAM.**  Espressif's own rev 3 defconfig leaves `CONFIG_SPIRAM_BOOT_INIT`
  off, and ESP-IDF only registers the PSRAM with the heap allocator when that
  flag is set.  So the application calls `esp_psram_init()` and
  `esp_psram_extram_add_to_heap_allocator()` itself (see `wled_idf.cpp`).
  Without it the board reports 33.5 MB of PSRAM it cannot allocate from.

### The variants

The firmware has three network variants, and they are two defines:

| variant | `WLED_USE_ETHERNET` | `WLED_USE_ETHERNET_ONLY` |
| --- | --- | --- |
| WiFi | - | - |
| Ethernet only | yes | yes |
| Ethernet and WiFi | yes | - |

The IDF environments share their flags in `esp32p4_idf_flags` in
`platformio/idf/esp32p4.ini`, so a variant is a set of flags over that
section - the same way `esp32p4_idf_hidctl` adds one usermod to it.  The
defines are not in the shared section yet, because the Ethernet-only build is
the one the board wants.

### The bundle

`pio-scripts/make_bundle.py` collects the four images, the factory image, the
flashing script, the notes and the checksums into `build_output/bundle-<env>/`,
and zips it, so the result can be handed to someone else as it is:

```
pio run -e esp32p4_idf_rev3
python pio-scripts/make_bundle.py esp32p4_idf_rev3
```

The one command that flashes the whole thing is

```
esptool --chip esp32p4 --port COM4 write_flash 0x0 firmware.factory.bin
```

The P4's ROM looks for its bootloader at **0x2000**, not at 0x0 - so the
bootloader must not be written at 0x0 when the files are flashed one at a time.

## Verifying

The loop is the established one: build, kill the monitor, flash to COM4, then
read `/json/ar` and the info page.

The monitor must not assert DTR - a DTR falling edge resets the P4, and
Windows drops DTR when the port closes.  A board that stops answering is
wedged and needs a power cycle; DTR will not bring it back.

The parity gate is `/json`: the Arduino build and the IDF build, on the same
board with the same configuration, have to produce the same document.
