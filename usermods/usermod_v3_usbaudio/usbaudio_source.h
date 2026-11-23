// usbaudio_source.h
//
// The AudioReactive source that reads from a USB audio device.
//
// The USB half lives in usbaudio_usb_host.cpp, which is textually included
// into wled.cpp and therefore lives in a different translation unit from
// this one. The two are joined by the four functions declared in
// usbaudio_usb_host.h.
//
// The class itself is deliberately thin. AudioSource already is the
// extension point of the analysis: getSamples() is the single place where
// samples enter (audio_reactive.h:5230), and everything downstream - the DC
// blocker, the two filter banks, the loudness meter, the tempo estimator -
// works on the samples without knowing where they came from.

#pragma once

#include <stdint.h>
#include <string.h>

#ifdef USERMOD_USBAUDIO_USB

#include "../usermod_v3_usbaudio/usbaudio_usb_host.h"

// A USB audio device, seen by the analysis as a mono source.
//
// The device may be sending one channel or two; the analysis takes the LEFT
// channel, which is the channel every other audio path in this project
// takes too (the I2S path reads the left slot). A device with more than two
// channels is refused by the client rather than half-understood here.
class USBHostAudioSource : public AudioSource {
  public:
    USBHostAudioSource(SRate_t sampleRate, int blockSize)
      : AudioSource(sampleRate, blockSize, 1.0f, true) {
      // The samples arrive from the USB task already scaled to -1.0 .. +1.0,
      // so no further scaling is applied here.
      _sampleScale = 1.0f;
    }

    void initialize(int8_t i2swsPin = I2S_GPIO_UNUSED,
                    int8_t i2ssdPin = I2S_GPIO_UNUSED,
                    int8_t i2sckPin = I2S_GPIO_UNUSED,
                    int8_t mclkPin  = I2S_GPIO_UNUSED,
                    uint8_t bitsPerSample = 32,
                    bool    useRightSlot  = false,
                    bool    i2sMaster     = true) override {
      (void)i2swsPin; (void)i2ssdPin; (void)i2sckPin; (void)mclkPin;
      (void)bitsPerSample; (void)useRightSlot; (void)i2sMaster;

      // Nothing to initialise here. The USB Host client is installed by the
      // USB task, which runs independently of the analysis: it registers the
      // client, claims whichever audio device turns up, and starts the
      // stream. This source only reads what the client hands it.
      _initialized = true;
    }

    void deinitialize() override {
      _initialized = false;
    }

    void getSamples(float *buffer, uint16_t num_samples) override {
      if (!_initialized || !buffer || num_samples == 0) return;

      // The reader paces itself: it waits for the frame's worth of samples,
      // and it keeps the analysis' cadence even when there is no device (in
      // which case it hands back silence). It never blocks forever on a
      // device that stopped talking.
      uac_usb_read_samples(buffer, num_samples, 100);
    }

    // The source is "initialised" as soon as the analysis has it. Whether a
    // device is actually attached is a separate question, and the info page
    // asks that one.
    bool isInitialized(void) override { return _initialized; }

    // The clock the samples are actually arriving at. There is no MCLK here
    // - the device has its own crystal - but the rate is known and it is the
    // number worth reporting.
    uint32_t getMclkHz(void) override { return 0; }
    uint32_t getClockSampleRateHz(void) override { return uac_usb_sample_rate(); }

    // A USB source has no analogue input to trim. The level is whatever the
    // device sends; the AGC takes care of the rest.
    bool  setInputGainTrimDb(float trimDb) override { (void)trimDb; return false; }
    float getInputGainTrimDb(void) override { return 0.0f; }
};

#endif // USERMOD_USBAUDIO_USB
