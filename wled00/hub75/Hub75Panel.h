#pragma once

/*
    WLED-MM-P4 - HUB75 panel driver.

    The peripheral half of the panel driver: the bus, the frame buffer and the
    continuous scan.  The WLED bus above it (BusHub75 in bus_manager.cpp)
    applies the correction and encodes the rendered frame into bit planes; no
    domain logic lives here.

    Ported from MoonLight (https://github.com/MoonModules/MoonLight), file
    src/platform/esp32/platform_esp32_hub75.cpp, which is GPL-3.0 like WLED.
    See hub75/README.md for the mapping.

    @author Ewowi (MoonLight)
    @license GNU GENERAL PUBLIC LICENSE Version 3
*/

#include <cstddef>   // size_t
#include <cstdint>

namespace hub75 {

/// Which peripheral clocks the panel out.
enum class Backend : uint8_t {
    LcdCam = 0,   ///< the ESP32's LCD_CAM, driven as an i80 bus
    Parlio = 1,   ///< the ESP32-P4's (and S31's) parallel IO transmitter
};

/// The label a backend is known by in the UI and in the log.
const char* backendLabel(Backend backend);

/// The panel's fourteen lines, in ribbon order: R1, G1, B1, R2, G2, B2, A, B, C, D, E, CLK, LAT, OE.
/// A pin of -1 is unset: the panel then cannot run.
struct Pins {
    int16_t r1 = -1, g1 = -1, b1 = -1;
    int16_t r2 = -1, g2 = -1, b2 = -1;
    int16_t a = -1, b = -1, c = -1, d = -1, e = -1;
    int16_t clk = -1, lat = -1, oe = -1;
    /// The chips sample the clock on the falling edge on some panels: every pixel lands one column over otherwise.
    bool clkFalling = false;
};

/// The number of lines a panel needs: the fourteen above.
static constexpr uint8_t kHub75PinCount = 14;

/// The opaque handle of an open panel.
struct Hub75Handle {
    void* impl = nullptr;
};

/// The reason the last call refused, or null when it did not.
const char* lastError();

/// Whether the backend can carry a frame of `frameBytes`, on this chip at all.
bool backendAvailable(Backend backend, size_t frameBytes);

/// Bring the panel up for a layout of `width` by `height`; false when it does not run, the reason in lastError().
bool init(Hub75Handle& handle, Backend backend, const Pins& pins, uint16_t width, uint16_t height,
          uint8_t scanRate, uint8_t bitDepth);

/// Arm the endless scan of the frame buffer.
bool start(Hub75Handle& handle);

/// The frame buffer the encoder writes the next frame into, or null when no panel is open.
uint8_t* buffer(Hub75Handle& handle);

/// The measured refresh rate in Hz, averaged over a second.
uint16_t refreshHz(const Hub75Handle& handle);

/// The backend the panel runs on, as its label.
const char* backendName(const Hub75Handle& handle);

/// Stop the scan and release the peripheral; safe to call on a closed panel.
void deinit(Hub75Handle& handle);

}  // namespace hub75
