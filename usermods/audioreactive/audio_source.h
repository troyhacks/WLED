#pragma once

/* 
   @title     MoonModules WLED - audioreactive usermod
   @file      audio_source.h
   @repo      https://github.com/MoonModules/WLED, submit changes to this file as PRs to MoonModules/WLED
   @Authors   https://github.com/MoonModules/WLED/commits/mdev/
   @Copyright © 2024 Github MoonModules Commit Authors (contact moonmodules@icloud.com for details)
   @license   Licensed under the EUPL-1.2 or later

*/


// #ifdef ARDUINO_ARCH_ESP32
#include <Wire.h>
#include "wled.h"

// IDF v5 new I2S driver — replaces the deprecated <driver/i2s.h>.
#include <driver/i2s_std.h>
#include <driver/i2s_pdm.h>
#include <driver/i2s_types.h>

// IDF v5 new I2C master driver — needed to install our own i2c_master_bus
// for the esp_codec_dev library when Wire's bus handle is not exposed.
#include <driver/i2c_master.h>

// esp_codec_dev - official espressif codec driver for ES8311, ES8388, ES8374, etc.
// esp_codec_dev.h provides core types; codec headers provide codec-specific structs.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <es8311_codec.h>
#include <es8388_codec.h>
#include <es8374_codec.h>
#include <es7210_adc.h>
#include <es7243_adc.h>
#include <es7243e_adc.h>
#include <es8389_codec.h>
#include <zl38063_codec.h>
#include <cjc8910_codec.h>
#endif

// Sample-rate type (the legacy i2s_config_t.sample_rate was "int" until IDF 4.4.x).
#define SRate_t uint32_t

// Read-back of the ES8311's configuration registers.
//
// Why this exists: the codec's clock manager (0x01-0x08) and ALC (0x18) are the
// two parts of the audio chain that no driver - not this one, not ESPhome's, not
// Espressif's own - ever verifies against the datasheet. They are also the two
// that decide, respectively, WHICH FREQUENCY the analysis is looking at and
// whether the gain is steady. On this build the app's console is UART0, so the
// serial dump of these registers cannot be read; keeping them here lets the
// info page show them, which is the difference between "we think it is right"
// and "we read it off the chip".
//
// The register meanings are taken from the Linux ALSA driver
// (sound/soc/codecs/es8311.c) and Espressif's own es8311 component, which
// agree on the bit layout:
//
//   0x01 bit7   MCLK_SEL (0 = use the MCLK pin)
//   0x02 bits7:5 DIV_PRE-1 (MCLK pre-divider), bits4:3 MULT_PRE (1/2/4/8x)
//   0x03 bits7:6 fs_mode, bits4:0 adc_osr
//   0x04 bits4:0 dac_osr
//   0x05 bits7:4 adc_div-1, bits3:0 dac_div-1
//   0x06 bit5   BCLK invert, bits4:0 BCLK divider-1
//   0x07 bits3:0 LRCK divider high
//   0x08        LRCK divider low
//   0x14 bits3:0 PGAGAIN (0 = 0 dB, 10 = +30 dB), bit4 LINSEL
//   0x18 bit7   ALC enable, bit6 automute enable, bits3:0 ALC winsize
//   0x19 bits7:4 ALC max level, bits3:0 ALC min level
struct AR_CodecRegs {
  bool     valid = false;   // true if the chip answered
  uint8_t  v[32] = {0};     // register value, 0x00..0x1F
};

// Single instance shared by the codec setup code and the info page.
inline AR_CodecRegs& arCodecRegs() {
  static AR_CodecRegs regs;
  return regs;
}

constexpr i2s_port_t AR_I2S_PORT = I2S_NUM_0;       // I2S port to use (do not change!  I2S_NUM_1 possible but this has
                                                    // strong limitations -> no MCLK routing, no PDM support on some targets

// Read the ESP32-P4's I2S clock back out of the clock registers.
//
// The P4's I2S has a fractional divider:
//     MCLK = SRC / (N + num/den)
// programmed in peri_clk_ctrl12 (N, x, y) and peri_clk_ctrl13 (z, yn1). The HAL
// writes those four fields from a (integer, numerator, denominator) triple:
//
//     yn1 = (num * 2 > den)
//     z   = yn1 ? (den - num) : num
//     x   = den / z - 1
//     y   = den % z
//
// so the triple can be recovered from the registers exactly:
//
//     den = (x + 1) * z + y
//     num = yn1 ? (den - z) : z
//
// This is the check that matters at boot. The codec never reports its own
// clock, and the I2S driver never reports the clock it programmed - so if the
// IDF's clock tree cannot produce exactly 256 x the requested rate, the
// divider lands somewhere else, the codec follows it, and the whole spectrum
// ends up on the wrong frequency axis. Nothing else in the chain would show
// it: the level, the noise floor and the band values would all look
// perfectly normal, just at the wrong frequencies.
//
// Only the XTAL source is decoded. APLL and an external clock are not fixed
// frequencies, so their MCLK cannot be computed from registers alone.
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  #include "soc/hp_sys_clkrst_struct.h"
  #include <math.h>

  // The P4's crystal. CONFIRMED at boot by esptool ("Crystal frequency: 40MHz")
  // and by the IDF's own I2S_LL_XTAL_CLK_FREQ.
  #define AR_P4_XTAL_HZ 40000000u

static bool arReadP4I2SClock(uint32_t &mclkHz, uint32_t &fsHz) {
  mclkHz = 0;
  fsHz = 0;

  uint32_t n = 0, x = 0, y = 0, z = 0, yn1 = 0, srcSel = 0;
  switch (AR_I2S_PORT) {
    case I2S_NUM_0:
      n      = HP_SYS_CLKRST.peri_clk_ctrl12.reg_i2s0_rx_div_n;
      x      = HP_SYS_CLKRST.peri_clk_ctrl12.reg_i2s0_rx_div_x;
      y      = HP_SYS_CLKRST.peri_clk_ctrl12.reg_i2s0_rx_div_y;
      z      = HP_SYS_CLKRST.peri_clk_ctrl13.reg_i2s0_rx_div_z;
      yn1    = HP_SYS_CLKRST.peri_clk_ctrl13.reg_i2s0_rx_div_yn1;
      srcSel = HP_SYS_CLKRST.peri_clk_ctrl11.reg_i2s0_rx_clk_src_sel;
      break;
    #if SOC_I2S_NUM > 1
    case I2S_NUM_1:
      n      = HP_SYS_CLKRST.peri_clk_ctrl14.reg_i2s1_rx_div_n;
      x      = HP_SYS_CLKRST.peri_clk_ctrl15.reg_i2s1_rx_div_x;
      y      = HP_SYS_CLKRST.peri_clk_ctrl15.reg_i2s1_rx_div_y;
      z      = HP_SYS_CLKRST.peri_clk_ctrl15.reg_i2s1_rx_div_z;
      yn1    = HP_SYS_CLKRST.peri_clk_ctrl15.reg_i2s1_rx_div_yn1;
      srcSel = HP_SYS_CLKRST.peri_clk_ctrl14.reg_i2s1_rx_clk_src_sel;
      break;
    #endif
    #if SOC_I2S_NUM > 2
    case I2S_NUM_2:
      n      = HP_SYS_CLKRST.peri_clk_ctrl17.reg_i2s2_rx_div_n;
      x      = HP_SYS_CLKRST.peri_clk_ctrl17.reg_i2s2_rx_div_x;
      y      = HP_SYS_CLKRST.peri_clk_ctrl18.reg_i2s2_rx_div_y;
      z      = HP_SYS_CLKRST.peri_clk_ctrl18.reg_i2s2_rx_div_z;
      yn1    = HP_SYS_CLKRST.peri_clk_ctrl18.reg_i2s2_rx_div_yn1;
      srcSel = HP_SYS_CLKRST.peri_clk_ctrl17.reg_i2s2_rx_clk_src_sel;
      break;
    #endif
    default:
      return false;
  }

  if (srcSel != 0) return false;        // not the XTAL: not a fixed frequency

  const uint32_t den = (x + 1) * z + y;
  if (den == 0) return false;
  const uint32_t num = yn1 ? (den - z) : z;

  const double divisor = (double)n + (double)num / (double)den;
  if (divisor <= 0.0) return false;

  const double mclk = (double)AR_P4_XTAL_HZ / divisor;
  mclkHz = (uint32_t)llround(mclk);
  fsHz   = (uint32_t)llround(mclk / 256.0);
  return true;
}
#endif // CONFIG_IDF_TARGET_ESP32P4

// Number of descriptors in the I2S DMA ring. The ring holds this many blocks of
// BLOCK_SIZE samples, and that depth is the ONLY hard deadline in the audio path:
// if the analysis task overruns it, the driver has nowhere to put the incoming samples
// and the audio drops. Nothing else in the chain has a limit this loose, which is
// worth knowing before treating a stale "time budget" comment as the constraint.
// AudioReactive derives its overrun threshold from this, so keep them in step.
#ifdef WLED_ENABLE_HUB75MATRIX
  #define AR_I2S_DMA_DESCS 18
#else
  #define AR_I2S_DMA_DESCS 24
#endif

// see https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/hw-reference/chip-series-comparison.html#related-documents
// and https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html#overview-of-all-modes
#if !defined(CONFIG_SOC_CPU_HAS_FPU) || defined(ESP8266) || defined(ESP8265)
  // there are two things in these MCUs that could lead to problems with audio processing:
  // * no floating point hardware (FPU) support - the analysis uses float calculations. If done in software, a strong slow-down can be expected (between 8x and 20x)
  // * single core, so analysis task might slow down other things like LED updates
  #if !defined(SOC_I2S_NUM) || (SOC_I2S_NUM < 1)
  #error This audio reactive usermod does not support devices without FPUs and I2S.
  #else
  #warning This audio reactive usermod does not officially support devices without FPUs.
  #endif
#endif

/* ToDo: remove. ES7243 is controlled via compiler defines
   Until this configuration is moved to the webinterface
*/

// Compile-time defaults. These may be overridden at runtime by the
// AudioReactive usermod's persisted config (see i2sBitsPerSample /
// i2sSlot / i2sMaster below).
//
//   I2S_USE_RIGHT_CHANNEL - select RIGHT slot on digital mics (compile-time)
//   I2S_USE_16BIT_SAMPLES - request 16-bit samples (compile-time)
//
//#define I2S_USE_RIGHT_CHANNEL    // (experimental) define this to use right channel (digital mics only)
//#define I2S_USE_16BIT_SAMPLES    // (experimental) define this to request 16bit - more efficient but possibly less compatible

#if defined(WLED_ENABLE_HUB75MATRIX) && defined(CONFIG_IDF_TARGET_ESP32)
  // this is bitter, but necessary to survive
  #define I2S_USE_16BIT_SAMPLES
#endif

// I2S_SAMPLE_RESOLUTION / I2S_data_size are kept as aliases so any
// out-of-tree code that still references them compiles, but they map to the
// new IDF v5 I2S_DATA_BIT_WIDTH_* constants.
#ifdef I2S_USE_16BIT_SAMPLES
#define I2S_SAMPLE_RESOLUTION I2S_DATA_BIT_WIDTH_16BIT
#define I2S_datatype int16_t
#define I2S_unsigned_datatype uint16_t
#define I2S_data_size I2S_DATA_BIT_WIDTH_16BIT
#undef  I2S_SAMPLE_DOWNSCALE_TO_16BIT
#else
#define I2S_SAMPLE_RESOLUTION I2S_DATA_BIT_WIDTH_32BIT
#define I2S_datatype int32_t
#define I2S_unsigned_datatype uint32_t
#define I2S_data_size I2S_DATA_BIT_WIDTH_32BIT
#define I2S_SAMPLE_DOWNSCALE_TO_16BIT
#endif

// Backwards-compatible channel-selection macros. These now map to the new
// IDF v5 I2S_SLOT_* constants. The legacy "left/right swapped" bug from
// IDF 4.4.x (issues #6625, #8538, #9635) is fixed in the new driver, so
// I2S_USE_RIGHT_CHANNEL now genuinely selects the right channel.
#ifdef I2S_USE_RIGHT_CHANNEL
#define I2S_MIC_CHANNEL_SLOT_MASK I2S_STD_SLOT_RIGHT
#define I2S_MIC_CHANNEL_TEXT "right channel only"
#define I2S_PDM_MIC_CHANNEL_SLOT_MASK I2S_PDM_SLOT_RIGHT
#define I2S_PDM_MIC_CHANNEL_TEXT "right channel only"
#else
#define I2S_MIC_CHANNEL_SLOT_MASK I2S_STD_SLOT_LEFT
#define I2S_MIC_CHANNEL_TEXT "left channel only"
#define I2S_PDM_MIC_CHANNEL_SLOT_MASK I2S_PDM_SLOT_LEFT
#define I2S_PDM_MIC_CHANNEL_TEXT "left channel only"
#endif

// PDM RX always uses 16-bit slot width per IDF v5 driver constraints.
#define I2S_PDM_SLOT_WIDTH I2S_DATA_BIT_WIDTH_16BIT


// max number of samples for a single i2s_read --> size of global buffer.
#define I2S_SAMPLES_MAX 512  // same as the frame's hop size

/* Interface class
   AudioSource serves as base class for all microphone types
   This enables accessing all microphones with one single interface
   which simplifies the caller code
*/
class AudioSource {
  public:
    /* All public methods are virtual, so they can be overridden
       Everything but the destructor is also removed, to make sure each mic
       Implementation provides its version of this function
    */
    virtual ~AudioSource() {};

    /* Initialize
       This function needs to take care of anything that needs to be done
       before samples can be obtained from the microphone.

       bitsPerSample: 16, 24, or 32 (selects i2s_data_bit_width_t on the new driver)
       useRightSlot:  false = LEFT slot, true = RIGHT slot (replaces the
                      legacy I2S_USE_RIGHT_CHANNEL compile-time macro)
       i2sMaster:     false = I2S_SLAVE role (rarely used)
    */
    virtual void initialize(int8_t i2swsPin   = I2S_GPIO_UNUSED,
                            int8_t i2ssdPin   = I2S_GPIO_UNUSED,
                            int8_t i2sckPin   = I2S_GPIO_UNUSED,
                            int8_t mclkPin    = I2S_GPIO_UNUSED,
                            uint8_t bitsPerSample = 32,
                            bool    useRightSlot  = false,
                            bool    i2sMaster     = true) = 0;

    /* Deinitialize
       Release all resources and deactivate any functionality that is used
       by this microphone
    */
    virtual void deinitialize() = 0;

    /* getSamples
       Read num_samples from the microphone, and store them in the provided
       buffer
    */
    virtual void getSamples(float *buffer, uint16_t num_samples) = 0;

    /* check if the audio source driver was initialized successfully */
    virtual bool isInitialized(void) {return(_initialized);}

    /* identify Audiosource type - I2S-ADC or I2S-digital.
     * Type_I2SAdc=1 was removed with the I2SAdcSource class (IDF v5 migration);
     * values preserved for ABI compatibility with persisted configs. */
    typedef enum{Type_unknown=0, Type_I2SAdc=1, Type_I2SDigital=2} AudioSourceType;
    virtual AudioSourceType getType(void) {return(Type_I2SDigital);}               // default is "I2S digital source" - ADC type overrides this method

    /* Codec clock manager, for codecs that have one (the ES8311).
       The pre-divider is the codec's MCLK divider; the vendor coefficient tables
       all use 1 for MCLK = 256 x rate. It is settable at runtime so the
       pre-divider can be A/B'd without reflashing. Sources without a clock
       manager ignore it and report 0. */
    virtual void setCodecClockDiv(uint8_t) {}          // default: no clock manager
    virtual uint8_t getCodecClockDiv(void) {return(0);}

    /* The clock the codec is actually being fed.
       getMclkHz() is the MCLK the I2S peripheral is generating, in Hz;
       getClockSampleRateHz() is the sample rate that implies (MCLK / 256).
       Zero means "this source cannot tell" - most cannot, because the clock is
       generated inside the I2S peripheral and the driver never reads it back.

       On the ESP32-P4 the fractional divider behind the I2S clock IS readable,
       so there this is a measurement and not an assumption. It is worth having:
       a codec never reports its own clock, so a wrong MCLK puts the whole
       spectrum on the wrong frequency axis while every other reading - level,
       noise floor, band values - still looks perfectly plausible. */
    virtual uint32_t getMclkHz(void) {return(0);}
    virtual uint32_t getClockSampleRateHz(void) {return(0);}

  protected:
    /* Post-process audio sample - currently on needed for I2SAdcSource*/
    virtual I2S_datatype postProcessSample(I2S_datatype sample_in) {return(sample_in);}   // default method can be overriden by instances (ADC) that need sample postprocessing

    // Private constructor, to make sure it is not callable except from derived classes
    AudioSource(SRate_t sampleRate, int blockSize, float sampleScale, bool i2sMaster) :
      _sampleRate(sampleRate),
      _blockSize(blockSize),
      _initialized(false),
      _i2sMaster(i2sMaster),
      _sampleScale(sampleScale)
    {};

    SRate_t _sampleRate;            // Microphone sampling rate
    int _blockSize;                 // I2S block size
    bool _initialized;              // Gets set to true if initialization is successful
    bool _i2sMaster;                // when false, ESP32 will be in I2S SLAVE mode (for devices that only operate in MASTER mode). Only works in newer IDF >= 4.4.x
    float _sampleScale;             // pre-scaling factor for I2S samples
    I2S_datatype newSampleBuffer[I2S_SAMPLES_MAX+4] = { 0 }; // global buffer for i2s_read
};

/* audioreactive_I2C — IDF v5 new I2C master bus wrapper
   ----------------------------------------------------------
   The audioreactive usermod used to rely on Arduino Wire (legacy I2C driver
   under the hood). On IDF v5 (arduino-esp32 v3.x), Wire.begin() installs
   its own i2c_master_bus_handle_t on a fixed port and does NOT expose the
   handle — making it impossible for the esp_codec_dev library (which
   needs the handle) to share the bus.

   audioreactive_I2C instead installs its own dedicated i2c_master_bus on a
   configurable port (default port 0 on chips with one I2C controller, port 1
   on chips like ESP32-P4 with two — leaving Wire on port 0 for the rest of
   WLED). Codec classes acquire their device handle via addDevice() and
   perform register r/w via writeReg()/readReg() — no Wire dependency.
*/
#ifndef SR_I2C_PORT
  #define SR_I2C_PORT 0        // default to port 0 — share with Wire; we'll tear Wire down first
#endif
#ifndef SR_I2C_FREQUENCY_HZ
#define SR_I2C_FREQUENCY_HZ 50000    // 50 kHz — conservative default; some ESP codec libs
                                     // (e.g. esp_codec_dev's es8311_codec_new on P4)
                                     // need slower SCL to avoid NACKs
#endif

class audioreactive_I2CDevice {
  public:
    audioreactive_I2CDevice() : _dev_handle(nullptr), _addr(0), _freq_hz(SR_I2C_FREQUENCY_HZ) {}
    i2c_master_dev_handle_t handle() const { return _dev_handle; }
    uint8_t addr() const { return _addr; }

  private:
    friend class audioreactive_I2C;
    i2c_master_dev_handle_t _dev_handle;
    uint8_t _addr;
    uint32_t _freq_hz;
};

class audioreactive_I2C {
  public:
    audioreactive_I2C() : _bus_handle(nullptr), _bus_port((i2c_port_t)SR_I2C_PORT), _sda_io(-1), _scl_io(-1) {}

    /* Returns true if the bus is installed and ready. */
    bool isReady() const { return _bus_handle != nullptr; }

    /* The port this bus was installed on. Useful for logging and for passing
       to esp_codec_dev's audio_codec_i2c_cfg_t. */
    i2c_port_t port() const { return _bus_port; }
    i2c_master_bus_handle_t busHandle() const { return _bus_handle; }

    /* Install the i2c_master_bus on the configured port, on the given SDA/SCL
       GPIO pins. Idempotent: if the bus is already installed on the same port+pins,
       it's reused. Returns true on success.

       On arduino-esp32 v3.x Wire.begin() installs an i2c_master_bus_handle_t on
       the same port (default 0) and refuses to share. To take over, we first
       call Wire.end() to release Wire's claim on the peripheral — the rest
       of WLED's Wire-based code (the I2C scan in setup()) will silently no-op
       thereafter, which is acceptable on builds that have moved to the new driver. */
    bool begin(int8_t sda_io, int8_t scl_io) {
      if (sda_io < 0 || scl_io < 0) return false;
      if (_bus_handle && _sda_io == sda_io && _scl_io == scl_io) return true;  // already installed
      if (_bus_handle) end();  // tear down old bus if ports/pins changed

      // Release Wire's bus on the same port before we install ours.
      Wire.end();

      // Some chips (notably ESP32-P4) need a brief delay after Wire.end() before
      // re-installing the bus, otherwise the GPIO mux holds stale Wire settings
      // and subsequent I²C transactions NACK with a phantom slave.
      delay(10);

      i2c_master_bus_config_t bus_cfg = {};
      bus_cfg.i2c_port          = _bus_port;
      bus_cfg.sda_io_num        = (gpio_num_t)sda_io;
      bus_cfg.scl_io_num        = (gpio_num_t)scl_io;
      bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
      bus_cfg.glitch_ignore_cnt = 7;
      bus_cfg.flags.enable_internal_pullup = true;
      bus_cfg.flags.allow_pd             = false;

      esp_err_t err = i2c_new_master_bus(&bus_cfg, &_bus_handle);
      if (err != ESP_OK || _bus_handle == nullptr) {
        DEBUGSR_PRINTF("audioreactive_I2C: i2c_new_master_bus failed on port %d (sda=%d scl=%d): %d\n",
                       (int)_bus_port, sda_io, scl_io, err);
        _bus_handle = nullptr;
        return false;
      }
      _sda_io = sda_io;
      _scl_io = scl_io;
      DEBUGSR_PRINTF("audioreactive_I2C: installed i2c_master_bus on port %d (sda=%d scl=%d)\n",
                     (int)_bus_port, sda_io, scl_io);
      return true;
    }

    /* Tear down the bus and all attached devices. */
    void end() {
      for (int i = 0; i < _device_count; i++) {
        if (_devices[i]._dev_handle) {
          i2c_master_bus_rm_device(_devices[i]._dev_handle);
          _devices[i]._dev_handle = nullptr;
        }
      }
      _device_count = 0;
      if (_bus_handle) {
        i2c_del_master_bus(_bus_handle);
        _bus_handle = nullptr;
      }
      _sda_io = _scl_io = -1;
    }

    /* Add a device at `addr` with a per-device clock rate. Returns a pointer
       to the device slot (valid until end() or the next addDevice call that
       overflows MAX_DEVICES). Returns nullptr on failure. */
    audioreactive_I2CDevice *addDevice(uint8_t addr, uint32_t freq_hz = SR_I2C_FREQUENCY_HZ) {
      if (!_bus_handle) return nullptr;
      if (_device_count >= MAX_DEVICES) return nullptr;
      i2c_device_config_t dev_cfg = {};
      dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
      dev_cfg.device_address  = addr;
      dev_cfg.scl_speed_hz    = freq_hz;
      dev_cfg.scl_wait_us     = 0;
      dev_cfg.flags.disable_ack_check = false;

      audioreactive_I2CDevice *dev = &_devices[_device_count++];
      esp_err_t err = i2c_master_bus_add_device(_bus_handle, &dev_cfg, &dev->_dev_handle);
      if (err != ESP_OK) {
        DEBUGSR_PRINTF("audioreactive_I2C: i2c_master_bus_add_device failed for addr 0x%02X: %d\n", addr, err);
        _device_count--;
        return nullptr;
      }
      dev->_addr    = addr;
      dev->_freq_hz = freq_hz;
      DEBUGSR_PRINTF("audioreactive_I2C: added device 0x%02X @ %u Hz\n", addr, (unsigned)freq_hz);
      return dev;
    }

    /* Probe an address: returns true if a device ACKed. */
    bool probe(uint8_t addr) {
      if (!_bus_handle) return false;
      uint8_t dummy = 0;
      return i2c_master_probe(_bus_handle, addr, 50) == ESP_OK;
    }

    /* Register write (single byte). Returns true on success. */
    bool writeReg(audioreactive_I2CDevice *dev, uint8_t reg, uint8_t val) {
      if (!dev || !dev->_dev_handle) return false;
      const uint8_t buf[2] = { reg, val };
      return i2c_master_transmit(dev->_dev_handle, buf, sizeof(buf), 50) == ESP_OK;
    }

    /* Register write with two bytes of data (e.g. WM8978/AC101 use 2-byte writes). */
    bool writeReg16(audioreactive_I2CDevice *dev, uint8_t reg, uint16_t val16) {
      if (!dev || !dev->_dev_handle) return false;
      const uint8_t buf[3] = { reg, (uint8_t)(val16 >> 8), (uint8_t)(val16 & 0xFF) };
      return i2c_master_transmit(dev->_dev_handle, buf, sizeof(buf), 50) == ESP_OK;
    }

    /* Register read (single byte). Returns true on success and stores the value in *out. */
    bool readReg(audioreactive_I2CDevice *dev, uint8_t reg, uint8_t *out) {
      if (!dev || !dev->_dev_handle || !out) return false;
      return i2c_master_transmit_receive(dev->_dev_handle, &reg, 1, out, 1, 50) == ESP_OK;
    }

    /* Helper: build a minimal audio_codec_ctrl_if_t shim that wraps a single
       i2c_master_dev_handle_t. Used by the esp_codec_dev path so the
       library's es8311_codec_open / es8311_codec_read / es8311_codec_write
       calls go through our (known-good) device handle instead of whatever
       audio_codec_new_i2c_ctrl() would have set up. On P4 + IDF v5.3 the
       latter produces a device that NACKs every transaction; ours does not. */
    const audio_codec_ctrl_if_t *_ar_make_i2c_ctrl_if(i2c_master_dev_handle_t dev_handle) {
      struct shim {
        audio_codec_ctrl_if_t base;
        i2c_master_dev_handle_t dev_handle;
        mutable bool opened;
      };
      shim *s = (shim *)calloc(1, sizeof(shim));
      if (!s) return nullptr;
      s->dev_handle = dev_handle;
      s->opened = false;
      s->base.open = [](const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size) -> int {
        (void)cfg; (void)cfg_size;
        shim *self = (shim *)ctrl;
        self->opened = true;
        return 0;
      };
      s->base.is_open = [](const audio_codec_ctrl_if_t *ctrl) -> bool {
        const shim *self = (const shim *)ctrl;
        return self->opened;
      };
      s->base.read_reg = [](const audio_codec_ctrl_if_t *ctrl,
                            int reg, int reg_len, void *data, int data_len) -> int {
        const shim *self = (const shim *)ctrl;
        if (reg_len < 1 || reg_len > 2) return -1;
        uint8_t rbuf[2];
        rbuf[0] = (uint8_t)(reg & 0xFF);
        if (reg_len == 2) rbuf[1] = (uint8_t)((reg >> 8) & 0xFF);
        esp_err_t err = i2c_master_transmit_receive(self->dev_handle, rbuf, reg_len,
                                                    (uint8_t *)data, data_len, 200);
        return (err == ESP_OK) ? 0 : -1;
      };
      s->base.write_reg = [](const audio_codec_ctrl_if_t *ctrl,
                             int reg, int reg_len, void *data, int data_len) -> int {
        const shim *self = (const shim *)ctrl;
        if (reg_len < 1 || reg_len > 2) return -1;
        uint8_t buf[4];
        uint8_t *p = buf;
        *p++ = (uint8_t)(reg & 0xFF);
        if (reg_len == 2) *p++ = (uint8_t)((reg >> 8) & 0xFF);
        const uint8_t *src = (const uint8_t *)data;
        for (int i = 0; i < data_len && (p - buf) < (int)sizeof(buf); i++) {
          *p++ = src[i];
        }
        esp_err_t err = i2c_master_transmit(self->dev_handle, buf, (p - buf), 200);
        return (err == ESP_OK) ? 0 : -1;
      };
      s->base.close = [](const audio_codec_ctrl_if_t *ctrl) -> int {
        shim *self = (shim *)ctrl;
        self->opened = false;
        return 0;
      };
      return &s->base;
    }

  private:
    static constexpr int MAX_DEVICES = 8;
    i2c_master_bus_handle_t _bus_handle;
    i2c_port_t _bus_port;
    int8_t _sda_io, _scl_io;
    audioreactive_I2CDevice _devices[MAX_DEVICES];
    int _device_count = 0;
};

// The single shared I2C bus used by the audioreactive usermod (IDF v5 driver).
// Defined here (after the class definition) so all codec classes can reach it.
audioreactive_I2C AR_I2C;

/* The single shared I2C bus used by the audioreactive usermod.
   Owned by AudioReactive::setup(); all codec classes acquire device handles
   via ::i2c.addDevice(addr). */
extern audioreactive_I2C AR_I2C;

/* Basic I2S microphone source
   All functions are marked virtual, so derived classes can replace them
   WARNING: i2sMaster = false is experimental, and most likely will not work
   Uses the IDF v5 new I2S driver (driver/i2s_std.h).
*/
class I2SSource : public AudioSource {
  public:
    I2SSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f, bool i2sMaster=true) :
      AudioSource(sampleRate, blockSize, sampleScale, i2sMaster) {
      // The new I2S driver is configured at initialize() time via i2s_std_config_t.
      // Member defaults (channel slot, sample bit width, etc.) are stored here.
      _isPDM = false;
      _i2sMaster = i2sMaster;
    }

    virtual void initialize(int8_t i2swsPin     = I2S_GPIO_UNUSED,
                            int8_t i2ssdPin     = I2S_GPIO_UNUSED,
                            int8_t i2sckPin     = I2S_GPIO_UNUSED,
                            int8_t mclkPin      = I2S_GPIO_UNUSED,
                            uint8_t bitsPerSample = 32,
                            bool    useRightSlot  = false,
                            bool    i2sMaster     = true) override {
      DEBUGSR_PRINTLN("I2SSource:: initialize().");
      _i2sMaster = i2sMaster;

      // Map runtime bit-width to the new driver's I2S_DATA_BIT_WIDTH_* enum.
      i2s_data_bit_width_t data_width = I2S_DATA_BIT_WIDTH_16BIT;
      switch (bitsPerSample) {
        case 24: data_width = I2S_DATA_BIT_WIDTH_24BIT; break;
        case 32: data_width = I2S_DATA_BIT_WIDTH_32BIT; break;
        case 16: default:   data_width = I2S_DATA_BIT_WIDTH_16BIT; break;
      }
      _slotMask = useRightSlot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;

      if (i2swsPin == I2S_GPIO_UNUSED || i2ssdPin == I2S_GPIO_UNUSED || i2sckPin == I2S_GPIO_UNUSED) {
        USER_PRINTLN("I2SSource:: Pins not configured, skipping initialization.");
        return;
      }

      _wsPin = i2swsPin;
      _dinPin = i2ssdPin;
      _bckPin = i2sckPin;

      if (!pinManager.allocatePin(i2swsPin, true, PinOwner::UM_Audioreactive) ||
        !pinManager.allocatePin(i2ssdPin, false, PinOwner::UM_Audioreactive)) {
        ERRORSR_PRINTF("\nAR: Failed to allocate I2S pins: ws=%d, sd=%d\n", i2swsPin, i2ssdPin);
        return;
      }

      // i2ssckPin needs special treatment, since it might be unused on PDM mics
      bool usePDM = false;
      if (i2sckPin != I2S_GPIO_UNUSED) {
        if (!pinManager.allocatePin(i2sckPin, true, PinOwner::UM_Audioreactive)) {
          ERRORSR_PRINTF("\nAR: Failed to allocate I2S pins: sck=%d\n", i2sckPin);
          return;
        }
      } else {
        #if !defined(SOC_I2S_SUPPORTS_PDM_RX)
          #warning this MCU does not support PDM microphones
        #endif
        #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32P4)
        // This is an I2S PDM microphone, these microphones only use a clock and
        // data line, to make it simpler to debug, use the WS pin as CLK and SD pin as DATA
        // PDM has known bugs on S3, and does not work on C3
        usePDM = true;
        #else
        ERRORSR_PRINTLN(F("AR: PDM microphones not supported on this MCU; bck_pin is required."));
        return;
        #endif
      }

      _mclkPin = mclkPin;
      if (mclkPin != I2S_GPIO_UNUSED) {
        if (!pinManager.allocatePin(mclkPin, true, PinOwner::UM_Audioreactive)) {
          ERRORSR_PRINTF("\nAR: Failed to allocate I2S pin: MCLK=%d\n", mclkPin);
          return;
        }
        _routeMclk(mclkPin);
      }

      // Allocate the RX channel
      i2s_chan_config_t chan_cfg = {
        .id            = AR_I2S_PORT,
        .role          = _i2sMaster ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE,
        .dma_desc_num  = AR_I2S_DMA_DESCS,
        .dma_frame_num = (uint32_t)_blockSize,
        .auto_clear    = false,
      };
      esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &_rx_handle);
      if (err != ESP_OK) {
        ERRORSR_PRINTF("AR: Failed to create new I2S RX channel: %d\n", err);
        return;
      }

      if (usePDM) {
        _isPDM = true;
        i2s_pdm_rx_slot_config_t pdm_slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        pdm_slot_cfg.slot_mask = useRightSlot ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;

        i2s_pdm_rx_clk_config_t pdm_clk_cfg = {
          .sample_rate_hz     = _sampleRate,
          .clk_src            = I2S_CLK_SRC_DEFAULT,
          .mclk_multiple      = I2S_MCLK_MULTIPLE_256,
          .dn_sample_mode     = I2S_PDM_DSR_8S,
        };
        i2s_pdm_rx_gpio_config_t pdm_gpio_cfg = {
          .clk = (gpio_num_t)i2swsPin,
          .din = (gpio_num_t)i2ssdPin,
          .invert_flags = { .clk_inv = false },
        };
        i2s_pdm_rx_config_t pdm_cfg = {
          .clk_cfg  = pdm_clk_cfg,
          .slot_cfg = pdm_slot_cfg,
          .gpio_cfg = pdm_gpio_cfg,
        };
        err = i2s_channel_init_pdm_rx_mode(_rx_handle, &pdm_cfg);
        if (err != ESP_OK) {
          ERRORSR_PRINTF("AR: Failed to init PDM RX mode: %d\n", err);
          i2s_del_channel(_rx_handle);
          _rx_handle = nullptr;
          return;
        }
        DEBUGSR_PRINTLN(F("AR: I2S#0 driver installed in PDM MASTER mode."));
      } else {
        i2s_std_slot_config_t std_slot_cfg = {
          .data_bit_width = data_width,
          .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
          .slot_mode      = I2S_SLOT_MODE_MONO,
          .slot_mask      = _slotMask,
          .ws_width       = bitsPerSample,
          .ws_pol         = false,
          .bit_shift      = true,
          .left_align     = true,
          .big_endian     = false,
          .bit_order_lsb  = false,
        };

        i2s_std_clk_config_t std_clk_cfg = {
          .sample_rate_hz = _sampleRate,
          .clk_src        = I2S_CLK_SRC_DEFAULT,
          .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        };

        i2s_std_config_t std_cfg = {
          .clk_cfg  = std_clk_cfg,
          .slot_cfg = std_slot_cfg,
          .gpio_cfg = {
            .mclk        = (gpio_num_t)mclkPin,
            .bclk        = (gpio_num_t)i2sckPin,
            .ws          = (gpio_num_t)i2swsPin,
            .dout        = I2S_GPIO_UNUSED,
            .din         = (gpio_num_t)i2ssdPin,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
          },
        };

        err = i2s_channel_init_std_mode(_rx_handle, &std_cfg);
        if (err != ESP_OK) {
          ERRORSR_PRINTF("AR: Failed to init STD RX mode: %d\n", err);
          i2s_del_channel(_rx_handle);
          _rx_handle = nullptr;
          return;
        }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
        // P4 workaround: the IDF v5 new I2S driver does not explicitly disable
        // the GPIO output driver on the DIN pin. Without this, the GPIO output
        // register drives the pin (gpio_dump_io_configuration shows
        // OutputEn:1, SigOut:256 on GPIO 45) and masks any external mic
        // signal — every sample reads 0xFFFFFFFF. The legacy i2s_set_pin()
        // cleared this implicitly via gpio_set_direction(pin, GPIO_MODE_INPUT).
        gpio_set_direction((gpio_num_t)i2ssdPin, GPIO_MODE_INPUT);
#endif
        DEBUGSR_PRINTF("AR: I2S#0 driver installed in %s mode, %u-bit.\n",
                       _i2sMaster ? "MASTER" : "SLAVE", bitsPerSample);
      }

      err = i2s_channel_enable(_rx_handle);
      if (err != ESP_OK) {
        ERRORSR_PRINTF("AR: Failed to enable I2S channel: %d\n", err);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
        return;
      }

      _initialized = true;
    }

    virtual void deinitialize() {
      _initialized = false;
      if (_rx_handle) {
        i2s_channel_disable(_rx_handle);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
      }
      if (_wsPin   != I2S_GPIO_UNUSED) pinManager.deallocatePin(_wsPin,   PinOwner::UM_Audioreactive);
      if (_dinPin  != I2S_GPIO_UNUSED) pinManager.deallocatePin(_dinPin,  PinOwner::UM_Audioreactive);
      if (_bckPin  != I2S_GPIO_UNUSED) pinManager.deallocatePin(_bckPin,  PinOwner::UM_Audioreactive);
      // Release the master clock pin
      if (_mclkPin != I2S_GPIO_UNUSED) pinManager.deallocatePin(_mclkPin, PinOwner::UM_Audioreactive);
    }

    virtual void getSamples(float *buffer, uint16_t num_samples) {
      // The destination is defined before anything else happens, so a caller
      // that does not clear its own buffer gets zeros rather than stale
      // samples. The analysis task relies on that: it fills the frame completely
      // from getSamples() and the carry-over memcpy, and does not clear the
      // frame itself (see arAnalysisTask()).
      //
      // It is only cleared where it is needed - on a failed read. Clearing it
      // on every frame would be 2 KB of writes that the loop below
      // immediately overwrites.
      const uint16_t want = num_samples;
      if (_initialized && _rx_handle) {
        size_t bytes_read = 0;

        I2S_datatype *newSamples = newSampleBuffer;
        if (num_samples > I2S_SAMPLES_MAX) num_samples = I2S_SAMPLES_MAX;

        esp_err_t err = i2s_channel_read(_rx_handle, (void *)newSamples, num_samples * sizeof(I2S_datatype), &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
          DEBUGSR_PRINTF("Failed to get samples: %d\n", err);
          memset(buffer, 0, sizeof(float) * want);
          return;
        }

        if (bytes_read != (num_samples * sizeof(I2S_datatype))) {
          DEBUGSR_PRINTF("Failed to get enough samples: wanted: %d read: %d\n", num_samples * sizeof(I2S_datatype), bytes_read);
          memset(buffer, 0, sizeof(float) * want);
          return;
        }

        for (int i = 0; i < num_samples; i++) {
          newSamples[i] = postProcessSample(newSamples[i]);
          float currSample = 0.0f;
#ifdef I2S_SAMPLE_DOWNSCALE_TO_16BIT
              currSample = (float) newSamples[i] / 65536.0f;
#else
              currSample = (float) newSamples[i];
#endif
          buffer[i] = currSample;
          buffer[i] *= _sampleScale;
        }
        // A request longer than the buffer can hold is truncated above, so
        // the tail is cleared here rather than being stale.
        if (want > num_samples) memset(buffer + num_samples, 0, sizeof(float) * (want - num_samples));
      } else {
        memset(buffer, 0, sizeof(float) * want);
      }
    }

  protected:
    void _routeMclk(int8_t mclkPin) {
      // MCLK routing by writing registers is no longer needed with IDF >= 4.4.0.
      // On classic ESP32 the I2S peripheral handles MCLK output automatically
      // when configured via the new driver API.
      (void)mclkPin;
    }

    i2s_chan_handle_t _rx_handle = nullptr;
    bool _isPDM = false;
    int8_t _wsPin  = I2S_GPIO_UNUSED;
    int8_t _dinPin = I2S_GPIO_UNUSED;
    int8_t _bckPin = I2S_GPIO_UNUSED;
    int8_t _mclkPin = I2S_GPIO_UNUSED;
    i2s_std_slot_mask_t _slotMask = I2S_STD_SLOT_LEFT;
};

/* ES7243 Microphone
   This is an I2S microphone that requires initialization over
   I2C before I2S data can be received
*/
class ES7243 : public I2SSource {
  private:
    #ifndef ES7243_ADDR
      #define ES7243_ADDR 0x13   // default address
    #endif

    void _es7243I2cWrite(uint8_t reg, uint8_t val) {
      if (!_codec) return;
      if (!AR_I2C.writeReg(_codec, reg, val)) {
        DEBUGSR_PRINTF("AR: ES7243 I2C write failed (addr=0x%X, reg 0x%X, val 0x%X).\n", ES7243_ADDR, reg, val);
      }
    }

    void _es7243InitAdc() {
      _es7243I2cWrite(0x00, 0x01);
      _es7243I2cWrite(0x06, 0x00);
      _es7243I2cWrite(0x05, 0x1B);
      _es7243I2cWrite(0x01, 0x00); // 0x00 for 24 bit to match INMP441 - not sure if this needs adjustment to get 16bit samples from I2S
      _es7243I2cWrite(0x08, 0x43);
      _es7243I2cWrite(0x05, 0x13);
    }

    audioreactive_I2CDevice *_codec = nullptr;

public:
    ES7243(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f, bool i2sMaster=true) :
      I2SSource(sampleRate, blockSize, sampleScale, i2sMaster) {
      // ES7243 historically used the RIGHT slot on legacy drivers; keep that as the
      // compile-time default but allow runtime override via the usermod.
      _slotMask = I2S_STD_SLOT_RIGHT;
    };

    void initialize(int8_t i2swsPin, int8_t i2ssdPin, int8_t i2sckPin, int8_t mclkPin,
                    uint8_t bitsPerSample = 32, bool useRightSlot = true, bool i2sMaster = true) {
      DEBUGSR_PRINTLN("ES7243:: initialize();");

      if ((i2c_sda < 0) || (i2c_scl < 0)) {
        ERRORSR_PRINTF("\nAR: invalid ES7243 global I2C pins: SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      if (!AR_I2C.isReady() && !AR_I2C.begin(i2c_sda, i2c_scl)) {
        ERRORSR_PRINTF("\nAR: failed to install audioreactive I2C bus with SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      _codec = AR_I2C.addDevice(ES7243_ADDR);
      if (!_codec) {
        ERRORSR_PRINTLN("AR: failed to add ES7243 to I2C bus");
        return;
      }

      _es7243InitAdc();
      I2SSource::initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin, bitsPerSample, useRightSlot, i2sMaster);
    }

    void deinitialize() {
      I2SSource::deinitialize();
      _codec = nullptr;  // AR_I2C owns the device handle; it's torn down by AudioReactive::setup on next init or deinit
    }
};

/* ES8388 Sound Module
   This is an I2S sound processing unit that requires initialization over
   I2C before I2S data can be received. 
*/
class ES8388Source : public I2SSource {
  private:
    // I2C initialization functions for ES8388
    void _es8388I2cWrite(uint8_t reg, uint8_t val) {
      #ifndef ES8388_ADDR
        #define ES8388_ADDR 0x10   // default address
      #endif
      if (!_codec) return;
      if (!AR_I2C.writeReg(_codec, reg, val)) {
        DEBUGSR_PRINTF("AR: ES8388 I2C write failed (addr=0x%X, reg 0x%X, val 0x%X).\n", ES8388_ADDR, reg, val);
      }
    }

    void _es8388InitAdc() {
      // https://dl.radxa.com/rock2/docs/hw/ds/ES8388%20user%20Guide.pdf Section 10.1
      // http://www.everest-semi.com/pdf/ES8388%20DS.pdf Better spec sheet, more clear.
      // https://docs.google.com/spreadsheets/d/1CN3MvhkcPVESuxKyx1xRYqfUit5hOdsG45St9BCUm-g/edit#gid=0 generally
      // Sets ADC to around what AudioReactive expects, and loops line-in to line-out/headphone for monitoring.
      // Registries are decimal, settings are binary as that's how everything is listed in the docs
      // ...which makes it easier to reference the docs.
      //
      _es8388I2cWrite( 8,0b00000000); // I2S to slave
      _es8388I2cWrite( 2,0b11110011); // Power down DEM and STM
      _es8388I2cWrite(43,0b10000000); // Set same LRCK
      _es8388I2cWrite( 0,0b00000101); // Set chip to Play & Record Mode
      _es8388I2cWrite(13,0b00000010); // Set MCLK/LRCK ratio to 256
      _es8388I2cWrite( 1,0b01000000); // Power up analog and lbias
      _es8388I2cWrite( 3,0b00000000); // Power up ADC, Analog Input, and Mic Bias
      _es8388I2cWrite( 4,0b11111100); // Power down DAC, Turn on LOUT1 and ROUT1 and LOUT2 and ROUT2 power
      _es8388I2cWrite( 2,0b01000000); // Power up DEM and STM and undocumented bit for "turn on line-out amp"

      // #define use_es8388_mic

    #ifdef use_es8388_mic
      // The mics *and* line-in are BOTH connected to LIN2/RIN2 on the AudioKit
      // so there's no way to completely eliminate the mics. It's also hella noisy. 
      // Line-in works OK on the AudioKit, generally speaking, as the mics really need
      // amplification to be noticeable in a quiet room. If you're in a very loud room, 
      // the mics on the AudioKit WILL pick up sound even in line-in mode. 
      // TL;DR: Don't use the AudioKit for anything, use the LyraT. 
      //
      // The LyraT does a reasonable job with mic input as configured below.

      // Pick one of these. If you have to use the mics, use a LyraT over an AudioKit if you can:
      _es8388I2cWrite(10,0b00000000); // Use Lin1/Rin1 for ADC input (mic on LyraT)
      //_es8388I2cWrite(10,0b01010000); // Use Lin2/Rin2 for ADC input (mic *and* line-in on AudioKit)
      
      _es8388I2cWrite( 9,0b10001000); // Select Analog Input PGA Gain for ADC to +24dB (L+R)
      _es8388I2cWrite(16,0b00000000); // Set ADC digital volume attenuation to 0dB (left)
      _es8388I2cWrite(17,0b00000000); // Set ADC digital volume attenuation to 0dB (right)
      _es8388I2cWrite(38,0b00011011); // Mixer - route LIN1/RIN1 to output after mic gain

      _es8388I2cWrite(39,0b01000000); // Mixer - route LIN to mixL, +6dB gain
      _es8388I2cWrite(42,0b01000000); // Mixer - route RIN to mixR, +6dB gain
      _es8388I2cWrite(46,0b00100001); // LOUT1VOL - 0b00100001 = +4.5dB
      _es8388I2cWrite(47,0b00100001); // ROUT1VOL - 0b00100001 = +4.5dB
      _es8388I2cWrite(48,0b00100001); // LOUT2VOL - 0b00100001 = +4.5dB
      _es8388I2cWrite(49,0b00100001); // ROUT2VOL - 0b00100001 = +4.5dB

      // Music ALC - the mics like Auto Level Control
      // You can also use this for line-in, but it's not really needed.
      //
      _es8388I2cWrite(18,0b11111000); // ALC: stereo, max gain +35.5dB, min gain -12dB 
      _es8388I2cWrite(19,0b00110000); // ALC: target -1.5dB, 0ms hold time
      _es8388I2cWrite(20,0b10100110); // ALC: gain ramp up = 420ms/93ms, gain ramp down = check manual for calc
      _es8388I2cWrite(21,0b00000110); // ALC: use "ALC" mode, no zero-cross, window 96 samples
      _es8388I2cWrite(22,0b01011001); // ALC: noise gate threshold, PGA gain constant, noise gate enabled 
    #else
      _es8388I2cWrite(10,0b01010000); // Use Lin2/Rin2 for ADC input ("line-in")
      _es8388I2cWrite( 9,0b00000000); // Select Analog Input PGA Gain for ADC to 0dB (L+R)
      _es8388I2cWrite(16,0b01000000); // Set ADC digital volume attenuation to -32dB (left)
      _es8388I2cWrite(17,0b01000000); // Set ADC digital volume attenuation to -32dB (right)
      _es8388I2cWrite(38,0b00001001); // Mixer - route LIN2/RIN2 to output

      _es8388I2cWrite(39,0b01010000); // Mixer - route LIN to mixL, 0dB gain
      _es8388I2cWrite(42,0b01010000); // Mixer - route RIN to mixR, 0dB gain
      _es8388I2cWrite(46,0b00011011); // LOUT1VOL - 0b00011110 = +0dB, 0b00011011 = LyraT balance fix
      _es8388I2cWrite(47,0b00011110); // ROUT1VOL - 0b00011110 = +0dB
      _es8388I2cWrite(48,0b00011110); // LOUT2VOL - 0b00011110 = +0dB
      _es8388I2cWrite(49,0b00011110); // ROUT2VOL - 0b00011110 = +0dB
    #endif

    }

  public:
    ES8388Source(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f, bool i2sMaster=true) :
      I2SSource(sampleRate, blockSize, sampleScale, i2sMaster) {
      _slotMask = I2S_STD_SLOT_LEFT;
    };

    void initialize(int8_t i2swsPin, int8_t i2ssdPin, int8_t i2sckPin, int8_t mclkPin,
                    uint8_t bitsPerSample = 32, bool useRightSlot = false, bool i2sMaster = true) {
      DEBUGSR_PRINTLN("ES8388Source:: initialize();");

      if ((i2c_sda < 0) || (i2c_scl < 0)) {
        ERRORSR_PRINTF("\nAR: invalid ES8388 global I2C pins: SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      if (!AR_I2C.isReady() && !AR_I2C.begin(i2c_sda, i2c_scl)) {
        ERRORSR_PRINTF("\nAR: failed to install audioreactive I2C bus with SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      _codec = AR_I2C.addDevice(ES8388_ADDR);
      if (!_codec) {
        ERRORSR_PRINTLN("AR: failed to add ES8388 to I2C bus");
        return;
      }

      // First route mclk, then configure ADC over I2C, then configure I2S
      _es8388InitAdc();
      I2SSource::initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin, bitsPerSample, useRightSlot, i2sMaster);
    }

    void deinitialize() {
      I2SSource::deinitialize();
      _codec = nullptr;
    }

    audioreactive_I2CDevice *_codec = nullptr;

};

/* ES8311 Sound Module
   This is an I2S sound processing unit that requires initialization over
   I2C before I2S data can be received. 
*/
class ES8311Source : public I2SSource {
  private:
    // Returns the active ADC device — the ES8311 normally, or the ES7210 if one
    // is present on the bus (the ES8311 is then held in reset).
    // (dead - removed; not referenced anywhere)
    // audioreactive_I2CDevice *_activeAdc() { ... }

    void _i2cWrite(audioreactive_I2CDevice *dev, uint8_t reg, uint8_t val) {
      if (!dev) return;
      if (!AR_I2C.writeReg(dev, reg, val)) {
        DEBUGSR_PRINTF("AR: ES8311 I2C write failed (addr=0x%X, reg 0x%X, val 0x%X).\n", dev->addr(), reg, val);
      }
    }

    void _es7210_init_22k_32bit(audioreactive_I2CDevice *dev) {
      // --- 1. RESET ---
      _i2cWrite(dev, 0x00, 0xFF);
      vTaskDelay(pdMS_TO_TICKS(10));
      _i2cWrite(dev, 0x00, 0x32);

      // --- 2. SLAVE MODE (clocks from ESP32) ---
      _i2cWrite(dev, 0x08, 0x00);  // Slave mode
      // _i2cWrite(dev, 0x06, 0x04);  // DLL off (not needed in slave mode)

      // --- 3. I2S FORMAT ---
      _i2cWrite(dev, 0x09, 0x30);  // Timing control
      _i2cWrite(dev, 0x0A, 0x30);  // Timing control
      _i2cWrite(dev, 0x11, 0x80);  // 32-bit I2S
      _i2cWrite(dev, 0x12, 0x00);  // MIC1/2 on SDOUT1

      // --- 4. HIGH PASS FILTER ---
      _i2cWrite(dev, 0x22, 0x0A);
      _i2cWrite(dev, 0x23, 0x2A);

      // --- 5. ANALOG POWER ---
      _i2cWrite(dev, 0x40, 0xC3);
      _i2cWrite(dev, 0x41, 0x70);  // 0x70 standard bias (0x7F is max)

      // --- 6. GAIN (no ALC) ---
      _i2cWrite(dev, 0x43, 0x18);
      _i2cWrite(dev, 0x44, 0x18);
      _i2cWrite(dev, 0x16, 0x00);  // ALC off

      // --- 7. MIC POWER ---
      _i2cWrite(dev, 0x47, 0x08);  // MIC1 power
      _i2cWrite(dev, 0x48, 0x08);  // MIC2 power
      _i2cWrite(dev, 0x49, 0x00);  // MIC3 OFF
      _i2cWrite(dev, 0x4A, 0x00);  // MIC4 OFF
      _i2cWrite(dev, 0x4B, 0x0F);  // ADC1/2 power
      _i2cWrite(dev, 0x4C, 0x00);  // ADC3/4 OFF

      // --- 8. START ---
      _i2cWrite(dev, 0x00, 0x71);
      _i2cWrite(dev, 0x00, 0x41);
    }

    // Write the ES8311 clock manager (0x01-0x08).
    //
    // The bit layout below is not guesswork: three independent vendor drivers
    // agree on it - Espressif's own "espressif/es8311" component, ESPhome's
    // es8311.cpp and the Linux ALSA sound/soc/codecs/es8311.c driver - and all
    // three key their coefficient table on (MCLK, rate). For MCLK = 256 x rate
    // they all produce the same register values:
    //
    //   01 = 0x3F  all clocks on, MCLK taken from the MCLK pin
    //   02 = 0x00  DIV_PRE = 1 (bits 7:5 hold DIV_PRE-1), MULT_PRE = 1x
    //   03 = 0x10  single speed, ADC OSR 128
    //   04 = 0x10  DAC OSR 128
    //   05 = 0x00  ADC/DAC clock divider = 1
    //   06 = 0x03  BCLK divider 4, no invert
    //   07 = 0x00  LRCK divider high byte
    //   08 = 0xFF  LRCK divider low byte  -> 256
    //
    // The values are the same for 22050 Hz and 48000 Hz; only the MCLK
    // frequency differs (5.6448 MHz vs 12.288 MHz), because MCLK = 256 x rate
    // is what the I2S side of this rig drives.
    //
    // divPre is the MCLK pre-divider (1..8). It is a parameter rather than a
    // constant so it can be A/B'd at runtime - see setCodecClockDiv().
    void _es8311WriteClock(audioreactive_I2CDevice *dev, uint8_t divPre) {
      if (!dev) return;
      if (divPre < 1) divPre = 1;
      if (divPre > 8) divPre = 8;
      _i2cWrite(dev, 0x01, 0x3F);                  // all clocks on, MCLK from the MCLK pin
      uint8_t r02 = 0;
      if (!AR_I2C.readReg(dev, 0x02, &r02)) r02 = 0;
      r02 &= 0x07;                                // bits 2:0 are not ours to change
      r02 |= (uint8_t)((divPre - 1) << 5);        // bits 7:5 = DIV_PRE - 1
      r02 |= (uint8_t)(0 << 3);                   // bits 4:3 = MULT_PRE (0 = 1x)
      _i2cWrite(dev, 0x02, r02);
      _i2cWrite(dev, 0x03, 0x10);                 // single speed, ADC oversampling bits
      _i2cWrite(dev, 0x04, 0x10);                 // DAC oversampling bits
      _i2cWrite(dev, 0x05, 0x00);                 // ADC/DAC clock divider = 1
      _i2cWrite(dev, 0x06, 0x03);                 // BCLK divider 4, no invert
      _i2cWrite(dev, 0x07, 0x00);                 // LRCK divider high byte
      _i2cWrite(dev, 0x08, 0xFF);                 // LRCK divider low byte -> 256
    }

    void _es8311InitAdc(audioreactive_I2CDevice *dev) {
      //
      // Values synced to ESPhome's es8311.cpp (the well-tested upstream
      // driver). The previous values were hand-tuned and may have had
      // suboptimal or wrong bits set. See:
      // https://github.com/esphome/esphome/blob/.../esphome/components/es8311/es8311.cpp
      //
      _i2cWrite(dev, 0x00, 0x1F);                  // Reset
      _i2cWrite(dev, 0x00, 0x00);                  // Clear reset

      // configure_clock_() equivalent — ESPhome uses a coefficient table
      // driven by sample rate + MCLK multiple. The table row for MCLK = 256 x
      // rate is the same at every rate we use, so the only variable is the
      // MCLK pre-divider, which is a member so it can be changed at runtime.
      _es8311WriteClock(dev, _codecClkDiv);

      // configure_format_() equivalent — set SDP in/out resolution
      _i2cWrite(dev, 0x09, 0x10);                  // SDP IN  = 32-bit (4<<2)
      _i2cWrite(dev, 0x0A, 0x10);                  // SDP OUT = 32-bit (4<<2)
      _i2cWrite(dev, 0x07, 0x00);                  // LRCK divider hi
      _i2cWrite(dev, 0x08, 0xFF);                  // LRCK divider lo

      // ---------------------------------------------------------------------------
// INPUT GAIN: line level vs microphone
// ---------------------------------------------------------------------------
// PGAGAIN was set to +30 dB here, which is correct for a microphone and badly
// wrong for line level. Line input is roughly 30 dB hotter than a mic, so the
// two together put the ADC into overload - and the symptom did not look like
// overload, which is why this went unnoticed for so long:
//
//   * Bands 14 and 15 read far too hot whenever there was no high-frequency
//     content, and read correctly with full-range music.
//   * Nothing upstream of the ADC could change it. Filtering hard on the high
//     side, inserting an external DSP, or removing the DSP entirely all made no
//     difference whatsoever, because the excess gain is inside the analog stage
//     and therefore applied after any line-side filtering.
//
// The two effects compound. fftBandEnergy() sums bin powers rather than
// averaging them, which is right for a tone (a tone's energy lands in one bin
// whatever the band width) but means noise scales as sqrt(bins). Band widths
// grow with centre frequency at constant Q, so band 15 sums 309 bins against
// band 0's 5: a 17.9 dB ramp. Against a 26 dB display span that ramp is 69% of
// the display, so the top two bars read hot on noise alone while the low bands -
// only 5-25 bins - looked correct. The overload set the level; the ramp decided
// where it showed. (The span is 48 dB now, and shared by both views.)
//
// The AGC had been reporting this the whole time. On the info page, "AGC Gain"
// read 0.03x, i.e. -30.5 dB: the servo saturating as it tried to pull down a
// signal the codec had amplified by +30 dB (REG14 = 0x1A -> PGAGAIN = 0b1010).
// The two figures agree to within 0.5 dB. That number was a correct reading of
// a 30 dB overload, not a mysterious gain value.
//
// 0x10 is the ES8311 factory default for this register (0 dB). Total input-path
// gain is then +4.5 dB (REG17 ADC_VOLUME), and "AGC Gain" should read near 1.0x
// instead of 0.03x - which is the check that this diagnosis is right. If that
// number does not move, the reasoning above is wrong.
//
// NOTE: this is hardcoded, but it is a genuinely per-installation setting.
// The right value depends on what is plugged into the mic/TRRS input - a
// microphone wants ~+30 dB, a line-level source wants 0 dB, and consumer line
// outputs vary by more than 10 dB. It wants to become a web-GUI setting
// alongside the existing i2sBitsPerSample / audio source options, writing the
// same register with the gain encoded in bits 3:0 (0=0dB, 1=3dB, 2=6dB ...
// 10=30dB). Until then, changing this line is how you set it.
//
// configure_mic_() equivalent — enable analog MIC + PGA gain
      //
      // GAIN: 0 dB, not the +30 dB this used to set. See the long note below.
      // _i2cWrite(dev, 0x14, 0x1A);              // ORIGINAL: PGAGAIN = 0b1010 = +30 dB
      _i2cWrite(dev, 0x14, 0x10);                  // REG14 = 0x10: LINSEL=1 (Mic1p-Mic1n), PGAGAIN=0b0000 = 0 dB
      _i2cWrite(dev, 0x16, 0x00);                  // REG16 = ADC mixer/scale (0 = no scale)
      _i2cWrite(dev, 0x17, 0xC8);                  // REG17 = 0xC8 (ADC_VOLUME +4.5 dB)

      // ALC and automute: OFF, explicitly.
      //
      // REG18 bit7 = ALC_ENABLE, bit6 = AUTOMUTE_ENABLE (Linux driver's
      // ES8311_ADC4_ALC_EN_SHIFT / ES8311_ADC4_AUTOMUTE_EN_SHIFT; Espressif's
      // own es8311 component names 0x18 "alc enable and winsize" the same way).
      //
      // Nothing here ever wrote these registers - not this driver, not
      // ESPhome's, not Espressif's - so the chip's power-on defaults were in
      // force. That is not a safe default to rely on: an active ALC ducks the
      // gain when the input is hot, and a ducked gain is a gain reduction
      // across the WHOLE spectrum, which looks exactly like "the level drifts
      // down over seconds and then creeps back up".
      //
      // NOTE: the earlier diagnostic in this file decoded ALC_ENABLE out of
      // REG1A bit7. That is the AUTOMUTE register, not the ALC enable - the
      // two are in different registers, and reading the wrong one makes a
      // disabled ALC look enabled and vice versa. REG18 is the one.
      _i2cWrite(dev, 0x18, 0x00);                  // REG18 = 0x00: ALC off, automute off

      // Power-up sequence (matches ESPhome)
      _i2cWrite(dev, 0x0D, 0x01);                  // analog power-up
      _i2cWrite(dev, 0x0E, 0x02);                  // enable analog PGA + ADC modulator
      _i2cWrite(dev, 0x12, 0x00);                  // power up DAC
      _i2cWrite(dev, 0x13, 0x10);                  // enable output to HP drive
      _i2cWrite(dev, 0x1C, 0x6A);                  // ADC EQ bypass + DC offset cancel
      _i2cWrite(dev, 0x37, 0x08);                  // DAC EQ bypass
      _i2cWrite(dev, 0x00, 0x80);                  // start ADC + DAC

      // TEMPORARY DIAGNOSTIC: read the codec's registers back.
      //
      // The ALC registers (0x18-0x1B) are never written here - and ESPhome's
      // driver does not write them either - so the chip's power-on defaults
      // are in force. That matters because the ALC is the classic reason a
      // line-level input behaves like a microphone: it ducks the gain when the
      // input is hot, and a ducked gain is a gain reduction across the WHOLE
      // spectrum - but it shows up as a level that drifts down over seconds
      // and then creeps back up, which is what the sweep capture looked like.
      //
      // Read the chip rather than guess at a datasheet: the register maps
      // disagree between sources on exactly these registers.
      {
        USER_PRINTLN("ES8311 regs:");
        _es8311ReadRegs(dev);
        // The ALC registers (0x18-0x1B) are the ones NO driver writes - not
        // this one, not ESPhome's, not Espressif's - so their values come from
        // the chip's reset state. They are now written explicitly above (ALC
        // off), and this reads them back to prove the write landed.
        //
        // The ALC enable bit is bit 7 of REG18, NOT bit 7 of REG1A: REG18 is
        // "alc enable and winsize", REG1A is "automute winsize / noise gate".
        // The earlier version of this diagnostic read the wrong register, so
        // its "ALC is off" conclusion was not about the ALC at all.
        uint8_t alcCtrl = 0;
        // NOTE: USER_PRINTF expands to a braced block, so the semicolon after
        // the call is a stray null statement - which is a syntax error between
        // an if and its else. The braces below are load-bearing.
        if (AR_I2C.readReg(dev, 0x18, &alcCtrl)) {
          USER_PRINTF("ES8311 ALC: 18=%02X  ALC_ENABLE(bit7)=%d  AUTOMUTE(bit6)=%d\n",
                      alcCtrl, (alcCtrl & 0x80) ? 1 : 0, (alcCtrl & 0x40) ? 1 : 0);
        } else {
          USER_PRINTLN("ES8311 ALC: 18 unreadable");
        }
      }
    }

    // Read the codec's configuration registers back into arCodecRegs(), so the
    // info page shows what the chip actually holds rather than what this code
    // believes it wrote. Called at init and again whenever the clock manager
    // is rewritten at runtime.
    void _es8311ReadRegs(audioreactive_I2CDevice *dev) {
      if (!dev) return;
      AR_CodecRegs& regs = arCodecRegs();
      regs.valid = true;
      for (uint8_t r = 0x00; r <= 0x1F; r++) {
        uint8_t v = 0;
        if (!AR_I2C.readReg(dev, r, &v)) { USER_PRINTF(" %02X=??", r); regs.v[r] = 0; regs.valid = false; }
        else { USER_PRINTF(" %02X=%02X", r, v); regs.v[r] = v; }
        if ((r & 0x07) == 0x07) USER_PRINTLN("");
      }
    }

    void _es8311_holdReset(audioreactive_I2CDevice *dev) {
      if (!dev) return;
      // Hold in reset, power down analog + digital
      _i2cWrite(dev, 0x00, 0x1F);  // Hold in reset
      _i2cWrite(dev, 0x0D, 0x00);  // Power down analog
      _i2cWrite(dev, 0x0C, 0x00);  // Power down digital
    }

    audioreactive_I2CDevice *_es8311 = nullptr;
    audioreactive_I2CDevice *_es7210 = nullptr;

    // The codec's MCLK pre-divider (1..8). The vendor coefficient tables all
    // use 1 for MCLK = 256 x rate. Kept as a member so it can be changed at
    // runtime (POST /json/state {"mclk_div": n}) and A/B'd without a reflash.
    // The state object publishes it under the same name it reads it by, so
    // the setting can be read back as well as written.
    uint8_t _codecClkDiv = 1;

public:
  ES8311Source(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f, bool i2sMaster = true) :
    I2SSource(sampleRate, blockSize, sampleScale, i2sMaster) {
    _slotMask = I2S_STD_SLOT_LEFT;
  };

  void initialize(int8_t i2swsPin, int8_t i2ssdPin, int8_t i2sckPin, int8_t mclkPin,
                  uint8_t bitsPerSample = 32, bool useRightSlot = false, bool i2sMaster = true) {
    DEBUGSR_PRINTLN("es8311Source:: initialize();");

    if ((i2c_sda < 0) || (i2c_scl < 0)) {
      ERRORSR_PRINTF("\nAR: invalid es8311 global I2C pins: SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
      return;
    }
    if (!AR_I2C.isReady() && !AR_I2C.begin(i2c_sda, i2c_scl)) {
      ERRORSR_PRINTF("\nAR: failed to install audioreactive I2C bus with SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
      return;
    }
    _es8311 = AR_I2C.addDevice(0x18);   // ES8311
    if (!_es8311) {
      ERRORSR_PRINTLN("AR: failed to add ES8311 to I2C bus");
      return;
    }
    // Detect optional ES7210 ADC on the same bus
    _es7210 = AR_I2C.addDevice(0x40);
    bool es7210_present = _es7210 && _es7210_present_probe();

    if (es7210_present) {
      USER_PRINTLN("Overriding ES8311 because an ES7210 is present on the I2C bus.");
      _es8311_holdReset(_es8311);
      _es7210_init_22k_32bit(_es7210);
    } else {
      _es8311InitAdc(_es8311);
    }
    I2SSource::initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin, bitsPerSample, useRightSlot, i2sMaster);
  }

  void deinitialize() {
    I2SSource::deinitialize();
    _es8311 = nullptr;
    _es7210 = nullptr;
  }

  // Change the codec's MCLK pre-divider live. This rewrites the clock manager
  // registers (0x01-0x08) without touching anything else, so the effect on
  // the analysis can be observed immediately.
  void setCodecClockDiv(uint8_t div) override {
    if (div < 1) div = 1;
    if (div > 8) div = 8;
    _codecClkDiv = div;
    if (_es8311) {
      _es8311WriteClock(_es8311, div);
      _es8311ReadRegs(_es8311);   // so the info page shows the new values
    }
  }

  uint8_t getCodecClockDiv(void) override { return _codecClkDiv; }

  // The MCLK the ESP is actually generating, read out of the I2S clock
  // registers. On the ESP32-P4 this is a measurement; elsewhere it is 0
  // ("cannot tell"), because the driver does not expose the clock it set.
  uint32_t getMclkHz(void) override {
    #if defined(CONFIG_IDF_TARGET_ESP32P4)
      uint32_t mclk = 0, fs = 0;
      if (arReadP4I2SClock(mclk, fs)) return mclk;
    #endif
    return 0;
  }

  uint32_t getClockSampleRateHz(void) override {
    #if defined(CONFIG_IDF_TARGET_ESP32P4)
      uint32_t mclk = 0, fs = 0;
      if (arReadP4I2SClock(mclk, fs)) return fs;
    #endif
    return 0;
  }

private:
  bool _es7210_present_probe() {
    // AR_I2C.probe() returns true on ACK. The ES7210 device at 0x40 may or may
    // not be on this board — probe first, then return whether we should use it.
    return AR_I2C.probe(_es7210->addr());
  }

};

class WM8978Source : public I2SSource {
  private:
    void _wm8978I2cWrite(uint8_t reg, uint16_t val) {
      #ifndef WM8978_ADDR
        #define WM8978_ADDR 0x1A
      #endif
      if (!_codec) return;
      // WM8978 uses a 2-byte write where the top bit of byte[0] holds bit 8 of val.
      uint8_t buf[2];
      buf[0] = (uint8_t)((reg << 1) | ((val >> 8) & 0x01));
      buf[1] = (uint8_t)(val & 0xFF);
      if (i2c_master_transmit(_codec->handle(), buf, sizeof(buf), 50) != ESP_OK) {
        DEBUGSR_PRINTF("AR: WM8978 I2C write failed (addr=0x%X, reg 0x%X, val 0x%X).\n", _codec->addr(), reg, val);
      }
    }

    void _wm8978InitAdc() {
      // https://www.mouser.com/datasheet/2/76/WM8978_v4.5-1141768.pdf
      // Sets ADC to around what AudioReactive expects, and loops line-in to line-out/headphone for monitoring.
      // Registries are decimal, settings are 9-bit binary as that's how everything is listed in the docs
      // ...which makes it easier to reference the docs.
      //
      _wm8978I2cWrite( 0,0b000000000); // Reset all settings
      _wm8978I2cWrite( 1,0b000111110); // Power Management 1 - power off most things, but enable mic bias and I/O tie-off to help mitigate mic leakage.
      _wm8978I2cWrite( 2,0b110111111); // Power Management 2 - enable output and amp stages (amps may lift signal but it works better on the ADCs)
      _wm8978I2cWrite( 3,0b000001100); // Power Management 3 - enable L&R output mixers

      #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
      _wm8978I2cWrite( 4,0b001010000); // Audio Interface - standard I2S, 24-bit
      #else
      _wm8978I2cWrite( 4,0b001001000); // Audio Interface - left-justified I2S, 24-bit
      #endif

      _wm8978I2cWrite( 6,0b000000000); // Clock generation control - use external mclk
      _wm8978I2cWrite( 7,0b000000100); // Sets sample rate to ~24kHz (only used for internal calculations, not I2S)
      _wm8978I2cWrite(14,0b010001000); // 128x ADC oversampling - high pass filter disabled as it kills the bass response
      _wm8978I2cWrite(43,0b000110000); // Mute signal paths we don't use
      _wm8978I2cWrite(44,0b100000000); // Disconnect microphones
      _wm8978I2cWrite(45,0b111000000); // Mute signal paths we don't use
      _wm8978I2cWrite(46,0b111000000); // Mute signal paths we don't use
      _wm8978I2cWrite(47,0b001000000); // 0dB gain on left line-in
      _wm8978I2cWrite(48,0b001000000); // 0dB gain on right line-in
      _wm8978I2cWrite(49,0b000000011); // Mixer thermal shutdown enable and unused IOs to 30kΩ
      _wm8978I2cWrite(50,0b000010110); // Output mixer enable only left bypass at 0dB gain
      _wm8978I2cWrite(51,0b000010110); // Output mixer enable only right bypass at 0dB gain
      _wm8978I2cWrite(52,0b110111001); // Left line-out enabled at 0dB gain
      _wm8978I2cWrite(53,0b110111001); // Right line-out enabled at 0db gain
      _wm8978I2cWrite(54,0b111000000); // Mute left speaker output
      _wm8978I2cWrite(55,0b111000000); // Mute right speaker output

    }

    audioreactive_I2CDevice *_codec = nullptr;

  public:
    WM8978Source(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f, bool i2sMaster=true) :
      I2SSource(sampleRate, blockSize, sampleScale, i2sMaster) {
      _slotMask = I2S_STD_SLOT_LEFT;
    };

    void initialize(int8_t i2swsPin, int8_t i2ssdPin, int8_t i2sckPin, int8_t mclkPin,
                    uint8_t bitsPerSample = 32, bool useRightSlot = false, bool i2sMaster = true) {
      DEBUGSR_PRINTLN("WM8978Source:: initialize();");

      if ((i2c_sda < 0) || (i2c_scl < 0)) {
        ERRORSR_PRINTF("\nAR: invalid WM8978 global I2C pins: SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      if (!AR_I2C.isReady() && !AR_I2C.begin(i2c_sda, i2c_scl)) {
        ERRORSR_PRINTF("\nAR: failed to install audioreactive I2C bus with SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      _codec = AR_I2C.addDevice(WM8978_ADDR, 400000);
      if (!_codec) {
        ERRORSR_PRINTLN("AR: failed to add WM8978 to I2C bus");
        return;
      }

      // First route mclk, then configure ADC over I2C, then configure I2S
      _wm8978InitAdc();
      I2SSource::initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin, bitsPerSample, useRightSlot, i2sMaster);
    }

    void deinitialize() {
      I2SSource::deinitialize();
      _codec = nullptr;
    }

};

class AC101Source : public I2SSource {
  private:
    void _ac101I2cWrite(uint8_t reg_addr, uint16_t val) {
      #ifndef AC101_ADDR
        #define AC101_ADDR 0x1A
      #endif
      if (!_codec) return;
      uint8_t buf[3];
      buf[0] = reg_addr;
      buf[1] = (uint8_t)((val >> 8) & 0xff);
      buf[2] = (uint8_t)(val & 0xff);
      if (i2c_master_transmit(_codec->handle(), buf, sizeof(buf), 50) != ESP_OK) {
        DEBUGSR_PRINTF("AR: AC101 I2C write failed (addr=0x%X, reg 0x%X, val 0x%X).\n", _codec->addr(), reg_addr, val);
      }
    }

    void _ac101InitAdc() {
      // https://files.seeedstudio.com/wiki/ReSpeaker_6-Mics_Circular_Array_kit_for_Raspberry_Pi/reg/AC101_User_Manual_v1.1.pdf
      // This supports mostly the older AI Thinkier AudioKit A1S that has an AC101 chip
      // Newer versions use the ES3833 chip - which we also support.

      #define CHIP_AUDIO_RS     0x00
      #define SYSCLK_CTRL       0x03
      #define MOD_CLK_ENA       0x04
      #define MOD_RST_CTRL      0x05
      #define I2S_SR_CTRL       0x06
      #define I2S1LCK_CTRL      0x10
      #define I2S1_SDOUT_CTRL   0x11
      #define I2S1_MXR_SRC      0x13
      #define ADC_DIG_CTRL      0x40
      #define ADC_APC_CTRL      0x50
      #define ADC_SRC           0x51
      #define ADC_SRCBST_CTRL   0x52
      #define OMIXER_DACA_CTRL  0x53
      #define OMIXER_SR         0x54
      #define HPOUT_CTRL        0x56

      _ac101I2cWrite(CHIP_AUDIO_RS, 0x123); // I think anything written here is a reset as 0x123 is kinda suss.

      delay(100);

      _ac101I2cWrite(SYSCLK_CTRL,       0b0000100000001000); // System Clock is I2S MCLK
      _ac101I2cWrite(MOD_CLK_ENA,       0b1000000000001000); // I2S and ADC Clock Enable
      _ac101I2cWrite(MOD_RST_CTRL,      0b1000000000001000); // I2S and ADC Clock Enable
      _ac101I2cWrite(I2S_SR_CTRL,       0b0100000000000000); // set to 22050hz just in case
      _ac101I2cWrite(I2S1LCK_CTRL,      0b1000000000110000); // set I2S slave mode, 24-bit word size
      _ac101I2cWrite(I2S1_SDOUT_CTRL,   0b1100000000000000); // I2S enable Left/Right channels
      _ac101I2cWrite(I2S1_MXR_SRC,      0b0010001000000000); // I2S digital Mixer, ADC L/R data
      _ac101I2cWrite(ADC_SRCBST_CTRL,   0b0000000000000100); // mute all boosts. last 3 bits are reserved/default
      _ac101I2cWrite(OMIXER_SR,         0b0000010000001000); // Line L/R to output mixer
      _ac101I2cWrite(ADC_SRC,           0b0000010000001000); // Line L/R to ADC
      _ac101I2cWrite(ADC_DIG_CTRL,      0b1000000000000000); // Enable ADC
      _ac101I2cWrite(ADC_APC_CTRL,      0b1011100100000000); // ADC L/R enabled, 0dB gain
      _ac101I2cWrite(OMIXER_DACA_CTRL,  0b0011111110000000); // L/R Analog Output Mixer enabled, headphone DC offset default
      _ac101I2cWrite(HPOUT_CTRL,        0b1111101111110001); // Headphone out from Analog Mixer stage, no reduction in volume

    }

    audioreactive_I2CDevice *_codec = nullptr;

  public:
    AC101Source(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f, bool i2sMaster=true) :
      I2SSource(sampleRate, blockSize, sampleScale, i2sMaster) {
      _slotMask = I2S_STD_SLOT_LEFT;
    };

    void initialize(int8_t i2swsPin, int8_t i2ssdPin, int8_t i2sckPin, int8_t mclkPin,
                    uint8_t bitsPerSample = 32, bool useRightSlot = false, bool i2sMaster = true) {
      DEBUGSR_PRINTLN("AC101Source:: initialize();");

      if ((i2c_sda < 0) || (i2c_scl < 0)) {
        ERRORSR_PRINTF("\nAR: invalid AC101 global I2C pins: SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      if (!AR_I2C.isReady() && !AR_I2C.begin(i2c_sda, i2c_scl)) {
        ERRORSR_PRINTF("\nAR: failed to install audioreactive I2C bus with SDA=%d, SCL=%d\n", i2c_sda, i2c_scl);
        return;
      }
      _codec = AR_I2C.addDevice(AC101_ADDR, 400000);
      if (!_codec) {
        ERRORSR_PRINTLN("AR: failed to add AC101 to I2C bus");
        return;
      }

      // First route mclk, then configure ADC over I2C, then configure I2S
      _ac101InitAdc();
      I2SSource::initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin, bitsPerSample, useRightSlot, i2sMaster);
    }

    void deinitialize() {
      I2SSource::deinitialize();
      _codec = nullptr;
    }

};

//-----------------------------------------------------------------------------------
// esp_codec_dev-based audio sources (IDF 4.4+)
// These use the official espressif esp_codec_dev driver which handles I2S internally.
// Each class creates its own esp_codec_dev handle and uses it for audio input.
//-----------------------------------------------------------------------------------
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)

/* Base class for esp_codec_dev-based codecs
   Manages the esp_codec_dev handle lifecycle.
   Subclasses provide codec-specific configuration via virtual methods.
*/
class CodecDevSource : public AudioSource {
  public:
    void initialize(
      int8_t i2swsPin  = -1,
      int8_t i2ssdPin  = -1,
      int8_t i2sckPin  = -1,
      int8_t mclkPinIn = -1,
      uint8_t bitsPerSample = 32,
      bool useRightSlot  = false,
      bool i2sMaster     = true) override
    {
      (void)mclkPinIn;
      _deinitialize();

      if (i2swsPin < 0 || i2ssdPin < 0 || i2sckPin < 0) {
        USER_PRINTLN("CodecDevSource: pins not configured, skipping.");
        return;
      }

      // Join I2C bus if not already started (via the audioreactive_I2C class —
      // installs an i2c_master_bus on AR_I2C_PORT, never touches Arduino Wire).
      int8_t sda = -1, scl = -1;
      if (i2c_sda >= 0 && i2c_scl >= 0) { sda = i2c_sda; scl = i2c_scl; }
      #ifdef HW_PIN_SDA
      else                               { sda = HW_PIN_SDA; scl = HW_PIN_SCL; }
      #endif
      DEBUGSR_PRINTF("CodecDevSource: I2C pins SDA=%d SCL=%d (port=%d)\n", sda, scl, (int)AR_I2C.port());
      if (sda < 0 || scl < 0) {
        ERRORSR_PRINTF("CodecDevSource: invalid I2C pins SDA=%d SCL=%d\n", sda, scl);
        return;
      }
      if (!AR_I2C.isReady() && !AR_I2C.begin(sda, scl)) {
        ERRORSR_PRINTLN("CodecDevSource: audioreactive_I2C bus install failed");
        return;
      }

      // Let subclass create the codec interface (this also creates the control interface)
      _codecIf = _createCodecInterface();
      if (!_codecIf) {
        ERRORSR_PRINTLN("CodecDevSource: _createCodecInterface failed");
        return;
      }

      // Pre-allocate the I²S RX channel up front, then hand it to the
      // esp_codec_dev library via _createDataInterface(). The library's
      // set_fs path dereferences the channel handle directly, so we must
      // guarantee it's valid BEFORE the first esp_codec_dev_open() call —
      // we can't rely on the lib creating the handle itself.
      _allocI2sRxChannel(i2swsPin, i2ssdPin, i2sckPin, mclkPinIn,
                           bitsPerSample, useRightSlot, i2sMaster);
      if (!_rx_handle) {
        ERRORSR_PRINTLN("CodecDevSource: I2S RX channel alloc failed");
        return;
      }

      // Let subclass create the data interface (I2S)
      _dataIf = _createDataInterface();
      if (!_dataIf) {
        ERRORSR_PRINTLN("CodecDevSource: _createDataInterface failed");
        return;
      }

      // Create high-level esp_codec_dev device
      esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = _codecDevType(),
        .codec_if  = _codecIf,
        .data_if   = _dataIf,
      };
      _codecDev = esp_codec_dev_new(&dev_cfg);
      if (!_codecDev) {
        ERRORSR_PRINTLN("CodecDevSource: esp_codec_dev_new failed");
        return;
      }

      // Open with sample format
      esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bitsPerSample,
        .channel         = 1,
        .channel_mask    = 0,
        .sample_rate     = _sampleRate,
        .mclk_multiple   = 256,
      };
      int err = esp_codec_dev_open(_codecDev, &fs);
      if (err != ESP_CODEC_DEV_OK) {
        ERRORSR_PRINTF("CodecDevSource: esp_codec_dev_open failed: 0x%x\n", err);
        esp_codec_dev_delete(_codecDev);
        _codecDev = nullptr;
        return;
      }

      // Configure the ES8311 via the canonical high-level esp_codec_dev API.
      // The library's open() leaves the microphone at 0 dB gain (effectively
      // muted for the on-board analog MEMS mic). The legacy ES8311Source
      // explicitly writes register 0x17 = 0b10111111 (+30 dB PGA gain) and
      // keeps the ADC unmuted (reg 0x1B = 0b00000101). Mirror that here.
      esp_codec_dev_set_in_gain(_codecDev, 30.0f);    // +30 dB mic PGA (reg 0x17)
      esp_codec_dev_set_in_mute(_codecDev, false);    // unmute (reg 0x1B = 1)

      // Some IDF v5.3 + P4 combinations don't actually power up the analog
      // path in es8311_codec_open(). Force it via direct register writes
      // matching the legacy ES8311Source sequence.
      // 0x0D bit 0 = analog power-up
      // 0x0C bit 0 = digital power-up
      // 0x0B bit 0 = system power-up
      esp_codec_dev_write_reg(_codecDev, 0x0B, 0x00);
      esp_codec_dev_write_reg(_codecDev, 0x0C, 0x01);
      esp_codec_dev_write_reg(_codecDev, 0x0D, 0x01);
      // 0x00 bits [2:0] = 0b000 = standby; 0b101 = ADC + DAC running.
      // The legacy code ends with 0x00 = 0b10000000 (RESET+start).
      esp_codec_dev_write_reg(_codecDev, 0x00, 0x80);

      // ── DEBUG: read back key ES8311 registers + canonical state values
      float inGain = -99.0f;
      int outVol = -99;
      bool inMute = true, outMute = true;
      esp_codec_dev_get_in_gain(_codecDev, &inGain);
      esp_codec_dev_get_out_vol(_codecDev, &outVol);
      esp_codec_dev_get_in_mute(_codecDev, &inMute);
      esp_codec_dev_get_out_mute(_codecDev, &outMute);
      DEBUGSR_PRINTF("ES8311IDF: get_in_gain=%.1f get_out_vol=%d get_in_mute=%d get_out_mute=%d\n",
                     inGain, outVol, inMute ? 1 : 0, outMute ? 1 : 0);
      // Read back power/clock/gain registers
      auto dump = [&](uint8_t reg, const char* name) {
        int v = -1;
        esp_err_t err = esp_codec_dev_read_reg(_codecDev, reg, &v);
        DEBUGSR_PRINTF("ES8311IDF: reg 0x%02X (%s) = 0x%02X (err=%d)\n", reg, name, v & 0xFF, err);
      };
      dump(0x00, "STATE/CTRL");
      dump(0x0B, "SYS_CTRL");
      dump(0x0C, "DIG_PWR");
      dump(0x0D, "ANA_PWR");
      dump(0x14, "ADC_VOL");
      dump(0x17, "ADC_GAIN");
      dump(0x1B, "ADC_MUTE");
      dump(0x1C, "ADC_HPF");

      DEBUGSR_PRINTF("CodecDevSource: initialized (rate=%u)\n", (unsigned)_sampleRate);
      _initialized = true;
    }

    void deinitialize() override { _deinitialize(); }

    void getSamples(float *buffer, uint16_t num_samples) override {
      // The destination is defined before anything else happens - see the
      // comment on the other getSamples().
      memset(buffer, 0, sizeof(float) * num_samples);
      if (!_initialized || !_rx_handle) return;

      if (num_samples > I2S_SAMPLES_MAX) num_samples = I2S_SAMPLES_MAX;

      // Read directly via i2s_channel_read (same as the legacy I2SSource
      // path). The lib's esp_codec_dev_read was returning zeros — its
      // internal read path doesn't work on the P4 with our I²S handle.
      I2S_datatype raw[I2S_SAMPLES_MAX];
      size_t bytes_read = 0;
      esp_err_t err = i2s_channel_read(_rx_handle, (void *)raw,
                                       num_samples * sizeof(I2S_datatype),
                                       &bytes_read, portMAX_DELAY);
      if (err != ESP_OK) {
        DEBUGSR_PRINTF("CodecDevSource: i2s_channel_read failed: %d\n", err);
        return;
      }
      if (bytes_read != (num_samples * sizeof(I2S_datatype))) {
        DEBUGSR_PRINTF("CodecDevSource: short read: %u / %u\n",
                       (unsigned)bytes_read, (unsigned)(num_samples * sizeof(I2S_datatype)));
        return;
      }

      // Codec output is left-justified in 32-bit words (the lower 16 bits
      // are zero or codec-DSP noise), so dividing by 2^16 is the right
      // normalisation for both the 16-bit and 32-bit paths. (I2SSource
      // does the same thing; the #ifdef here was an apparent fix that
      // produced two identical branches.)
      for (uint16_t i = 0; i < num_samples; i++) {
        buffer[i] = ((float)raw[i] / 65536.0f) * _sampleScale;
      }
    }

  protected:
    virtual esp_codec_dev_type_t _codecDevType() const = 0;
    virtual const audio_codec_if_t *_createCodecInterface() = 0;
    virtual const audio_codec_data_if_t *_createDataInterface() = 0;

    CodecDevSource(SRate_t sampleRate, int blockSize, float sampleScale, bool i2sMaster = true)
      : AudioSource(sampleRate, blockSize, sampleScale, i2sMaster)
      , _codecDev(nullptr)
      , _codecIf(nullptr)
      , _dataIf(nullptr)
      , _rx_handle(nullptr)
    {}

    virtual ~CodecDevSource() { _deinitialize(); }

    esp_codec_dev_handle_t _codecDev;
    i2s_chan_handle_t _rx_handle;
    int8_t _wsPin  = I2S_GPIO_UNUSED;
    int8_t _dinPin = I2S_GPIO_UNUSED;
    int8_t _bckPin = I2S_GPIO_UNUSED;
    int8_t _mclkPin = I2S_GPIO_UNUSED;

    /* Pre-allocate the I²S RX channel with the user-requested bit-width and
       slot. Mirrors I2SSource::initialize() but kept local to CodecDevSource
       so the IDF path can share one channel allocation across all ES8311/
       ES8388/etc. variants. Returns true on success. */
    bool _allocI2sRxChannel(int8_t i2swsPin, int8_t i2ssdPin, int8_t i2sckPin,
                            int8_t mclkPin, uint8_t bitsPerSample,
                            bool useRightSlot, bool i2sMaster) {
      if (i2swsPin < 0 || i2ssdPin < 0 || i2sckPin < 0) return false;

      // Tear down any previous allocation so the new pin set takes effect.
      _deallocateI2sPins();

      // Claim the I²S pins through pinManager so they show up in the
      // GPIO info table and are protected from other usermods claiming
      // the same pins. Mirror I2SSource::initialize()'s allocation pattern.
      if (!pinManager.allocatePin(i2swsPin, true,  PinOwner::UM_Audioreactive) ||
          !pinManager.allocatePin(i2ssdPin, false, PinOwner::UM_Audioreactive)) {
        ERRORSR_PRINTF("\nAR: failed to allocate I2S pins: ws=%d, sd=%d\n", i2swsPin, i2ssdPin);
        return false;
      }
      _wsPin  = i2swsPin;
      _dinPin = i2ssdPin;
      if (i2sckPin != I2S_GPIO_UNUSED) {
        if (!pinManager.allocatePin(i2sckPin, true, PinOwner::UM_Audioreactive)) {
          ERRORSR_PRINTF("\nAR: failed to allocate I2S pins: sck=%d\n", i2sckPin);
          _deallocateI2sPins();
          return false;
        }
        _bckPin = i2sckPin;
      }
      if (mclkPin != I2S_GPIO_UNUSED) {
        if (!pinManager.allocatePin(mclkPin, true, PinOwner::UM_Audioreactive)) {
          ERRORSR_PRINTF("\nAR: failed to allocate I2S pin: MCLK=%d\n", mclkPin);
          _deallocateI2sPins();
          return false;
        }
        _mclkPin = mclkPin;
      }

      if (_rx_handle) {
        i2s_channel_disable(_rx_handle);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
      }
      i2s_data_bit_width_t data_width = I2S_DATA_BIT_WIDTH_16BIT;
      switch (bitsPerSample) {
        case 24: data_width = I2S_DATA_BIT_WIDTH_24BIT; break;
        case 32: data_width = I2S_DATA_BIT_WIDTH_32BIT; break;
        default: break;
      }
      i2s_std_slot_mask_t slot = useRightSlot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
      i2s_chan_config_t chan_cfg = {
        .id            = AR_I2S_PORT,
        .role          = i2sMaster ? I2S_ROLE_MASTER : I2S_ROLE_SLAVE,
        .dma_desc_num  = AR_I2S_DMA_DESCS,
        .dma_frame_num = (uint32_t)_blockSize,
        .auto_clear    = false,
      };
      esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &_rx_handle);
      if (err != ESP_OK || !_rx_handle) {
        ERRORSR_PRINTF("CodecDevSource: i2s_new_channel failed: %d\n", err);
        return false;
      }
      i2s_std_slot_config_t std_slot_cfg = {
        .data_bit_width = data_width,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .slot_mode      = I2S_SLOT_MODE_MONO,
        .slot_mask      = slot,
        .ws_width       = bitsPerSample,
        .ws_pol         = false,
        .bit_shift      = true,
        .left_align     = true,
        .big_endian     = false,
        .bit_order_lsb  = false,
      };
      i2s_std_clk_config_t std_clk_cfg = {
        .sample_rate_hz = _sampleRate,
        .clk_src        = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
      };
      i2s_std_config_t std_cfg = {
        .clk_cfg  = std_clk_cfg,
        .slot_cfg = std_slot_cfg,
        .gpio_cfg = {
          .mclk        = (gpio_num_t)mclkPin,
          .bclk        = (gpio_num_t)i2sckPin,
          .ws          = (gpio_num_t)i2swsPin,
          .dout        = I2S_GPIO_UNUSED,
          .din         = (gpio_num_t)i2ssdPin,
          .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
      };
      err = i2s_channel_init_std_mode(_rx_handle, &std_cfg);
      if (err != ESP_OK) {
        ERRORSR_PRINTF("CodecDevSource: i2s_channel_init_std_mode failed: %d\n", err);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
        return false;
      }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
      // P4 workaround: see I2SSource::initialize() — the new I2S driver does
      // not explicitly disable the GPIO output driver on the DIN pin.
      gpio_set_direction((gpio_num_t)i2ssdPin, GPIO_MODE_INPUT);
#endif
      err = i2s_channel_enable(_rx_handle);
      if (err != ESP_OK) {
        ERRORSR_PRINTF("CodecDevSource: i2s_channel_enable failed: %d\n", err);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
        _deallocateI2sPins();
        return false;
      }
      return true;
    }

    /* Release the I2S pins we claimed via pinManager. */
    void _deallocateI2sPins() {
      if (_mclkPin != I2S_GPIO_UNUSED) { pinManager.deallocatePin(_mclkPin, PinOwner::UM_Audioreactive); _mclkPin = I2S_GPIO_UNUSED; }
      if (_bckPin  != I2S_GPIO_UNUSED) { pinManager.deallocatePin(_bckPin,  PinOwner::UM_Audioreactive); _bckPin  = I2S_GPIO_UNUSED; }
      if (_dinPin  != I2S_GPIO_UNUSED) { pinManager.deallocatePin(_dinPin,  PinOwner::UM_Audioreactive); _dinPin  = I2S_GPIO_UNUSED; }
      if (_wsPin   != I2S_GPIO_UNUSED) { pinManager.deallocatePin(_wsPin,   PinOwner::UM_Audioreactive); _wsPin   = I2S_GPIO_UNUSED; }
    }

  private:
    void _deinitialize() {
      if (_codecDev) {
        esp_codec_dev_close(_codecDev);
        esp_codec_dev_delete(_codecDev);
        _codecDev = nullptr;
      }
      // codec_if and data_if are freed by esp_codec_dev_delete
      _codecIf = nullptr;
      _dataIf = nullptr;

      // Tear down the I²S RX channel we pre-allocated, and release the
      // pins back to pinManager.
      if (_rx_handle) {
        i2s_channel_disable(_rx_handle);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
      }
      _deallocateI2sPins();

      _initialized = false;
    }

    const audio_codec_if_t    *_codecIf;
    const audio_codec_data_if_t *_dataIf;
};

// ES8311 via esp_codec_dev
#ifdef CONFIG_CODEC_ES8311_SUPPORT
class ES8311IDFSource : public CodecDevSource {
  public:
    ES8311IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

    ~ES8311IDFSource() override {
      // The ctrl_if shim was calloc()'d in _createCodecInterface().
      // The shim's `base` field sits at offset 0 of the allocation, so
      // casting _ctrlIf back to void* and free()ing recovers the
      // original block.
      if (_ctrlIf) {
        free((void *)_ctrlIf);
        _ctrlIf = nullptr;
      }
      _codec = nullptr;  // AR_I2C owns the device handle
    }

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      // Workaround for esp_codec_dev on IDF v5.x: the library's
      // audio_codec_new_i2c_ctrl() returns a ctrl_if whose internal
      // device handle is misconfigured for the P4 ES8311 (reliably NACKs).
      // We build our own ctrl_if shim that wraps our AR_I2C-managed
      // device handle, then open it explicitly (the library's own factory
      // would do this internally before returning). The ES8311 register
      // sequence is then driven by the library via our shim's write
      // callback, which uses our (known-good) device handle.
      if (!_codec) {
        _codec = AR_I2C.addDevice(0x18);
        if (!_codec) {
          ERRORSR_PRINTLN("ES8311IDFSource: AR_I2C.addDevice(0x18) failed");
          return nullptr;
        }
      }
      if (!_ctrlIf) {
        _ctrlIf = AR_I2C._ar_make_i2c_ctrl_if(_codec->handle());
        if (!_ctrlIf) {
          ERRORSR_PRINTLN("ES8311IDFSource: ctrl_if shim alloc failed");
          return nullptr;
        }
        // Open the control interface explicitly — the library's factory
        // would do this before returning; our shim is built on-demand so
        // we have to do it here. Without this, es8311_codec_open() bails
        // out with "Control interface not open yet".
        _ctrlIf->open(_ctrlIf, nullptr, 0);
      }

      static const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
      if (!gpio_if) return nullptr;

      // Pre-codec register setup: replicate the ESPhome es8311.cpp
      // sequence (the upstream-tested driver) via the shim's write_reg.
      // The previous values were hand-tuned and may have had suboptimal
      // bits set. See:
      // https://github.com/esphome/esphome/blob/.../esphome/components/es8311/es8311.cpp
      uint8_t v00 = 0x00, v01 = 0x01, v02 = 0x02, v03 = 0x03, v10 = 0x10,
              v14 = 0x10, v3F = 0x3F, v6A = 0x6A, v80 = 0x80,
              vC8 = 0xC8, vFF = 0xFF;
      // Reset
      _ctrlIf->write_reg(_ctrlIf, 0x00, 1, &v00, 1);
      // configure_clock_() — the vendor row for MCLK = 256 x rate. Espressif's
      // own espressif/es8311 component, ESPhome's es8311.cpp and the Linux ALSA
      // driver all produce these same values for every rate we use (the row is
      // rate-independent; only the MCLK frequency changes):
      //   01 = 0x3F  all clocks on, MCLK from the MCLK pin
      //   02 = 0x00  DIV_PRE = 1, MULT_PRE = 1x
      //   03 = 0x10  single speed, ADC OSR 128
      //   04 = 0x10  DAC OSR 128
      //   05 = 0x00  ADC/DAC clock divider = 1
      //   06 = 0x03  BCLK divider 4, no invert
      //   07 = 0x00 / 08 = 0xFF  LRCK divider 256
      // This path already wrote 02 = 0x00, but 04 and 06 were 0x00 where the
      // vendors write 0x10 and 0x03, and the LRCK divider was never written
      // at all. Now both codec paths in this file agree.
      _ctrlIf->write_reg(_ctrlIf, 0x01, 1, &v3F, 1);    // enable all clocks
      _ctrlIf->write_reg(_ctrlIf, 0x02, 1, &v00, 1);    // DIV_PRE=1, MULT_PRE=1x
      _ctrlIf->write_reg(_ctrlIf, 0x03, 1, &v10, 1);    // ADC OSR=128
      _ctrlIf->write_reg(_ctrlIf, 0x04, 1, &v10, 1);    // DAC OSR=128
      _ctrlIf->write_reg(_ctrlIf, 0x05, 1, &v00, 1);    // div=1
      _ctrlIf->write_reg(_ctrlIf, 0x06, 1, &v03, 1);    // BCLK divider 4, no invert
      _ctrlIf->write_reg(_ctrlIf, 0x07, 1, &v00, 1);    // LRCK divider high
      _ctrlIf->write_reg(_ctrlIf, 0x08, 1, &vFF, 1);    // LRCK divider low -> 256
      // configure_format_() — I²S 32-bit
      _ctrlIf->write_reg(_ctrlIf, 0x09, 1, &v10, 1);    // SDP IN
      _ctrlIf->write_reg(_ctrlIf, 0x0A, 1, &v10, 1);    // SDP OUT
      // configure_mic_() — analog MIC + PGA gain
      //
      // PGA gain 0 dB, not the +30 dB that both ESPhome's and Espressif's
      // drivers use. They use it because they drive a MICROPHONE; this rig
      // takes a line-level input, and +30 dB of extra gain puts the ADC into
      // overload - the symptom of which (bands 14/15 hot with no HF content,
      // AGC gain pinned at 0.03x) looks nothing like overload and cost a long
      // time to diagnose. The non-IDF path in this same file already used
      // 0 dB; now both paths agree.
      _ctrlIf->write_reg(_ctrlIf, 0x14, 1, &v14, 1);
      _ctrlIf->write_reg(_ctrlIf, 0x16, 1, &v00, 1);
      _ctrlIf->write_reg(_ctrlIf, 0x17, 1, &vC8, 1);
      // ALC and automute off (REG18 bit7 / bit6) - see the note in
      // _es8311InitAdc(); the chip's power-on default is not a safe default.
      _ctrlIf->write_reg(_ctrlIf, 0x18, 1, &v00, 1);
      // Power-up
      _ctrlIf->write_reg(_ctrlIf, 0x0D, 1, &v01, 1);    // analog
      _ctrlIf->write_reg(_ctrlIf, 0x0E, 1, &v02, 1);    // PGA + ADC mod
      _ctrlIf->write_reg(_ctrlIf, 0x12, 1, &v00, 1);    // DAC
      _ctrlIf->write_reg(_ctrlIf, 0x13, 1, &v10, 1);    // HP drive
      _ctrlIf->write_reg(_ctrlIf, 0x1C, 1, &v6A, 1);    // ADC EQ bypass
      _ctrlIf->write_reg(_ctrlIf, 0x37, 1, &v00, 1);    // DAC EQ bypass (reg 0x37 = 0x00)
      // Start
      _ctrlIf->write_reg(_ctrlIf, 0x00, 1, &v80, 1);

      static es8311_codec_cfg_t codec_cfg = {
        .ctrl_if     = _ctrlIf,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin      = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = true,
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = {5.0f, 3.3f, 0.0f},
        .no_dac_ref  = true,    // mono mic: leave right channel empty, not filled with DAC output
        .mclk_div    = 256,
      };
      const audio_codec_if_t *codec = es8311_codec_new(&codec_cfg);
      if (!codec) {
        ERRORSR_PRINTLN("ES8311IDFSource: es8311_codec_new failed");
      }
      return codec;
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }

    audioreactive_I2CDevice *_codec = nullptr;
    const audio_codec_ctrl_if_t *_ctrlIf = nullptr;
};
#else
#warning "ES8311 IDF source is not available on this target (CONFIG_CODEC_ES8311_SUPPORT is not set)"
#endif

// ES8388 via esp_codec_dev
#ifdef CONFIG_CODEC_ES8388_SUPPORT
class ES8388IDFSource : public CodecDevSource {
  public:
    ES8388IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x20,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
      if (!gpio_if) return nullptr;

      static es8388_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
        .master_mode = true,
        .pa_pin      = GPIO_NUM_NC,
        .pa_reverted = false,
        .hw_gain     = {5.0f, 3.3f, 0.0f},
      };
      return es8388_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "ES8388 IDF source is not available on this target (CONFIG_CODEC_ES8388_SUPPORT is not set)"
#endif

// ES8374 via esp_codec_dev
#ifdef CONFIG_CODEC_ES8374_SUPPORT
class ES8374IDFSource : public CodecDevSource {
  public:
    ES8374IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x20,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
      if (!gpio_if) return nullptr;

      static es8374_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
        .master_mode = true,
        .pa_pin      = GPIO_NUM_NC,
        .pa_reverted = false,
      };
      return es8374_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "ES8374 IDF source is not available on this target (CONFIG_CODEC_ES8374_SUPPORT is not set)"
#endif

// ES7243 via esp_codec_dev
#ifdef CONFIG_CODEC_ES7243_SUPPORT
class ES7243IDFSource : public CodecDevSource {
  public:
    ES7243IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x13,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static es7243_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
      };
      return es7243_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "ES7243 IDF source is not available on this target (CONFIG_CODEC_ES7243_SUPPORT is not set)"
#endif

// ES7243E via esp_codec_dev
#ifdef CONFIG_CODEC_ES7243E_SUPPORT
class ES7243EIDFSource : public CodecDevSource {
  public:
    ES7243EIDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x13,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static es7243e_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
      };
      return es7243e_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "ES7243E IDF source is not available on this target (CONFIG_CODEC_ES7243E_SUPPORT is not set)"
#endif

// ZL38063 via esp_codec_dev
#ifdef CONFIG_CODEC_ZL38063_SUPPORT
class ZL38063IDFSource : public CodecDevSource {
  public:
    ZL38063IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x30,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
      if (!gpio_if) return nullptr;

      static zl38063_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin      = GPIO_NUM_NC,
        .pa_reverted = false,
        .reset_pin   = GPIO_NUM_NC,
      };
      return zl38063_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "ZL38063 IDF source is not available on this target (CONFIG_CODEC_ZL38063_SUPPORT is not set)"
#endif  // CONFIG_CODEC_ZL38063_SUPPORT

// CJC8910 via esp_codec_dev
#ifdef CONFIG_CODEC_CJC8910_SUPPORT
class CJC8910IDFSource : public CodecDevSource {
  public:
    CJC8910IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x30,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
      if (!gpio_if) return nullptr;

      static cjc8910_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin      = GPIO_NUM_NC,
        .pa_reverted = false,
        .invert_lr   = false,
        .invert_sclk = false,
        .hw_gain     = {5.0f, 3.3f, 0.0f},
      };
      return cjc8910_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "CJC8910 IDF source is not available on this target (CONFIG_CODEC_CJC8910_SUPPORT is not set)"
#endif  // CONFIG_CODEC_CJC8910_SUPPORT

// ES8389 via esp_codec_dev
#ifdef CONFIG_CODEC_ES8389_SUPPORT
class ES8389IDFSource : public CodecDevSource {
  public:
    ES8389IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x20,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
      if (!gpio_if) return nullptr;

      static es8389_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin      = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = true,
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = {5.0f, 3.3f, 0.0f},
        .no_dac_ref  = false,
        .mclk_div    = 256,
      };
      return es8389_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "ES8389 IDF source is not available on this target (CONFIG_CODEC_ES8389_SUPPORT is not set)"
#endif

// ES7210 via esp_codec_dev
#ifdef CONFIG_CODEC_ES7210_SUPPORT
class ES7210IDFSource : public CodecDevSource {
  public:
    ES7210IDFSource(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f)
      : CodecDevSource(sampleRate, blockSize, sampleScale) {}

  protected:
    esp_codec_dev_type_t _codecDevType() const override {
      return ESP_CODEC_DEV_TYPE_IN;
    }

    const audio_codec_if_t *_createCodecInterface() override {
      audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AR_I2C.port(),
        .addr       = 0x40,
        .bus_handle = AR_I2C.busHandle(),
      };
      const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
      if (!ctrl_if) return nullptr;

      static es7210_codec_cfg_t codec_cfg = {
        .ctrl_if     = ctrl_if,
        .master_mode = true,
        .mic_selected = 0,
        .mclk_src    = ES7210_MCLK_FROM_PAD,
        .mclk_div    = 256,
      };
      return es7210_codec_new(&codec_cfg);
    }

    const audio_codec_data_if_t *_createDataInterface() override {
      audio_codec_i2s_cfg_t i2s_cfg = {
        .port       = AR_I2S_PORT,
        .rx_handle  = _rx_handle,
        .tx_handle  = nullptr,
      };
      return audio_codec_new_i2s_data(&i2s_cfg);
    }
};
#else
#warning "ES7210 IDF source is not available on this target (CONFIG_CODEC_ES7210_SUPPORT is not set)"
#endif

#endif  // ESP_IDF_VERSION >= 4.4.0

// YEAH YEAH WE KNOW BUT NOBODY WILL
// I2SAdcSource (classic-ESP32 ADC mic) was removed when migrating to the IDF v5 new I2S driver,
// because i2s_set_adc_mode / i2s_adc_enable / I2S_MODE_ADC_BUILT_IN have no replacement in the new API.

/* SPH0645 Microphone
   This is an I2S microphone with some timing quirks that need
   special consideration.
*/

// SPH0645 workaround note:
// The original legacy-driver workaround (REG_SET_BIT on I2S_TIMING_REG/I2S_CONF_REG)
// addressed esp-idf issue #7192 (IDFGH-5453). In the IDF v5 new I2S driver those
// registers/macros are no longer exported. The new driver correctly configures
// Philips-standard framing by default, and the workaround does not appear to be
// required when using the new API. If a board regresses, file an issue.
class SPH0654 : public I2SSource {
  public:
    SPH0654(SRate_t sampleRate, int blockSize, float sampleScale = 1.0f, bool i2sMaster=true) :
      I2SSource(sampleRate, blockSize, sampleScale, i2sMaster)
    {}

    void initialize(int8_t i2swsPin, int8_t i2ssdPin, int8_t i2sckPin, int8_t mclkPin = I2S_GPIO_UNUSED,
                    uint8_t bitsPerSample = 24, bool useRightSlot = false, bool i2sMaster = true) {
      DEBUGSR_PRINTLN("SPH0654:: initialize();");
      // SPH0645 puts 24-bit data on the wire.
      I2SSource::initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin, bitsPerSample, useRightSlot, i2sMaster);
    }
};
