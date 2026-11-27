# The HUB75 panel driver

This directory is the native HUB75 driver: a LED panel driven straight from
the chip's own pins, with no receiving card in between.  It is **ported from
[MoonLight](https://github.com/MoonModules/MoonLight)** (GPL-3.0, like
WLED-MM), and it is meant to be shared back and forth with that project.

MoonLight's HUB75 driver is the sibling of its `PanelCardDriver`, which drives
the same panels over Ethernet to a ColorLight card.  This one is the local
path: fourteen lines of the chip's own, and a bit-plane encoder that feeds
the panel's peripheral.

## The address, leading or lagging

MoonLight's encoder makes the row address **lag the data by one row**: row
`r`'s words name row `r - 1`, and the latch word moves the address to `r` in
the dark.  That is right on a panel whose shift register is **shared** between
the rows: the data goes into the one register and the latch puts it in the row
the address names.  It also keeps the picture on the panel in the frame it was
encoded, with no frame of latency.

On a panel whose shift register **belongs to the row**, the lag puts the data
in the wrong row: the picture comes out one row low.  The classic HUB75
timing - the address is set, the row's data is clocked into it, the latch
strobes - has no such problem and is right on both kinds of panel, at the
price of one frame of latency.

`Hub75Geometry::addressLeadsData` chooses between the two, and the LED
settings page exposes it ("Address leads the data").  Leading is the default
here; MoonLight's own panel takes the lagging one, so the setting is worth
keeping on both sides.

The lit window follows the address too.  With the address lagging, the first
row of a plane lights the last row of the plane before, so it takes that
plane's window; with the address leading, every row takes its own plane's
window, and the picture is even.

## The files, and where they come from

| This file | MoonLight's file | What changed |
|---|---|---|
| `Hub75Bus.h` | `src/platform/Hub75Bus.h` | The namespace only: MoonLight's `mm` is `hub75` here, plus `Hub75Geometry::addressLeadsData` (see below). |
| `Hub75Encode.h` | `src/platform/Hub75Encode.h` | The namespace only, plus the leading-address branch. |
| `Hub75Panel.h` / `Hub75Panel.cpp` | `src/platform/esp32/platform_esp32_hub75.cpp` | The namespace, the naming of the API (`init`, `start`, `buffer`, `deinit`), MoonLight's `platform::` helpers replaced by the plain ESP-IDF ones, and the pin bookkeeping moved up into the WLED bus. |

The namespace rename is the only thing to undo when a change goes back to
MoonLight: `hub75::` becomes `mm::`, and the `hub75::` prefix in the driver
becomes `mm::platform::`.

## What the pieces do

* **`Hub75Bus.h`** — the bus contract: which bit of the 16-bit bus word
  carries which panel line, and the geometry (`Hub75Geometry`) that sizes one
  frame: the panel's width and height, its scan rate, its bit depth and how
  long each plane is lit.
* **`Hub75Encode.h`** — the encoder.  It turns one RGB picture into the bit
  planes the panel's peripheral clocks out: a plane per bit, a row address
  that lags the data by one row, the latch riding the last word of a row, and
  the brightness carried by how long each plane is lit rather than by the
  values.
* **`Hub75Panel.cpp`** — the peripheral half.  Two backends:
  * **LCD_CAM** — the chip's i80 bus, sixteen bits wide, driven by the
    peripheral's own DMA.  A refill task queues the frame again as each scan
    completes, so the panel is scanned forever.
  * **PARLIO** — the parallel IO transmitter (the ESP32-P4 and the S31 have
    one; the S3 does not).  It loops the frame on its own, so its callback
  only counts the scans.

## Why the panel is a different kind of output

A strand latches a frame and holds it.  A panel lights only while it is
clocked, so there is no "show" call per frame: the peripheral scans one
buffer forever, and the bus writes the next frame into it between scans.
The frame therefore has to live in internal memory (the peripheral keeps
clocking while the transfer engine lags on an external read), and a frame
that is being rewritten while it is scanned tears — a fraction of a percent
of brightness, which is invisible at the refresh rates a panel runs at.

## The WLED side of it

The bus itself (`BusHub75`, in `../bus_manager.cpp`) is WLED's: it holds the
picture the way WLED's 2D matrix does, so the effects, the segments and the
LED map work exactly as they do for a strip.  The LED count is the panel's
width times its height, and the picture fills it row by row.

The fourteen lines are in ribbon order - `R1, G1, B1, R2, G2, B2, A, B, C,
D, E, CLK, LAT, OE` - and the settings page names them one by one.

The brightness is the panel's lit time, so it does not scale the values: the
picture is held as it was drawn, and `getPixelColorRestored()` returns it
unchanged.

With `WLEDMM_REMAP_AT_OUTPUT` (the P4's build) the LED map is applied here,
at the output, and not in the renderer: the map is inverted, so it is indexed
by the panel's pixel and holds the matrix position the pixel takes its
colour from.
