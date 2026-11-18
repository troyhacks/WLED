/*
 * c5_board.h - the hardware of the WaveShare ESP32-C5-LCD-2.73.
 *
 * Three things live on this board that the rest of WLED knows nothing about:
 *
 *  1. the CH32V003 I2C IO expander at 0x24. It holds the LCD's reset line
 *     and the LCD's backlight (a PWM output), and its own IO pins are on
 *     the board's header - those become WLED buttons.
 *  2. the QMI8658 IMU at 0x6B. For now it only prints to the console.
 *  3. the three buttons on the chip itself: GPIO2 (previous), GPIO3 (next)
 *     and GPIO28 (the boot button, which is a button after the boot).
 *
 * The IO expander's register map is the vendor's, taken from the Arduino
 * demo for this board:
 *
 *   0x02  MODE    16 bits, one per IO. 1 = output, 0 = input.
 *   0x03  OUTPUT  16 bits, the value driven on the outputs.
 *   0x04  INPUT   16 bits, what the inputs read.
 *   0x05  PWM     1 byte, the backlight's duty (0..255).
 *   0x06  ADC     16 bits, the battery's voltage.
 *   0x07  RTCINT  16 bits, the RTC's interrupt state.
 *
 * All of the word registers are little-endian.
 */

#pragma once

#include "wled.h"

#if defined(WLEDMM_C5_DISPLAY)

// The TF card.
//
// The card is on the SAME SPI bus as the panel: the same clock, the same
// data lines, and only the card's chip select is its own.  The bus
// carries one transaction at a time, so the two must not talk at once -
// the panel's picture and the card's protocol would interleave, and both
// would be wrong.
//
// So the card is mounted ONCE, at boot, before the panel starts sending
// anything, and it is not touched again while the panel is running.
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include <dirent.h>

#define C5_SD_MOUNT "/sdcard"

// ---------------------------------------------------------------------------
// The board's wiring
// ---------------------------------------------------------------------------
#define C5_LCD_SCLK      6      // SPI clock
#define C5_LCD_MOSI      7      // SPI data out
#define C5_LCD_MISO      5      // SPI data in
#define C5_LCD_DC        4      // data/command
#define C5_LCD_CS        8      // chip select
#define C5_SD_CS         9      // the TF card shares the SPI bus
#define C5_I2C_SDA      27
#define C5_I2C_SCL      26
#define C5_SPI_HOST     SPI2_HOST

#define C5_BTN_PREV      2      // "previous" / menu
#define C5_BTN_NEXT      3      // "next" / menu
#define C5_BTN_BOOT     28      // the boot button, a button once it has booted

// The IO expander
#define C5_IOEXP_ADDR        0x24
#define C5_IOEXP_REG_MODE    0x02
#define C5_IOEXP_REG_OUTPUT  0x03
#define C5_IOEXP_REG_INPUT   0x04
#define C5_IOEXP_REG_PWM     0x05
#define C5_IOEXP_REG_ADC     0x06
#define C5_IOEXP_REG_RTCINT  0x07

#define C5_IOEXP_LCD_RST     0      // the panel's reset line
#define C5_IOEXP_BTN_FIRST   4      // IO4..IO14 are on the header
#define C5_IOEXP_BTN_LAST   14
#define C5_IOEXP_BTN_COUNT  (C5_IOEXP_BTN_LAST - C5_IOEXP_BTN_FIRST + 1)

// The IMU
#define C5_IMU_ADDR      0x6B

// ---------------------------------------------------------------------------
// The IO expander
// ---------------------------------------------------------------------------
class C5IoExpander {
  public:
    bool begin(TwoWire &wire);

    // 1 = output, 0 = input. The bits are the IO numbers.
    bool setMode(uint16_t pinMask);

    bool writeOutput(uint16_t value);
    bool setPin(uint8_t pin, bool high);
    bool readInput(uint16_t &value);
    bool getPin(uint8_t pin, bool &high);

    // The backlight: 0..100 %.
    bool setPwm(uint8_t percent);

    // The battery's voltage, in the expander's own counts.
    uint16_t readAdc();
    uint8_t  readRtcInt();

  private:
    bool writeWord(uint8_t reg, uint16_t value);
    bool readWord(uint8_t reg, uint16_t &value);
    TwoWire *_wire = nullptr;
    uint16_t _outputs = 0xFFFF;   // what we believe is on the output register
};

// The mode the remote uses: the LCD's reset is an output, the header's IO
// pins are inputs (they are the buttons), and everything else is left as
// the vendor set it up.
//
// IO2 stays an input because that is how the vendor's own demo has it, and
// nothing on this board is known to drive it.
#define C5_IOEXP_MODE  (uint16_t)(0x800B)

// ---------------------------------------------------------------------------
// The TF card
//
// It is mounted with ESP-IDF's FAT filesystem over the SPI card driver -
// the same way the vendor's own demo mounts it - and what is on it is
// read once, at boot.
//
// The card is not formatted under any circumstances: a card that cannot
// be mounted is a card that is reported, not one that is wiped.
// ---------------------------------------------------------------------------
class C5SdCard {
  public:
    // Mounts the card and reads what is on it.
    //
    // The SPI bus has to be there already (the panel's bring-up makes
    // it), and the panel must not be sending anything yet - the card and
    // the panel share the bus.
    bool begin();

    bool mounted() const { return _mounted; }
    const char *status() const { return _status; }
    uint64_t sizeBytes() const { return _sizeBytes; }
    // The names in the card's root directory, as many as fit.
    const char *listing() const { return _listing; }
    uint8_t fileCount() const { return _fileCount; }

  private:
    bool        _mounted = false;
    const char *_status = "not tried";
    uint64_t    _sizeBytes = 0;
    char        _listing[160] = {0};
    uint8_t     _fileCount = 0;
};

// ---------------------------------------------------------------------------
// The IMU
//
// The driver is the one from the board's own Arduino demo (see
// qmi8658c.h). It is not written here.
//
// The accelerometer is read in m/s2 and the gyroscope in rad/s, which is
// what that driver gives.
// ---------------------------------------------------------------------------
class C5Imu {
  public:
    bool begin();
    // The same, in a task of its own.
    //
    // The driver's calibration waits two and a quarter seconds for the
    // chip to finish, and the board does not need the IMU to be up to be
    // useful: the screen is.  So the IMU is brought up in parallel with
    // the Wi-Fi's wait, and the two do not add up.
    bool beginAsync();
    // Reads the accelerometer and the gyroscope. Returns false when the chip
    // did not answer or had no fresh numbers.
    bool read(float acc[3], float gyr[3]);
    bool present() const { return _present; }

  private:
    volatile bool _present = false;
};

#include "qmi8658c.h"
#include "qmi8658c.cpp"

#include "c5_board.cpp"

#endif  // WLEDMM_C5_DISPLAY
