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

constexpr i2s_port_t AR_I2S_PORT = I2S_NUM_0;       // I2S port to use (do not change!  I2S_NUM_1 possible but this has
                                                    // strong limitations -> no MCLK routing, no PDM support on some targets

// see https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/hw-reference/chip-series-comparison.html#related-documents
// and https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html#overview-of-all-modes
#if !defined(CONFIG_SOC_CPU_HAS_FPU) || defined(ESP8266) || defined(ESP8265)
  // there are two things in these MCUs that could lead to problems with audio processing:
  // * no floating point hardware (FPU) support - FFT uses float calculations. If done in software, a strong slow-down can be expected (between 8x and 20x)
  // * single core, so FFT task might slow down other things like LED updates
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
//   I2S_GRAB_ADC1_COMPLETELY - obsolete (I2SAdcSource was removed when
//                               migrating to the IDF v5 new I2S driver)
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
#define I2S_SAMPLES_MAX 512  // same as samplesFFT

// TEMPORARY DIAGNOSTIC - remove before release.
// Dumps the raw (pre-scaling) I2S sample statistics and the resolved I2S pin/clock
// config, to distinguish "no data on the wire" from "data present but mis-scaled".
#define AR_DEBUG_RAW_SAMPLES

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

    /* identify Audiosource type - I2S-ADC or I2S-digital */
    typedef enum{Type_unknown=0, Type_I2SAdc=1, Type_I2SDigital=2} AudioSourceType;
    virtual AudioSourceType getType(void) {return(Type_I2SDigital);}               // default is "I2S digital source" - ADC type overrides this method
 
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
        .dma_desc_num  = 24,
        .dma_frame_num = (uint32_t)_blockSize,
        .auto_clear    = false,
      };
      #if defined(WLED_ENABLE_HUB75MATRIX)
      chan_cfg.dma_desc_num = 18;
      #endif
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
        DEBUGSR_PRINTF("AR: I2S#0 driver installed in %s mode, %u-bit.\n",
                       _i2sMaster ? "MASTER" : "SLAVE", bitsPerSample);
#ifdef AR_DEBUG_RAW_SAMPLES
        // TEMPORARY DIAGNOSTIC - remove before release.
        USER_PRINTF("AR CFG: ws=%d sd=%d bck=%d mclk=%d | bits=%u slot=%s role=%s rate=%u mclk_mult=256\n",
                    (int)i2swsPin, (int)i2ssdPin, (int)i2sckPin, (int)mclkPin,
                    (unsigned)bitsPerSample,
                    (_slotMask == I2S_STD_SLOT_RIGHT) ? "RIGHT" : "LEFT",
                    _i2sMaster ? "MASTER" : "SLAVE",
                    (unsigned)_sampleRate);
        if (mclkPin == I2S_GPIO_UNUSED) {
          USER_PRINTLN("AR CFG: *** MCLK pin is UNUSED - an external line-in ADC will output silence ***");
        }
#endif
      }

      err = i2s_channel_enable(_rx_handle);
      if (err != ESP_OK) {
        ERRORSR_PRINTF("AR: Failed to enable I2S channel: %d\n", err);
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
        return;
      }

#ifdef AR_DEBUG_RAW_SAMPLES
      // TEMPORARY DIAGNOSTIC - remove before release.
      // Dump the post-init GPIO matrix state for the I2S pins. This shows the
      // routed peripheral signal per pin, so we can confirm DIN is actually
      // connected to the I2S RX signal and that BCLK/WS/MCLK are driven.
      {
        uint64_t mask = 0;
        if (i2swsPin  != I2S_GPIO_UNUSED) mask |= (1ULL << (uint8_t)i2swsPin);
        if (i2ssdPin  != I2S_GPIO_UNUSED) mask |= (1ULL << (uint8_t)i2ssdPin);
        if (i2sckPin  != I2S_GPIO_UNUSED) mask |= (1ULL << (uint8_t)i2sckPin);
        if (mclkPin   != I2S_GPIO_UNUSED) mask |= (1ULL << (uint8_t)mclkPin);
        USER_PRINTF("AR GPIO DUMP [I2SSource] ws=%d sd=%d bck=%d mclk=%d:\n",
                    (int)i2swsPin, (int)i2ssdPin, (int)i2sckPin, (int)mclkPin);
        gpio_dump_io_configuration(stdout, mask);
        fflush(stdout);
      }
#endif

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
      if (_initialized && _rx_handle) {
        size_t bytes_read = 0;

        memset(buffer, 0, sizeof(float) * num_samples);
        I2S_datatype *newSamples = newSampleBuffer;
        if (num_samples > I2S_SAMPLES_MAX) num_samples = I2S_SAMPLES_MAX;

        esp_err_t err = i2s_channel_read(_rx_handle, (void *)newSamples, num_samples * sizeof(I2S_datatype), &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
          DEBUGSR_PRINTF("Failed to get samples: %d\n", err);
          return;
        }

        if (bytes_read != (num_samples * sizeof(I2S_datatype))) {
          DEBUGSR_PRINTF("Failed to get enough samples: wanted: %d read: %d\n", num_samples * sizeof(I2S_datatype), bytes_read);
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

#ifdef AR_DEBUG_RAW_SAMPLES
        // TEMPORARY DIAGNOSTIC - remove before release.
        // Report raw pre-scale statistics roughly once per second.
        {
          static unsigned long lastRawDump = 0;
          if (millis() - lastRawDump > 1000) {
            lastRawDump = millis();
            int32_t rawMin = newSamples[0], rawMax = newSamples[0];
            uint16_t nonZero = 0;
            for (int i = 0; i < num_samples; i++) {
              int32_t v = newSamples[i];
              if (v < rawMin) rawMin = v;
              if (v > rawMax) rawMax = v;
              if (v != 0) nonZero++;
            }
            USER_PRINTF("AR RAW: n=%u bytes=%u nonzero=%u min=%ld max=%ld scale=%.5f\n",
                        (unsigned)num_samples, (unsigned)bytes_read, (unsigned)nonZero,
                        (long)rawMin, (long)rawMax, _sampleScale);
          }
        }
#endif
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
    audioreactive_I2CDevice *_activeAdc() {
      if (_es7210 && AR_I2C.probe(_es7210->addr())) return _es7210;
      return _es8311;
    }

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
      // driven by sample rate + MCLK multiple. We hard-code the values
      // for 22050 Hz / MCLK=256 (matching the legacy path).
      _i2cWrite(dev, 0x01, 0x3F);                  // Clock Manager: enable all clocks
      _i2cWrite(dev, 0x02, 0x40);                  // pre_div=0, pre_mult=0
      _i2cWrite(dev, 0x03, 0x10);                  // ADC OSR=128
      _i2cWrite(dev, 0x04, 0x00);                  // DAC OSR=128
      _i2cWrite(dev, 0x05, 0x00);                  // ADC/DAC div=1
      _i2cWrite(dev, 0x06, 0x03);                  // BCLK divider, no invert

      // configure_format_() equivalent — set SDP in/out resolution
      _i2cWrite(dev, 0x09, 0x10);                  // SDP IN  = 32-bit (4<<2)
      _i2cWrite(dev, 0x0A, 0x10);                  // SDP OUT = 32-bit (4<<2)
      _i2cWrite(dev, 0x07, 0x00);                  // LRCK divider hi
      _i2cWrite(dev, 0x08, 0xFF);                  // LRCK divider lo

      // configure_mic_() equivalent — enable analog MIC + max PGA gain
      _i2cWrite(dev, 0x14, 0x1A);                  // REG14 = 0x1A (analog MIC + max PGA)
      _i2cWrite(dev, 0x16, 0x00);                  // REG16 = ADC mixer/scale (0 = no scale)
      _i2cWrite(dev, 0x17, 0xC8);                  // REG17 = 0xC8 (max gain + ALC + automute)

      // Power-up sequence (matches ESPhome)
      _i2cWrite(dev, 0x0D, 0x01);                  // analog power-up
      _i2cWrite(dev, 0x0E, 0x02);                  // enable analog PGA + ADC modulator
      _i2cWrite(dev, 0x12, 0x00);                  // power up DAC
      _i2cWrite(dev, 0x13, 0x10);                  // enable output to HP drive
      _i2cWrite(dev, 0x1C, 0x6A);                  // ADC EQ bypass + DC offset cancel
      _i2cWrite(dev, 0x37, 0x08);                  // DAC EQ bypass
      _i2cWrite(dev, 0x00, 0x80);                  // start ADC + DAC
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
      (void)useRightSlot;
      (void)i2sMaster;
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
      if (!_initialized || !_rx_handle) return;

      memset(buffer, 0, sizeof(float) * num_samples);
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

#ifdef AR_DEBUG_RAW_SAMPLES
      // TEMPORARY DIAGNOSTIC - remove before release.
      {
        static unsigned long lastRawDumpCD = 0;
        if (millis() - lastRawDumpCD > 1000) {
          lastRawDumpCD = millis();
          int32_t rawMin = raw[0], rawMax = raw[0];
          uint16_t nonZero = 0;
          for (uint16_t i = 0; i < num_samples; i++) {
            int32_t v = raw[i];
            if (v < rawMin) rawMin = v;
            if (v > rawMax) rawMax = v;
            if (v != 0) nonZero++;
          }
          USER_PRINTF("AR RAW: n=%u bytes=%u nonzero=%u min=%ld max=%ld scale=%.5f [CodecDev]\n",
                      (unsigned)num_samples, (unsigned)bytes_read, (unsigned)nonZero,
                      (long)rawMin, (long)rawMax, _sampleScale);
        }
      }
#endif

#ifdef I2S_SAMPLE_DOWNSCALE_TO_16BIT
      for (uint16_t i = 0; i < num_samples; i++) {
        buffer[i] = ((float)raw[i] / 65536.0f) * _sampleScale;
      }
#else
      for (uint16_t i = 0; i < num_samples; i++) {
        buffer[i] = ((float)raw[i] / 65536.0f) * _sampleScale;  // see I2SSource
      }
#endif
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
        .dma_desc_num  = 24,
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
#ifdef AR_DEBUG_RAW_SAMPLES
      // TEMPORARY DIAGNOSTIC - remove before release.
      // Same format as the I2SSource log, so the working codec config can be
      // diffed directly against the failing legacy config.
      USER_PRINTF("AR CFG: ws=%d sd=%d bck=%d mclk=%d | bits=%u slot=%s role=%s rate=%u mclk_mult=256 [CodecDev]\n",
                  (int)i2swsPin, (int)i2ssdPin, (int)i2sckPin, (int)mclkPin,
                  (unsigned)bitsPerSample,
                  (slot == I2S_STD_SLOT_RIGHT) ? "RIGHT" : "LEFT",
                  i2sMaster ? "MASTER" : "SLAVE",
                  (unsigned)_sampleRate);
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
      uint8_t v00 = 0x00, v01 = 0x01, v02 = 0x02, v10 = 0x10,
              v1A = 0x1A, v3F = 0x3F, v6A = 0x6A, v80 = 0x80,
              vC8 = 0xC8;
      // Reset
      _ctrlIf->write_reg(_ctrlIf, 0x00, 1, &v00, 1);
      // configure_clock_() — values for 22050 Hz / MCLK=256
      _ctrlIf->write_reg(_ctrlIf, 0x01, 1, &v3F, 1);    // enable all clocks
      _ctrlIf->write_reg(_ctrlIf, 0x02, 1, &v00, 1);    // pre_div=0, pre_mult=0
      _ctrlIf->write_reg(_ctrlIf, 0x03, 1, &v10, 1);    // ADC OSR=128
      _ctrlIf->write_reg(_ctrlIf, 0x04, 1, &v00, 1);    // DAC OSR=128
      _ctrlIf->write_reg(_ctrlIf, 0x05, 1, &v00, 1);    // div=1
      _ctrlIf->write_reg(_ctrlIf, 0x06, 1, &v00, 1);    // BCLK divider
      // configure_format_() — I²S 32-bit
      _ctrlIf->write_reg(_ctrlIf, 0x09, 1, &v10, 1);    // SDP IN
      _ctrlIf->write_reg(_ctrlIf, 0x0A, 1, &v10, 1);    // SDP OUT
      // configure_mic_() — analog MIC + max PGA
      _ctrlIf->write_reg(_ctrlIf, 0x14, 1, &v1A, 1);
      _ctrlIf->write_reg(_ctrlIf, 0x16, 1, &v00, 1);
      _ctrlIf->write_reg(_ctrlIf, 0x17, 1, &vC8, 1);
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
