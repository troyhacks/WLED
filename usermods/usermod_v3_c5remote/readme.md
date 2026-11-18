# The ESP32-C5 remote

A WaveShare **ESP32-C5-LCD-2.73** board, running WLED, as a remote and a
display for the board that has the beat tracker.

```
  ESP32-C5-LCD-2.73                     ESP32-P4
  ┌──────────────────┐                  ┌──────────────────┐
  │  2.73" 320x320   │   audio sync    │  the tracker     │
  │  ILI9488 over    │◀────────────────│  the tempo       │
  │  SPI             │  239.0.0.1:11988│  the grid        │
  │                  │                  │                  │
  │  CH32V003 IO exp │   HTTP /json/ar │                  │
  │  QMI8658 IMU     │◀────────────────│                  │
  └──────────────────┘   POST /json    └──────────────────┘
```

The board has no audio hardware of its own: it is a **receiver**. The
AudioReactive usermod is built in "network receive only" mode (`SR_DMTYPE=254`),
so it listens for the audio sync packets and shows them.

## What is on the screen

| Page     | What it shows                                                             |
|----------|---------------------------------------------------------------------------|
| `SYNC`   | the boards that are sending audio sync packets, how many packets each has sent, how fast, and how long ago - and the sixteen bands of the last packet as a meter |
| `BEAT`   | the tempo of the board that has the tracker: the rate, the beat, the quality, the drift, and whether the clock is locked |
| `BOARD`  | this board: the network, the uptime, the memory, the IMU, the expander's battery reading and the TF card |
| `BUTTONS`| **the button debug**: every button the board has, what it is doing now, and a log of what has been pressed |
| `PACKETS`| **the cadence of the audio sync packets**: how far apart they are, and whether they are spread out or arriving in bunches |
| `TEST`   | the colour bars: whether the panel is being driven at all |

The `SYNC` page is the one that is watched, so it is kept cheap: the
packet cadence is a picture that changes every frame, and that is
drawing cycles the meter needs.  The `SYNC` page says only whether the
packets are bunched - which is nothing, most of the time - and the
`PACKETS` page has the picture.

## The memory on this board

The ESP32-C5 has 327 KB of RAM and everything counts, so two stacks are
set in the build:

* the **web task** (`CONFIG_ASYNC_TCP_STACK_SIZE`, 12 KB) - and note that
  this is the macro the stack actually comes from.
  `CONFIG_ASYNC_TCP_TASK_STACK_SIZE`, which every environment in this repo
  sets, is used by nothing: AsyncTCP takes its stack from
  `CONFIG_ASYNC_TCP_STACK_SIZE`, which defaults to 16 KB, and the
  AudioReactive usermod's sanity check looks at the other one.  So on this
  board the web task asked for 16 KB of *contiguous* internal RAM that the
  heap did not have, and the web server never started ("begin(): failed to
  start task").  Both are set now, and the check in the AudioReactive
  usermod is satisfied by the same number.
* the **analysis task** (`AR_TASK_STACK`, 8 KB) - it measures about 3.5 KB
  in use, and the free part of it is published as
  "analysis_task_stack_free" so the number can be looked at.

The board has no LEDs, so it has no output bus either
(`DEFAULT_LED_TYPE=TYPE_NONE`): an Art-Net sender on a board with nothing
to send is just noise on the network and in the console.

And the **JSON document** is 20 KB (`JSON_BUFFER_SIZE`) rather than the 54 KB
the boards with more RAM use.  It is a static array in DRAM, and every byte
of it comes out of the heap that the tasks have to be carved out of.  At
40 KB the heap was down to **7 KB** and the web server could not allocate
anything at all - the board answered every request with an empty reply
(`curl: (52) Empty reply from server`).  At 20 KB the heap has 56 KB and
the server is there.

Moving the document to PSRAM would be the obvious answer, and it does not
work: WLED's own `PSRAM_Allocator` is disabled in `wled00/wled.h` with a
"TroyHacks: P4 FIXME: JSON in PSRAM is borked for some reason on v5.5",
and turning it back on made the heap **worse** (6 KB) and left the server
dead.

The heap's *largest block* is the number that matters here, not the heap's
free total: a task's stack has to be one piece.  The console prints it
once a second, beside the IMU's numbers.

Three on the chip, and up to eleven on the IO expander:

| Button                | Pin                | What it does                                    |
|-----------------------|--------------------|-------------------------------------------------|
| KEY_L (previous)      | GPIO2              | the previous page; held: the previous preset on the other board |
| KEY_R (next)          | GPIO3              | the next page; held: the next preset on the other board |
| BOOT                  | GPIO28             | **mark the downbeat** on the other board; held: lock or unlock its clock |
| IO4 .. IO14          | the IO expander    | ordinary WLED buttons - but the usermod's, not the core's (see below) |

Those are the board's own names, from its schematic: **KEY_L** is GPIO2,
**KEY_R** is GPIO3 and **BOOT** is GPIO28, each pulled up with 10K and
pulling its pin down when it is pressed.

The board has a fourth button, **PWR**, and it is not on a GPIO of this
chip at all: it goes to the chip that latches the board's power on, so
there is nothing to read and nothing to show for it.

The three on the chip are the core's: it reads the pins and it does the
debounce, the long press and the macros. The core asks the usermods first,
though, so a usermod can take a button over:

```cpp
// wled00/fcn_declare.h
virtual bool handleButton(uint8_t b) { return false; }
```

### The button debug

The `BUTTONS` page watches the buttons instead of acting on them: the
core's buttons are **captured** while the page is up, so that pressing
"next" does not leave the page before the press can be seen.  `BOOT` leaves
the page and puts it back where it was.

The page shows the board's own three buttons and the expander's eleven, a
box each: green while the button is down, yellow once it has been held
long enough for its long press.  Below them is a log of the last eight
events - down, up, or held - with how long the button was down for and
when it happened.

The same log is in the JSON, for reading from a desk:

```
/json/ar  ->  C5Remote.buttons
  { "down": 0, "presses": 3, "key_l": false, "key_r": false, "boot": false,
    "log": ["KEY_R up 214 ms", "KEY_R down -", "BOOT up 96 ms"] }
```

Two things about the expander's eleven are worth knowing: they are the
header's IO pins, so they are not buttons until something is wired to
them; and the expander's inputs are read every 50 ms and have to be
**stable three times** before they are believed, so a press on one of them
is seen about 150 ms after it happens.

The expander's buttons are **not** the core's, and cannot be: the core
skips a button whose pin is negative before it asks the usermods about
one, so the expander's buttons are read by the usermod itself, and their
debounce, their long press and their macros are done there.

## The tempo actions

The tempo's actions are **button macros that are not preset numbers**. A
macro at or above `WLED_BTN_ACTION_BASE` (300) is an action, and the
usermods are asked which of them they own:

```cpp
// wled00/const.h
#define WLED_ACT_TEMPO_DOWNBEAT        (WLED_BTN_ACTION_BASE +  0)
#define WLED_ACT_TEMPO_CLEAR_DOWNBEAT  (WLED_BTN_ACTION_BASE +  1)
#define WLED_ACT_TEMPO_LOCK            (WLED_BTN_ACTION_BASE +  2)
#define WLED_ACT_TEMPO_UNLOCK          (WLED_BTN_ACTION_BASE +  3)
#define WLED_ACT_TEMPO_TOGGLE_LOCK     (WLED_BTN_ACTION_BASE +  4)
#define WLED_ACT_TEMPO_RELOCK          (WLED_BTN_ACTION_BASE +  5)
#define WLED_ACT_TEMPO_SYNC            (WLED_BTN_ACTION_BASE +  6)
#define WLED_ACT_TEMPO_MODE_DJ         (WLED_BTN_ACTION_BASE +  7)
#define WLED_ACT_TEMPO_MODE_PLAYLIST   (WLED_BTN_ACTION_BASE +  8)
```

The AudioReactive usermod acts on them **when it owns a tempo tracker** -
that is, when it has a local audio source. A board that only listens to the
network has no grid of its own to lock, so it sends the action to the board
that does: that is what the C5 remote does.

**The macros are sixteen bits wide.** They used to be bytes, which is
enough for a preset number and not enough for an action: 300 does not fit
in a byte, so the value was truncated on the way in (304 became 48) and the
action was read as a preset.  The action never happened, and the button
looked like it did something else.  `runButtonMacro()` has taken a
`uint16_t` all along; the arrays that feed it are now the same width.

The same actions are available over the network, without a button:

```
POST http://<the board with the tracker>/json
{"AudioReactive": {"setDownbeat": true}}
{"AudioReactive": {"clearDownbeat": true}}
{"AudioReactive": {"lockTempo": true}}
{"AudioReactive": {"unlockTempo": true}}
{"AudioReactive": {"relockTempo": true}}
{"AudioReactive": {"syncTempo": true}}
{"AudioReactive": {"playlistMode": true}}
{"AudioReactive": {"djSetMode": true}}
```

`relockTempo` is the one that is new here; the others were already there.

## Who is sending

The audio sync packet carries **no identity at all** - 44 bytes, every one
of them audio. No name, no MAC, no number. The only thing that says who
sent a packet is the address it came from.

So the AudioReactive usermod keeps the senders apart:

```
/json/ar  ->  AudioReactive.udp_sync.senders[]
  [ { "ip": "192.168.6.249", "packets": 181554, "rate": 50,
      "last_ms_ago": 4, "format": 2, "level": 128, "bands": [ ... ] } ]
```

and the remote turns the address into a name with WLED's own node list (the
notifier's node-info packet carries the name and the address), falling back
to the address itself.

## The cadence of the packets

The audio sync goes out at the **analysis rate**, so the packets should be
evenly spaced - one every ten milliseconds or so.  Whether they are is
worth knowing: a sender that bunches them up, or leaves gaps between
them, is a sender whose timing is off, and that shows up as a jittery
display long before it shows up as anything else.

So the arrival times are kept, **where the packet arrives** - the packet
callback, in the network's task.  That is the only place the arrival is
known to the millisecond: a reader that sampled the packet counter would
only see what happened between its samples, and at ten milliseconds a
packet that is sampled every thirty is two packets and a gap.

The times are kept per sender (the last 24 of them) and the gaps between
them are published:

```
/json/ar  ->  AudioReactive.udp_sync.senders[].gaps
  [ 10, 11, 9, 10, 12, 9, ... ]      the milliseconds between the packets
```

and the `PACKETS` page draws them: a strip of the last quarter of a
second with a tick for each packet, and below it the gaps as bars.
Evenly spaced packets are an even comb of ticks; bunches are ticks that
crowd together.  The line above says what the gaps are - the average,
the smallest and the largest - and whether they are `even` or `BUNCHED`.

On the P4's own board, sending 94 packets a second, the gaps come out at
8 to 14 ms with an average of about 10.6, and the comb is even.

**At boot the packets arrive in a bunch** - the first ones come out of
the network stack together - and the gaps are zero for a moment.  That
is the one thing this catches that a rate would hide.

## The beat, counted here

The `BEAT` page shows which beat it is.  That number is **not** asked
for: the board with the tracker is asked every few seconds, so what it
says about the beat is a few seconds old by the time it is on the
screen - and a beat that is three seconds old is not a beat.

The beat's **period** is known, though: the rate is in the answer.  So
the place in the beat is worked out from the answer and kept up to date
here:

```
beats = the place in the beat it gave + the time since / the period
```

and the beat count and the beat in the bar follow from it.  The answer
was made somewhere between the asking and the getting, so the middle of
the two is when it was made - a better guess than the moment it
arrived.

What this cannot do is follow a grid that is moving: the P4's own grid
can drift, and this walks the last rate it was told about.  Over the
three seconds between the answers that is a small error, and it is an
error in the same direction every time.

## The presets, and the sync

The remote changes the other board's presets over WLED's own API:

```
POST http://<the board with the tracker>/json
{"preset": 3}
```

and that board does what WLED always does with it: it changes, and it
**broadcasts** its notifier, so the other boards in the sync group follow
its look. The remote is a WLED board like any other, so it can be put in
that sync group as well (Settings -> Sync) and follow along.

What the notifier carries is the look - the brightness, the colour, the
effect, the palette - and not the preset: that is WLED's sync, and it has
always been that way.


## The TF card

The card is on the **same SPI bus as the panel**: the same clock, the
same data lines, and only the card's chip select is its own (GPIO9).  The
bus carries one transaction at a time, so the two must not talk at once -
the panel's picture and the card's protocol would interleave, and both
would be wrong.

So the card is mounted **once, at boot**, between the panel's bring-up
(which makes the bus) and the first picture (which is sent in the loop),
and it is not touched again while the panel is running.

It is mounted with ESP-IDF's FAT filesystem over the SPI card driver -
`esp_vfs_fat_sdspi_mount()`, the same way the vendor's own demo mounts it
- and what is on it is read at the same time.  The `BOARD` page says
what it found:

```
tf: 116 MB  4 files
```

and the JSON has the lot:

```
/json/ar  ->  C5Remote.sd
  { "mounted": true, "status": "mounted", "size_mb": 116, "files": 4,
    "listing": "System Volume Information ALPHATHETA REC PIONEER Contents" }
```

**The card is never formatted.**  `format_if_mount_failed` is off: a card
that cannot be mounted is a card that is reported, not one that is
wiped.

**The headers are not where the P4 gets them.**  The P4's build (Troy's
own Arduino-ESP32) has FATFS's include path in the framework's list;
the C5's build (the platform's own) has the SD card drivers but not
FATFS, so two include paths are added to the C5's environment:

```
-I $PROJECT_PACKAGES_DIR/framework-arduinoespressif32-libs/esp32c5/include/fatfs/src
-I $PROJECT_PACKAGES_DIR/framework-arduinoespressif32-libs/esp32c5/include/fatfs/vfs
```

The SD headers themselves need nothing extra: `driver/sdspi_host.h` and
`driver/sdmmc_host.h` are already on the path.  (In IDF 5.5
`sdmmc_host.h` lives in `driver/`, under `esp_driver_sdmmc`, not in
`sdmmc/` - that is where the C5's copy is, and where the P4's is too.)

## The board's hardware

```
  GPIO6   LCD SCLK        GPIO27  I2C SDA
  GPIO7   LCD MOSI        GPIO26  I2C SCL
  GPIO5   LCD MISO        GPIO28  BOOT button (a button once it has booted)
  GPIO4   LCD DC          GPIO2   KEY_L (previous)
  GPIO8   LCD CS          GPIO3   KEY_R (next)
  GPIO9   TF card CS      GPIO13/14  USB (D-/D+)
```

**The schematic is the authority for all of this.** It is in the vendor's
own repository, and it is a PDF with the text in it:

```
https://github.com/waveshareteam/ESP32-C5-LCD-2.73
  schematic/ESP32-C5-LCD-2.73.pdf
```

It is where the button names come from - KEY_L on GPIO2, KEY_R on GPIO3,
BOOT on GPIO28, each with a 10K pull-up and a switch to ground - and where
PWR is found to be on the power latch's KEY input rather than on a GPIO of
the ESP32-C5.  The demo code in the same repository is where the vendor's
own pin assignments come from, but it does not use the buttons at all.

The CH32V003 IO expander (0x24) holds the LCD's reset line (IO0) and the
LCD's backlight (a PWM output), and its own IO4..IO14 are on the header.

| Register | What            |
|----------|-----------------|
| 0x02 MODE    | 16 bits, one per IO. 1 = output, 0 = input. |
| 0x03 OUTPUT  | 16 bits, the value driven on the outputs. |
| 0x04 INPUT   | 16 bits, what the inputs read. |
| 0x05 PWM     | 1 byte, the backlight's duty (0..255). |
| 0x06 ADC     | 16 bits, the battery's voltage. |
| 0x07 RTCINT  | 16 bits, the RTC's interrupt state. |

### GPIO27 is a boot strap

On the ESP32-C5, **GPIO27 is a boot strap**: it has to be HIGH when the
chip resets, and GPIO27 low is an invalid combination - the chip comes up
with the wrong flash configuration and cannot read the flash.

On this board GPIO27 is the **I2C SDA line**.  The I2C master pulls it
low while it talks, so a reset that lands in the middle of a transaction
can strap the chip low and leave it unable to boot.

That is a property of the board, not of the firmware, and it is worth
knowing before anything else is blamed.  The remote keeps the I2C traffic
small - one short read of the expander every 50 ms (which is the debounce
anyway) and the IMU once a second - so the window is small.

This is the most likely reason a reset can leave a board that will not
boot again.

## The display

WLEDMM's display layer is the P4's: a DSI/PPA pipeline that scales the LED
bus into a monitor's framebuffer. It has no drawing primitives in it at all,
and this board is a different chip with an SPI panel, so it gets its own:

* the picture lives in frame buffers in PSRAM (320 x 320 x 2 bytes, RGB565),
* the drawing happens there,
* only the **rows** that changed are sent to the panel,
* the text is WLED's own console font (`wled00/src/font/console_font_6x8.h`).

The panel itself is ESP-IDF's `esp_lcd` with the ILI9488's panel driver in
`wled00/src/dependencies/esp_lcd_ili9488`, in **18 bit mode**. The panel's
**reset line is on the IO expander**, so the expander is what resets it.

### 18 bits, not 16

The vendor's own demo for this board runs the panel at 18 bits
(`COLMOD=0x66`, RGB666) and that is what works.  16 bit mode was tried
first and gave **a flat grey screen**: the panel was told `COLMOD=0x55`
and fed RGB565 bytes, and this panel does not show that.  There is also
no `esp_lcd_panel_reset()` in the vendor's flow without it, so the panel
never came out of its reset state.

In 18 bit mode the driver converts the picture from RGB565 to RGB666 on
the way out, into a buffer of its own (a band's worth of pixels, three
bytes a pixel), so **the picture in the frame buffer is still RGB565** and
the conversion is the driver's business.

### The window is set for every band

The panel **starts each block of pixels at the start of the window**, so a
window set once for the whole screen has every band land on the same
rows.  That is what the grey screen with a picture that never went past
the top was: a window set once, streaming the whole picture into it.

So the window is set for **every band** - eight rows at a time, forty
bands to a screen.  A row at a time was tried first and was far too slow:
the window has to be set before every block of pixels, and each of those
is a round trip that waits for the one before it.  Six hundred and forty
of them for one screen, and the screen took a second.  A band at a time
is forty.

The band is also the driver's limit: it converts a whole rectangle at a
time and will not take one larger than its buffer.

### Only the rows that changed

The panel keeps what it was last given, so the board keeps a **third**
copy of the picture - what the panel is showing now - and compares the
new picture with it **row by row**.  Only the rows that differ go over
the wire, in runs of consecutive dirty rows.

A row is the finest grain that is worth it: the window can be set to any
run of rows, and a row is 320 pixels.  The meter moves by a pixel or two
at a time, so sending whole bands for it would send most of the screen
for nothing.  In practice a moving meter sends a handful of bands, not
forty.

The frames go out **asynchronously**, with a budget: the send picks up
where it left off when the budget runs out, so a full screen goes out
over a few loops of the main loop instead of stalling one of them.

### Seeing the screen without looking at it

`GET /c5screen.bmp?row=<n>` is what the panel is showing, as a picture: a
BMP of 40 rows of the screen, and the whole screen is eight of them. A
quarter of a megabyte in one request is more than this board's Wi-Fi likes
- the driver gives up in the middle of it - so it comes in tiles.

```
python tools/c5_screen.py <the C5's address> c5.png
```

puts the tiles back together and writes a PNG. That is the only way to see
the screen from a desk.

### What the panel is sent

**Three** pictures are kept, all of them in PSRAM (200 KB each):

| Buffer        | What it is                                                    |
|---------------|---------------------------------------------------------------|
| `_fb`         | what is drawn into                                            |
| `_fbShow`     | what is sent - the finished picture                           |
| `_fbPanel`    | what the panel is showing now - the picture the new one is compared against |

The drawing goes into one and the panel is sent the other, and they are
swapped when a frame is finished: with one picture the panel can be sent
half of one frame and half of the next, and a page that changes shows as a
mixture of both.

The bytes go out as the chip holds them - little-endian, so the low byte
of a pixel is the blue - and in 18 bit mode **the driver does the
conversion itself** (RGB565 to RGB666), so there is no swapping to do
here.  (In 16 bit mode the driver hands the picture over as it is, and
the swap would be ours to do.  That mode does not show anything on this
panel, so the question does not arise.)

RGB565 puts three of the green's six bits in the low byte and three in
the high one, so the green is the awkward colour and has to be put
together from both bytes.

### Talking to the other board

ESP-IDF's HTTP client has two ways to make a request, and only one of
them sends a body: `open()` and `fetch_headers()` never do, so the other
board got an empty one and answered 400. `esp_http_client_perform()` is
the one that sends it.

The status code is also only known **after** the headers have been
fetched - asking for it before that always answers zero, which is what
made the remote think the board was not answering at all.

And the action is not done where it is asked for: the tempo's actions make
an HTTP request, and when the request came in over the network that
request was made from the web server's own task, inside the handling of a
request - and the HTTP client wants the network task, which is that one.
The board fell over. So the action is remembered and the main loop does
it, and a button press goes the same way.

## Building and flashing

```
pio run -e esp32c5_8MB_troyhacks_lcd
python tools/c5_flash.py COM10
python tools/c5_monitor.py 30 COM10
```

**The flashing is not the ordinary one.** The ESP32-C5's USB Serial/JTAG
does not like the stub flasher: esptool starts it, and then the first write
times out. The ROM's own flasher does the job - it is slower, and it works.
So `tools/c5_flash.py` flashes with `--no-stub`.

The offsets on this chip are the C5's own: the **bootloader goes to 0x2000**
(not 0x0 and not 0x1000), the partition table to 0x8000 and the application
to 0x10000.

**A wedged board.** A board that will not boot comes up as a loop of

```
ESP-ROM:esp32c5-eco3-20250704
rst:0x7 (TG0_WDT_HPSYS),boot:0x3f (SPI_FAST_FLASH_BOOT)
Core0 Saved PC:0x4003abb0
```

with the ROM crashing before it can load the application - and with **no
application output at all**, not even the boot log. The USB download mode
still works, so the flash can still be written, and what is written is
correct: esptool verifies the hash of every byte it puts down.

It does not come back.  What has been tried, with the board in that state:

* the application rewritten (the hash checks out, and the known-good
  firmware does not boot either),
* the filesystem wiped and the configuration lost with it,
* the flash written in DIO and in QIO, at 40 MHz and at 80 MHz,
* the ROM's own erase (which only works for small regions - 4 KB is fine,
  4 MB answers "0106"),
* the stub flasher's erase (which times out).

What is left is the power: **the board has to be unplugged and plugged in
again**. There is no reset button on it - the power button cuts the
battery side, and the USB keeps the board alive.

## The network

The board needs the credentials of the network it belongs to, so that a
board whose config was wiped still finds its way home.  They are **not** in
the build files: a network's name and its password have no business in a
file that is tracked and shared.  They go in `wled00/my_config.h`, which is
ignored by git, like every other board's settings:

```c
#define CLIENT_SSID "your network"
#define CLIENT_PASS "your password"
```

The ESP32-C5 is a dual-band chip, and the board comes up in the automatic
band mode: 2.4 GHz and 5 GHz, and the network's name decides which one.  A
board that has to be told which band to use has that in its configuration
(`wifi_band`: 0 = both, 1 = 5 GHz only, 2 = 2.4 GHz only).

The board can also be given the address of the board with the tracker
after the fact, over the API:

```
curl -X POST -d '{"C5Remote":{"host":"192.168.6.249"}}' http://<the C5>/json
```

## The two boards' builds

The C5 and the P4 share one PlatformIO platform and one toolchain (the
RISC-V toolchain is a large download, and there is no point in having two).
They do not share the **framework**: the P4 pins the fork with the P4's
library set, and the C5 uses the platform's own.  Building one after the
other therefore replaces the framework, and the next build of the other
board is a full one.

## Zigbee

The ESP32-C5 has an 802.15.4 radio, and the Arduino core **already ships
the whole Zigbee stack for it** - the headers and the compiled libraries,
ZBOSS and all of it, for the C5 (`espressif__esp-zigbee-lib` and
`espressif__esp-zboss-lib` in the C5's library set, with `CONFIG_ZB_ENABLED`
already on).

The C++ side of it - the core's own `Zigbee` library, with 30 endpoint
classes and 28 examples (an occupancy sensor, a contact switch, a range
extender, a gateway) - is **not reachable from here**.  The include fails
with "Zigbee.h: No such file or directory" in every arrangement: in the
header or in the source, with quotes or with brackets, inside the guard or
outside it.  (`lib_extra_dirs` over the framework's libraries does not
help either - it is the same directory the finder already scans.)

So the Zigbee build is **not in this repository yet**, and there are two
ways to get there:

* **Vendor the library** into `wled00/src/dependencies/zigbee/` (where the
  ILI9488's driver lives too - that one *is* found, because the project
  scans its own files).  But the project's source filter is `+<**/*.cpp>`,
  so a vendored copy would be compiled into **every** environment - the
  P4's, the S3's - and every one of them would want the Zigbee component.
  It wants a directory of its own and a source filter that leaves it out
  everywhere else.
* **Use ESPHome's Zigbee component**, which already supports the ESP32-C5
  as an end device or a router and talks to ZHA or Zigbee2MQTT directly.
  That is twenty minutes of YAML against a day of vendoring - and it is
  what I would do.

The rest of this section is what the Arduino way would need: the library
above, `tools/WLED_ESP32_32MB_ZIGBEE.csv` (this board's table with the two
Zigbee partitions added after the filesystem), and `-DZIGBEE_MODE_ED` (an
end device) or `-DZIGBEE_MODE_ZCZR` (a router).

**What Home Assistant needs.**  The C5 cannot be a ZHA radio: there is no
zigpy radio for the C5, and Espressif's own NCP path is not shipped in the
Arduino core.  So there are two shapes:

* **The C5 joins a network that already has a coordinator.**  The C5 is an
  end device or a router; Home Assistant needs nothing new beyond the
  coordinator you already have (ZHA with a stick, or Zigbee2MQTT).  This is
  the small, certain experiment.
* **The C5 is the coordinator.**  That means a ZBOSS NCP firmware (not the
  Arduino library - the core has no NCP app) and Zigbee2MQTT's `zboss`
  adapter, which calls itself experimental; the ZHA route is a HACS
  integration that is broken on recent Home Assistant.  Do not start here.

**The radio.**  Three things matter on this board:

* The TX power comes up at **maximum** (+20 dBm), which with the WROOM-1's
  2 dBi PCB antenna is over the European 20 dBm EIRP limit.  There is no
  Kconfig for it; it takes a runtime `esp_ieee802154_set_txpower()`.  A
  real deployment runs at 5-10 dBm.
* There is **no external antenna**: the WROOM-1 has its PCB antenna, and
  the 2.4 GHz path is shared by the Wi-Fi, the BLE and the 802.15.4.
* **Coexistence is the real risk.**  Espressif rates Router + Wi-Fi as
  "supported but performance is unstable" (C1) and End Device + Wi-Fi as
  stable.  A board that is also serving WLED's Wi-Fi wants to be an **end
  device**.

**The size.**  Plenty of room: an end device with the Wi-Fi stack is
36 KB of RAM and 650 KB of flash.

