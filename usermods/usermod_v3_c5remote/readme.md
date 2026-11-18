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

## The buttons

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

`GET /c5screen.bmp` is what the panel is showing, as a picture. It is the
only way to see the screen from a desk.

```
curl -o c5.bmp http://<the C5>/c5screen.bmp
python tools/bmp_to_png.py c5.bmp c5.png
```

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

with the ROM crashing before it can load the application. The USB download
mode still works, so the flash can still be written - but the flash does not
come back until the board has been **powered off and on again**. There is no
reset button on this board, so that means unplugging it.

## The network

The build carries the credentials of the network the board belongs to, so
that a board whose config was wiped still finds its way home:

```
-D CLIENT_SSID=\"YOUR_NETWORK\"
-D CLIENT_PASS=\"YOUR_PASSWORD\"
```

The ESP32-C5 is a dual-band chip, and the board comes up in the automatic
band mode: 2.4 GHz and 5 GHz, and the network's name decides which one.
