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
| `BOARD`  | this board: the network, the uptime, the memory, the IMU and the expander's battery reading |

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

And the **JSON document** is 40 KB (`JSON_BUFFER_SIZE`) rather than the 54 KB
the boards with more RAM use.  It is a static array in DRAM, and every byte
of it comes out of the heap that the tasks have to be carved out of: with
54 KB the heap had 15 KB in one piece, the web task took 12 KB of it, and
the two UDP tasks - the audio sync's listener and the notifier's - had
nothing left ("listen(): failed to start task").  Without a listener there
is no audio sync, and without the web server there is no way to see the
screen.

The heap's *largest block* is the number that matters here, not the heap's
free total: a task's stack has to be one piece.  The console prints it
once a second, beside the IMU's numbers.

Three on the chip, and up to eleven on the IO expander:

| Button                | Pin                | What it does                                    |
|-----------------------|--------------------|-------------------------------------------------|
| previous              | GPIO2              | the previous page; held: the previous preset on the other board |
| next                  | GPIO3              | the next page; held: the next preset on the other board |
| boot                  | GPIO28             | **mark the downbeat** on the other board; held: lock or unlock its clock |
| IO4 .. IO14          | the IO expander    | ordinary WLED buttons - the debounce, the long press, the double press and the macros are all the core's |

The expander's buttons are answered for by the usermod, so the core's own
button logic does all of it. The core asks the usermods first:

```cpp
// wled00/fcn_declare.h
virtual bool buttonState(uint8_t index, bool &pressed) { return false; }
```

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


## The board's hardware

```
  GPIO6   LCD SCLK        GPIO27  I2C SDA
  GPIO7   LCD MOSI        GPIO26  I2C SCL
  GPIO5   LCD MISO        GPIO28  BOOT button (a button once it has booted)
  GPIO4   LCD DC          GPIO2   previous button
  GPIO8   LCD CS          GPIO3   next button
  GPIO9   TF card CS      GPIO13/14  USB (D-/D+)
```

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

* the picture lives in a frame buffer in PSRAM (320 x 320 x 2 bytes, RGB565),
* the drawing happens there,
* only the part that changed is sent to the panel, through a buffer in
  internal RAM (the SPI's DMA cannot read the frame buffer where it is),
* the text is WLED's own console font (`wled00/src/font/console_font_6x8.h`).

The panel itself is ESP-IDF's `esp_lcd` with the ILI9488's panel driver in
`wled00/src/dependencies/esp_lcd_ili9488`, in 16 bit mode, so the picture
goes out as it is with no conversion in between. The panel's **reset line is
on the IO expander**, so the expander is what resets it.

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

The picture is held as the chip holds it - little-endian, so the low byte
of a pixel is the blue - and the ILI9488's 16 bit mode takes the high
byte first, so the bytes are swapped on the way out. (In 18 bit mode the
driver does the conversion itself and gets this right; in 16 bit mode it
hands the picture over as it is, so the swap is ours to do.)

Two pictures are kept, one to draw into and one to send, and they are
swapped when a frame is finished: with one picture the panel can be sent
half of one frame and half of the next, and a page that changes shows as a
mixture of both.

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

The build carries the credentials of the network the board belongs to, so
that a board whose config was wiped still finds its way home:

```
-D CLIENT_SSID=\"YOUR_NETWORK\"
-D CLIENT_PASS=\"YOUR_PASSWORD\"
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

It is a build flag and a partition scheme:

```ini
build_flags             = -DZIGBEE_MODE_ED     ;; or -DZIGBEE_MODE_ZCZR
board_build.partitions  = zigbee_8MB.csv       ;; the .csv suffix is REQUIRED
```

and the library is the core's own `Zigbee` (in
`framework-arduinoespressif32/libraries/Zigbee`), with 30 endpoint classes
and 28 examples - an occupancy sensor, a contact switch, a range extender, a
gateway.

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

**The partitions.**  Zigbee keeps its network storage in `zb_storage` and
`zb_fct`, so a Zigbee build needs one of the `zigbee*` schemes - there is
no 32 MB one, so either the 8 MB scheme is used and the rest of the flash
is wasted, or a scheme with the Zigbee partitions is written for this
board.  This board's own table (`tools/WLED_ESP32_32MB.csv`) has neither.

**The size.**  Plenty of room: an end device with the Wi-Fi stack is
36 KB of RAM and 650 KB of flash.

