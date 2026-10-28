  #pragma once

/* 
   @title     MoonModules WLED - audioreactive usermod
   @file      audio_reactive.h
   @repo      https://github.com/MoonModules/WLED, submit changes to this file as PRs to MoonModules/WLED
   @Authors   https://github.com/MoonModules/WLED/commits/mdev/
   @Copyright © 2024 Github MoonModules Commit Authors (contact moonmodules@icloud.com for details)
   @license   Licensed under the EUPL-1.2 or later

*/

#include "wled.h"
#include <AsyncUDP.h>
// #include "esp_dsp.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"
#include "dsps_fft4r.h"
#include "dsps_wind_blackman_harris.h"
#include <driver/i2s.h>
#include <driver/adc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <esp_timer.h>
#include <atomic>
// #define SR_STATS
#if !defined(FFTTASK_PRIORITY)
  #define FFTTASK_PRIORITY 6   // above async_tcp and USB Mass Storage
#endif
// #define FFT_MAJORPEAK_HUMAN_EAR // removed guards for this, so always on
#define SR_HIRES_TYPE double  // legacy alias. The sub-bin DFT path that used this is gone entirely - the estimator is Dirichlet interpolation in float; runDCBlocker is float too. Kept for any out-of-tree code that still references it.
// Comment/Uncomment to toggle usb serial debugging
// #define MIC_LOGGER                   // MIC sampling & sound input debugging (serial plotter)
// #define FFT_SAMPLING_LOG             // FFT result debugging
// #define SR_DEBUG                     // generic SR DEBUG messages

#ifdef SR_DEBUG
  #define DEBUGSR_PRINT(x) DEBUGOUT(x)
  #define DEBUGSR_PRINTLN(x) DEBUGOUTLN(x)
  #define DEBUGSR_PRINTF(x...) DEBUGOUTF(x)
#else
  #define DEBUGSR_PRINT(x)
  #define DEBUGSR_PRINTLN(x)
  #define DEBUGSR_PRINTF(x...)
#endif

#if defined(SR_DEBUG)
#define ERRORSR_PRINT(x) DEBUGSR_PRINT(x)
#define ERRORSR_PRINTLN(x) DEBUGSR_PRINTLN(x)
#define ERRORSR_PRINTF(x...) DEBUGSR_PRINTF(x)
#else
#if defined(WLED_DEBUG)
#define ERRORSR_PRINT(x) DEBUG_PRINT(x)
#define ERRORSR_PRINTLN(x) DEBUG_PRINTLN(x)
#define ERRORSR_PRINTF(x...) DEBUG_PRINTF(x)
#else
  #define ERRORSR_PRINT(x)
  #define ERRORSR_PRINTLN(x)
  #define ERRORSR_PRINTF(x...)
#endif
#endif

#if defined(MIC_LOGGER) || defined(FFT_SAMPLING_LOG)
  #define PLOT_PRINT(x) DEBUGOUT(x)
  #define PLOT_PRINTLN(x) DEBUGOUTLN(x)
  #define PLOT_PRINTF(x...) DEBUGOUTF(x)
  #define PLOT_FLUSH() DEBUGOUTFlush()
#else
  #define PLOT_PRINT(x)
  #define PLOT_PRINTLN(x)
  #define PLOT_PRINTF(x...)
  #define PLOT_FLUSH()
#endif

// sanity checks
// we need more space in for oappend() stack buffer -> SETTINGS_STACK_BUF_SIZE and CONFIG_ASYNC_TCP_TASK_STACK_SIZE
#if SETTINGS_STACK_BUF_SIZE < 3904    // 3904 is required for WLEDMM-0.14.0-b28
  #warning please increase SETTINGS_STACK_BUF_SIZE >= 3904
#endif
#if (CONFIG_ASYNC_TCP_TASK_STACK_SIZE - SETTINGS_STACK_BUF_SIZE) < 4352 // at least 4096+256 words of free task stack is needed by async_tcp alone
  #ifndef CONFIG_IDF_TARGET_ESP32P4 /// TroyHacks: Yeah, we got the PSRAM.
    #error remaining async_tcp stack will be too low - please increase CONFIG_ASYNC_TCP_TASK_STACK_SIZE
  #endif
#endif


// audiosync constants
#define AUDIOSYNC_NONE 0x00      // UDP sound sync off
#define AUDIOSYNC_SEND 0x01      // UDP sound sync - send mode
#define AUDIOSYNC_REC  0x02      // UDP sound sync - receiver mode
#define AUDIOSYNC_REC_PLUS 0x06  // UDP sound sync - receiver + local mode (uses local input if no receiving udp sound)
#define AUDIOSYNC_IDLE_MS  2500  // timeout for "receiver idle" (milliseconds) 

static volatile bool disableSoundProcessing = false;      // if true, sound processing (FFT, filters, AGC) will be suspended. "volatile" as its shared between tasks.
static uint8_t audioSyncEnabled = AUDIOSYNC_NONE;         // bit field: bit 0 - send, bit 1 - receive, bit 2 - use local if not receiving
static bool audioSyncSequence = true;                     // if true, the receiver will drop out-of-sequence packets
static bool udpSyncConnected = false;         // UDP connection status -> true if connected to multicast group

#ifndef NUM_GEQ_CHANNELS                                               // number of frequency channels. Don't change !! (see fcn_declare.h)
#define NUM_GEQ_CHANNELS 16
#endif

// audioreactive variables

static float    sampleAvg = 0.0f;               // the published level, 0..255. < 1 means "quiet" (the simple noise gate the effects use)
static float    sampleAgc = 0.0f;               // the same value, published under the name the effects read

static float    volumeSmth = 0.0f;              // the smoothed sample level
static float FFT_MajorPeak = 1.0f;              // FFT: strongest (peak) frequency
static float FFT_Magnitude = 0.0f;              // FFT: volume (magnitude) of peak frequency
static bool samplePeak = false;      // Boolean flag for peak - used in effects. Responding routine may reset this flag. Auto-reset after strip.getMinShowDelay()
static bool udpSamplePeak = false;   // Boolean flag for peak. Set at the same time as samplePeak, but reset by transmitAudioData
static unsigned long timeOfPeak = 0; // time of last sample peak detection.
volatile bool haveNewFFTResult = false; // flag to directly inform UDP sound sender when new FFT results are available (to reduce latency). Flag is reset at next UDP send

// The published band values.
//
// fftResult[] is written by the FFT task (core 1) and read by the LED effects
// (core 0). A reader that copies the 16 bytes while the writer is halfway
// through them gets a mix of two frames - a torn read.
//
// The fix is a double buffer with a pointer swap. The writer fills the BACK
// buffer and then publishes it by swapping the pointer that u_data[2] holds.
// A reader therefore always sees one complete frame: the one before the swap
// or the one after it, never half of either.
//
// The pointer is what um_data->u_data[2] holds, so the effects read it and
// copy the 16 bytes into their own storage. They must copy: a reader that
// kept the pointer across the effect body would still be reading a buffer the
// writer has recycled by the time it comes back to it.
//
// There is more than one writer. The FFT task publishes the analysed bands
// (arPublishBands), and the main loop publishes a frame that came from
// elsewhere - the UDP sound sync receive path, and the dynamics limiter
// (arPublishRaw). They share fftResultIdx, so "pick the back buffer, fill it,
// swap" has to be atomic as a whole: without that, both writers can pick the
// same back buffer and write two different frames into it, and the two swaps
// can cancel each other out.
//
// The critical section is a 16-byte copy plus a pointer store - a few hundred
// nanoseconds - so a spinlock is the right tool: neither task can be blocked
// by the other for longer than that.
static std::atomic<uint8_t> arPublishLock{0};

// RAII guard for the publish critical section.
class ArPublishGuard {
public:
  ArPublishGuard() { while (arPublishLock.exchange(1, std::memory_order_acquire)) { /* spin */ } }
  ~ArPublishGuard() { arPublishLock.store(0, std::memory_order_release); }
  ArPublishGuard(const ArPublishGuard&) = delete;             // not copyable
  ArPublishGuard& operator=(const ArPublishGuard&) = delete;
};

static uint8_t fftResultBuf[2][NUM_GEQ_CHANNELS] = {{0}};  // double buffer: fftResultBuf[fftResultIdx] is the published one
static uint8_t  fftResultIdx = 0;                          // which buffer is published
static uint8_t* fftResult = fftResultBuf[0];               // the published buffer

// The RTA set: 31 third-octave bands, published for the effects that draw an
// analyser (the GEQ PPA effect's RTA mode) and for the serial plot.
//
// The 16 display bands are SIXTEEN OF THESE. They are the same measurement -
// the INTEGRATED power over the band, in units^2, which is the unit a
// hardware analyser's bar shows - and the sixteen are a subset of the
// thirty-one. In that unit white noise rises 3 dB per octave and pink noise
// is flat.
//
// The two sets still get their own buffers, because they are drawn at two
// different resolutions: sixteen bars for the effects, thirty-one for the
// analyser. But there is only ONE measurement behind them, and ONE window.
static uint8_t rtaResultBuf[2][NUM_RTA_CHANNELS] = {{0}};  // double buffer, as above
static uint8_t  rtaResultIdx = 0;                          // which buffer is published
static uint8_t* rtaResult = rtaResultBuf[0];               // the published buffer

// The usermod's um_data_t, so that the FFT task - a free function, with no
// access to the class - can repoint u_data[2] when it swaps buffers. It is
// set once in setup(), next to the allocation.
static um_data_t* arUmData = nullptr;
static float   fftCalc[NUM_GEQ_CHANNELS] = {0.0f}; // Try and normalize fftBin values to a max of 4096, so that 4096/16 = 256. (also used by dynamics limiter)
static float   fftAvg[NUM_GEQ_CHANNELS] = {0.0f};  // Calculated frequency channel results, with smoothing (used if dynamics limiter is ON)

static uint16_t zeroCrossingCount = 0; // number of zero crossings in the current batch of 512 samples

// TODO: probably best not used by receive nodes
static float agcSensitivity = 128;            // the input's headroom below full scale, 0..255. calculated by getSensitivity().

// user settable parameters for limitSoundDynamics()
#ifdef UM_AUDIOREACTIVE_DYNAMICS_LIMITER_OFF
static bool limiterOn = false;                 // bool: enable / disable dynamics limiter
#else
static bool limiterOn = true;
#endif
static uint16_t attackTime = 24;              // int: attack time in milliseconds. Default 0.024sec
static uint16_t decayTime = 250;              // int: decay time in milliseconds.  New default 250ms.

// peak detection
static void detectSamplePeak(void);  // peak detection function (needs scaled FFT results in vReal[]) - no used for 8266 receive-only mode
static void autoResetPeak(void);     // peak auto-reset function
static uint8_t maxVol = 31;          // (was 10) Reasonable value for constant volume for 'peak detector', as it won't always trigger  (deprecated)
static uint8_t binNum = 8;           // Used to select the bin for FFT based beat detection  (deprecated)

// #ifdef ARDUINO_ARCH_ESP32

// use audio source class (ESP32 specific)
#include "audio_source.h"
constexpr int BLOCK_SIZE = 128;                  // I2S buffer size (samples)

// globals
static uint8_t inputLevel = 128;              // UI slider value

// The squelch's DEFAULT.
//
// It used to be a plain #ifndef pair, so that the build's -D SR_SQUELCH=x
// chose the value and the settings page only printed the default when that
// -D was there. On a board built without it, the page showed the squelch as
// a bare number with no explanation at all - and "squelch" without a word
// about what it is relative to reads like a floor.
//
// The default is now always defined, so the page can always say what the
// number means.
#ifndef SR_SQUELCH
  #define SR_SQUELCH 10
#endif
uint8_t soundSquelch = SR_SQUELCH;            // the margin above the noise floor, dB (config value)
// sampleGain is GONE. It was the "config:gain" setting: it was written, read,
// and then never applied to anything. The key is removed from a stored config
// by readFromConfig().

// The legacy AGC.
//
// The old usermod had an "AGC" setting with three presets (normal / vivid /
// lazy), each one a table of PI gains and setpoints driving a bang-bang
// integrator. The new AGC is a single dB offset applied to the band levels,
// with a fixed attack and release, and it is always on - there is nothing left
// for a preset to select. The tables are gone, and so is the setting: the
// "AGC" key is removed from a stored config by readFromConfig().

static AudioSource *audioSource = nullptr;
static uint8_t useInputFilter = 0;                        // enables low-cut filtering. Applies before FFT.

// The chain is FIXED: samples -> DC blocker -> input LPF -> FFT.
//
// Both stages are always in the signal path, and the calibration is built
// from the chain as it actually runs. There is no runtime switch: a switch
// would leave the calibration describing a filter that is no longer there,
// which is exactly the failure the old "rebuild the profile on every switch"
// dance was working around. The self-test ('t') measures the chain directly,
// which is a better answer than a toggle ever was.
static constexpr bool dcBlockerEnabled = true;            // DC blocker in the chain?
static constexpr bool inputLpfEnabled   = true;            // input LPF in the chain?

// Is the DC blocker's response part of the chain being inverted?
static inline bool dcBlockerInChain() {
  return (useInputFilter > 0) && (useInputFilter < 99) && dcBlockerEnabled;
}

// Is the input LPF's response part of the chain being inverted?
// The LPF is only APPLIED inside the useInputFilter block, so with
// useInputFilter == 0 the profile used to invert a filter that was not in the
// signal path at all.
static inline bool inputLpfInChain() {
  return (useInputFilter > 0) && (useInputFilter < 99) && inputLpfEnabled;
}

// Which filters the LIVE chain runs.
//
// Three places have to agree on them: the FFT task, which sets them on the
// live chain every frame; the filter bank's calibration, which divides the
// chain's response out of what the bank measures; and the bank's self-test,
// which runs the chain. If they ever disagree, the measurement describes a
// filter that is not the one running.
//
// They differ from dcBlockerInChain() in one case: the PDM path
// (useInputFilter == 1) applies the input LPF but not the DC blocker, while
// dcBlockerInChain() assumes the DC blocker is in for any non-zero setting.
// The bank's own numbers use THESE, because they have to describe the chain
// that actually runs.
static inline bool liveChainDcOn()  { return (useInputFilter == 2) && dcBlockerEnabled; }
static inline bool liveChainLpfOn() { return inputLpfEnabled; }

//WLEDMM add experimental settings
//
// micLevelMethod is DEPRECATED. It chose between a floating level, a frozen
// level and a fast-frozen level. All three are subsumed by the new floor
// tracker, which reads an absolute level and a floor as two independent
// numbers - see arTrackLevel(). It is gone from the config.
//
// freqDist is DEPRECATED. It chose between the old right-shifted bin layout
// and the log (psychoacoustic) layout. The layout is now fixed: 16 constant-Q
// display bands and a 31-band 1/3-octave RTA set, both derived from the
// sample rate. It is gone from the config.
//
// micQuality is DEPRECATED too: it selected how much input filtering to
// apply, and the input chain is now fixed and calibrated (see InputChain and
// estInitCalibration()), so there is nothing left for it to select.
//
// All three keys are removed from a stored config by readFromConfig().

static uint8_t doSlidingFFT = 1;                            // 1 = use sliding window FFT (faster & more accurate)

// The noise gate.
//
// There is no threshold constant here any more. The gate is a comparison
// between the broadband LEVEL and the noise FLOOR, both in dB, and it is
// decided in the FFT task from the frame that is being processed - see
// arTrackLevel().
//
// The note that used to sit here - "DISABLING THE GATE (0.0f) WAS TESTED AND
// RULED OUT - a 5120 Hz tone still read nothing with the gate forced open" -
// was not a valid test: the gate test was
// `fabsf(volumeSmth) > NOISE_GATE_THRESHOLD`, so setting the threshold to
// 0.0f left the test as `fabsf(volumeSmth) > 0.0f`, which is FALSE when
// volumeSmth is exactly 0.0 - and exactly 0.0 is what the squelch produced
// when it closed. Forcing the threshold to zero could not have opened the
// gate; it could only have left it shut.
//
// What actually gated a sustained tone off was the noise-floor tracker
// climbing onto the tone - see the comment on micLev in getSample(), which
// no longer exists.

// shared vars for debugging
#ifdef MIC_LOGGER
static volatile float    micReal_min = 0.0f;             // MicIn data min from last batch of samples
static volatile float    micReal_avg = 0.0f;             // MicIn data average (from last batch of samples)
static volatile float    micReal_max = 0.0f;             // MicIn data max from last batch of samples
#if 0
static volatile float    micReal_min2 = 0.0f;             // MicIn data min after filtering
static volatile float    micReal_max2 = 0.0f;             // MicIn data max after filtering
#endif
#endif

////////////////////
// Begin FFT Code //
////////////////////

// some prototypes, to ensure consistent interfaces
static float mapf(float x, float in_min, float in_max, float out_min, float out_max); // map function for float
void FFTcode(void * parameter);             // audio processing task: read samples, run FFT, fill GEQ channels from FFT results
static void runMicFilter(uint16_t numSamples, float *sampleBuffer);          // pre-filtering of raw samples (band-pass)


static TaskHandle_t FFT_Task = nullptr;

// The hand-measured frequency profiles are GONE.
//
// fftResultPink[][] held one row of per-band gains per microphone (generic
// line-in, IMNP441, ICS-43434, SPM1423, two user tables, a flat one) and was
// selected by the "frequency:profile" config key. The rows were multiplication
// factors applied to the old estimator's magnitudes.
//
// They are not carried over, for two reasons:
//
//   - a multiplication factor on a magnitude is a gain, and a gain changes the
//     measurement. The new path reports a calibrated PSD in dB, and a band
//     value that has been multiplied by a table is no longer a measurement.
//   - the tables were fitted to the OLD estimator's scale, which was not a
//     PSD at all (see the note on the deleted fftBandEnergy()). Re-expressing
//     them against the new unit would be a guess dressed up as a measurement.
//
// The "Auto" profile - the one the old code derived at runtime from the input
// chain - is what the new path does for EVERY band, always, because the chain
// is measured rather than modelled and the correction is additive in dB. See
// estInitCalibration().
//
// The profile key is still read from the config so a stored config loads
// cleanly, and then ignored. Re-measuring a microphone against the new path
// is a separate job.

// globals and FFT Output variables shared with animations
static float FFT_MajPeakSmth = 1.0f;            // FFT: (peak) frequency, smooth
#if 1 || defined(WLED_DEBUG) || defined(SR_DEBUG) || defined(SR_STATS)
static float fftTaskCycle = 0;      // avg cycle time for FFT task
// The fastest cycle the FFT task has EVER run, in TIMING_UNIT_US units.
//
// The smoothed cycle time above settles back to the frame period as soon as
// the disturbance is over, so a task that ran ahead of the audio for a few
// seconds at boot leaves no trace in it. This keeps the worst case, because
// "the task went round 340 times a second instead of 93.75" is a fact about
// the system, not about the moment the info page was rendered.
static float fftTaskMinCycle = 0;
// The longest cycle the FFT task has EVER run, in the same units.
//
// The min says how far ahead of the audio the task has ever got; the max
// says how far behind it has ever fallen. Both are facts about the system
// rather than about the moment the info page was rendered, and between them
// they bracket the frame period - which is what a healthy task looks like.
static float fftTaskMaxCycle = 0;
// How many frames have overrun their deadline, and how many frames in total.
//
// This is NOT an alarm on its own, and it is worth saying why, because 41% of
// frames overrunning is the NORMAL state of a task that is paced by its
// audio: the read blocks until the hop arrives, which is one frame period
// after the frame started, and the work then takes a couple of milliseconds
// more - so the frame ends just after the deadline roughly half the time.
// What matters is the DMA surplus (see arHealth), because that is the
// quantity that decides whether samples are dropped.
static uint32_t arFramesTotal = 0;
static uint32_t arFramesOverran = 0;
static float fftTime = 0;           // avg time for single FFT
static float sampleTime = 0;        // avg (blocked) time for reading I2S samples
static float filterTime = 0;        // avg time for filtering I2S samples
// fftTime, sampleTime, filterTime, fftTaskCycle and the per-stage timers below
// are all stored in units of 10 us: esp_timer_get_time() counts microseconds and the
// capture divides by 10 before smoothing. So 2490 means 24.90 ms, not 2.49 ms.
// This is not cosmetic - the info page used to hand these raw values to a
// formatter that assumed microseconds, under-reporting every figure 10x and
// hiding a 25 ms frame behind a reassuring "2.49 ms". TIMING_UNIT_US exists so
// the conversion happens in exactly one place; do not reintroduce raw values.
constexpr float TIMING_UNIT_US = 10.0f;

// The smoothing the cycle has always used: a two-sample average, then a
// 0.4 EMA.
//
// It lives in a function because the cycle, the sum of its parts and the
// residual between them all have to go through the SAME filter. Filtered
// differently, their differences are artefacts of the filtering rather than
// facts about the frame - which is exactly what happened when the cycle was
// compared against the sum of the separately-smoothed parts.
//
// The arithmetic is done in float rather than in the integer units the
// original used, so that the filter stays LINEAR: with the residual defined
// as cycle minus parts, a linear filter gives back exactly the residual,
// and cycle = parts + residual holds to the last bit.
static float arCycleSmooth(float prev, float nowUnits, float lastUnits) {
  return (((nowUnits + lastUnits)/2.0f) *4.0f + prev*6.0f)/10.0f;
}

// Breakdown of fftTime. fftTime was a single ~25 ms number with no way to tell
// which of the five things inside it was responsible, and that answer is what
// decides whether samplesFFT can stay at 2048. Same units as fftTime.
static float tWindow  = 0;          // memcpy + max-sample + zero-crossing + windowing
static float tPrep = 0;          // frame prep - the sub-bin DFT that used to be timed here is gone
static float tFft     = 0;          // dsps_fft4r_fc32 - the hardware transform itself
static float tPost    = 0;          // bitrev + cplx2real + magnitude fold + peak scan + pink
static float tFinal   = 0;          // band mapping + the publish step
// The two halves of tPost, measured separately.
//
// tPost was a single number covering four quite different things, and the
// bit-reversal permutation is the one of them that is worth arguing about: it
// moves 16 KB of the spectrum into a different order so that everything
// downstream can read it in order.  If it is a large part of tPost, it is
// worth replacing with a lookup; if it is not, it is not worth touching.  So
// measure it rather than guess.
static float tBitrev  = 0;          // dsps_bit_rev4r_fc32 - the permutation itself
static float tEst     = 0;          // estComputeTransform - the estimator's reads of the spectrum

// The rest of the cycle.
//
// The numbers above - the read, the filter, the FFT region and the cycle
// itself - do NOT tile the cycle: between them sit the peak detection, the
// publish, and the two waits that pace the frame. Those are what the cycle
// time is actually made of, and the cycle time is the rate at which the task
// consumes the DMA ring, so an unaccounted millisecond in here is a
// millisecond of audio-clock error. Same units as fftTime.
static float tTail     = 0;         // peak detection + publish, after the FFT region
static float tFloor    = 0;         // the one-tick floor under the deadline wait
static float tWait     = 0;         // the deadline wait that paces the frame
static float tLoop     = 0;         // loop entry to the frame's timestamp
// The sum of the parts, and what is left of the cycle once they are taken out.
//
// These two are computed from the RAW per-frame microsecond counts, not from
// the smoothed ones above. The cycle is smoothed with a 0.4 EMA over a
// two-sample average while the parts are each smoothed with a 0.3 EMA, and
// the two filters have different gains on the frame-to-frame jitter - so
// adding up the smoothed parts and comparing them with the smoothed cycle
// compares two different numbers, and the difference is an artefact of the
// smoothing rather than time that went missing. Computing the sum and the
// residual per frame and smoothing THOSE makes the comparison exact by
// construction.
static float tParts    = 0;         // the cycle's parts, summed, smoothed like the cycle
static float tResidual = 0;         // cycle minus parts, per frame, smoothed

// The init's bill, in milliseconds.
//
// The same numbers are printed to the serial log at boot, but that log is
// not a reliable place to read a measurement from: the boot spews two
// self-tests' worth of lines, the USB CDC drops bytes when the CPU is busy,
// and the line that matters - the last one - is the one most likely to be
// lost. These are kept so the info page can show them for as long as the
// device is up.
static float arInitBuffersMs = 0;   // allocation of the FFT buffers and the window
static float arInitCalMs     = 0;   // measuring the chain and building the calibration
static float arInitChainMs   = 0;   // the chain self-test
static float arInitEstMs     = 0;   // the estimator self-test
static float arInitTotalMs   = 0;   // all of it

// The boot-time self-tests are OFF.
//
// There are four of them - the chain's, the estimator's, the bottom bank's
// and the RTA top bank's - and they cost about 340 ms of boot time before
// the banks' own two, which is time the display waits for nothing. They are
// diagnostics: they report on the code, they do not change it, and the code
// they check has not changed since they last passed.
//
// The tests are still there and still run on demand: the serial key 't' runs
// the chain's self-test, and the info page still publishes whatever the last
// run found.
//
// This is a runtime switch on purpose. A build flag would force a full
// rebuild every time it is changed; this one line is in a header, so it is
// an incremental build.
//
// The banks' health flags come from these tests, so with the tests off the
// banks are simply trusted - which is the point: they are the measurement,
// and the transform is the fallback.
static bool arBootSelfTests = false;
#endif

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
// audio source parameters and constant
constexpr SRate_t SAMPLE_RATE = 48000;        // Base sample rate in Hz
//constexpr SRate_t SAMPLE_RATE = 22050;        // 22.05 kHz - the old default
//constexpr SRate_t SAMPLE_RATE = 16000;        // 16kHz - use if FFTtask takes more than 20ms. Physical sample time -> 32ms
//constexpr SRate_t SAMPLE_RATE = 20480;        // Base sample rate in Hz - 20Khz is experimental.    Physical sample time -> 25ms
//constexpr SRate_t SAMPLE_RATE = 10240;        // Base sample rate in Hz - previous default.         Physical sample time -> 50ms
// The task's wake period was a hand-set FFT_MIN_CYCLE (21 ms at 22.05 kHz,
// 25 ms on the -C3). That number was really an estimate of how long the
// transform took, back when the transform was software and the rate was
// 16 kHz - it was not a rate anybody chose. It is now derived from the frame
// period: see FFT_TASK_PERIOD_TICKS below.
//
//#define FFT_MIN_CYCLE 30                      // Use with 16Khz sampling
//#define FFT_MIN_CYCLE 23                      // minimum time before FFT task is repeated. Use with 20Khz sampling
//#define FFT_MIN_CYCLE 46                      // minimum time before FFT task is repeated. Use with 10Khz sampling
#else
// slightly lower the sampling rate for -C3, to improve stability
//constexpr SRate_t SAMPLE_RATE = 20480;        // 20Khz; Physical sample time -> 25ms
constexpr SRate_t SAMPLE_RATE = 18000;          // 18Khz; Physical sample time -> 28ms
// try 16Khz in case your device still lags and responds too slowly.
//constexpr SRate_t SAMPLE_RATE = 16000;        // 16Khz -> Physical sample time -> 32ms
#endif

// The MCLK the codec is fed. The I2S side runs I2S_MCLK_MULTIPLE_256, so
// MCLK = 256 x SAMPLE_RATE - 12.288 MHz at 48 kHz. This is the number the
// ES8311's clock manager has to agree with, and the one that has to be
// checked at boot: it is the one thing in the chain that can silently be
// wrong, because a codec does not report its own clock.
constexpr uint32_t ES8311_MCLK_HZ = (uint32_t)SAMPLE_RATE * 256u;

// How much audio the I2S driver has buffered, in ms. This - not the task's wake
// period - is the real deadline: if the FFT task runs longer than this, the DMA
// ring refills underneath it and samples are dropped. At 48 kHz with 24x128
// descriptors it works out at about 64 ms, so the task has roughly six times
// the headroom rather than the 8 ms the old constant implied. Exceeding the
// wake period is not dangerous, it just makes the update rate slip.
//
// Outside the SAMPLE_RATE branch above on purpose: arHealth() uses it,
// and that function is common to every target, so leaving it inside would break
// the -C3 build on an undefined name.
constexpr float I2S_RING_MS = (AR_I2S_DMA_DESCS * BLOCK_SIZE * 1000.0f) / (float)SAMPLE_RATE;

// Nyquist: the upper bound on the strongest-frequency the FFT can report.
// Used in three FFT_MajorPeak clamps - leaving it as 11025.0f was correct for
// SAMPLE_RATE=22050 but tied the code to that one rate.
constexpr float NYQUIST_HZ = (float)SAMPLE_RATE * 0.5f;

// FFT Constants
// The length used to be pinned at 512, because the sub-bin and binned band paths
// were reconciled by a constant fitted at 512 rather than derived - see the note
// above BAND_F_LOW. That bridge is gone: both paths are now band powers in the
// same units, so the length is a free parameter again. It is 2048 because that is
// what it takes to separate the bottom bands. A tone at band 0's centre leaks
// only -1.2 dB into band 1 with a 23.2 ms window (512) and -31 dB with a 92.9 ms
// one (2048), so at 512 bands 0-2 are not really three measurements. 2048 also
// puts the window past band 0's 59 ms natural width, which is the point past
// which extra length buys resolution nothing - there is no reason to go higher.
// hopSamples is decoupled below, so the update rate is a separate decision.
//
// COST, as measured on ESP32-P4. These are not comparable to the old
// software-FFT numbers that used to sit in this file - that hardware transform
// does not exist here. The whole FFT task (fftTime, everything from windowing
// through postProcessFFTResults) measures 7.8 ms at N=512 and 24.73 ms at
// N=2048: 3.19x for 4x the length, not the 4.89x that N*log N predicts, so the
// P4 transform carries enough fixed overhead that longer transforms are cheaper
// than the textbook figure suggests.
//
// The 24.73 ms had almost nothing to do with the transform, which is worth
// stating because it is the opposite of what the total suggested. Broken down
// by stage: sub-bin DFT 23.10 ms, bands+post 0.72, bitrev+magnitude 0.43,
// ESP-DSP FFT 0.26, windowing 0.21. dsps_fft4r_fc32 dispatches to the on-chip
// hardware transform on this target and a 2048-point one costs 260 us. The cost
// was the direct-DFT integration beside it, 93% of the frame, running
// emulated double-precision arithmetic - now replaced by a precomputed twiddle
// table (see dftTwiddle). That is what pinned core 1 at 95%; 24.73 ms against
// an 11.6 ms frame period means the task never sleeps and the update rate
// collapses to roughly 35 Hz.
//
// Re-measure from the AudioReactive info page if any of this changes; the
// per-stage rows there break the same number down by stage, and the sub-bin DFT
// row is the one to watch.
//
// N = 4096 at 48 kHz: an 85.3 ms window, 11.72 Hz bins. The bottom bands are
// the reason: band 0's centre is 41.9 Hz and band 1's is 60.2 Hz, which are
// 0.8 bins apart at 23.4 Hz/bin (the old 2048 at 22.05 kHz) and 1.6 bins
// apart at 11.72 Hz/bin. 4096 is also the hard ceiling - ESP-DSP ships the
// bit-reversal tables for fft4r up to 4096 points only
// (CONFIG_DSP_MAX_FFT_SIZE).
constexpr uint16_t samplesFFT = 4096;           // Samples in an FFT batch - This value MUST ALWAYS be a power of 2

// Hop: NEW samples consumed per FFT frame, and the retained overlap is whatever
// is left over (retainSamples), so consecutive windows share retainSamples.
//
// The old samplesFFT_2 conflated this with samplesFFT/2, which made the two
// inseparable: raising the transform length to buy frequency resolution silently
// halved the update rate, and there was no way to express the trade. They answer
// different questions - length sets what the analyser can resolve, hop sets how
// often it looks - so they are separate constants now.
//
// 512 is 10.67 ms at 48 kHz, so the DISPLAY is designed to update at 94 Hz
// whatever the transform length is. At N=4096 that is 87.5% overlap, which is a
// lot of redundant work but is the point: the window has to be long to resolve
// the bottom band, and consecutive windows are genuinely near-identical, so the
// display changes by the newest 10.67 ms each frame rather than jumping.
//
// 94 Hz is the design rate, not the achieved one: the task also has to fit its
// own work inside 10.67 ms, and the frame currently takes a few ms, so
// vTaskDelayUntil should sleep between frames. That is what FRAME_PERIOD_MS
// below is for.
//
// The cost of a long window is attack time, not update rate. An 85.3 ms window
// cannot report a kick drum onset faster than it averages it away - the band
// value ramps over ~8 consecutive frames. That is the correct measurement of a
// 42 Hz band, not a bug, but it does mean the lows will read as a sustained
// envelope. If that turns out to be too soft, the fix is a second, short-window
// path for the bottom bands, not a shorter hop.
constexpr uint16_t hopSamples = 512;
constexpr uint16_t retainSamples = samplesFFT - hopSamples;

// Wall-clock time one frame's worth of audio takes to arrive. This - not the
// task's wake period, and not I2S_RING_MS - is the CPU budget the FFT task has
// to meet. Exceed it and the task cannot consume the samples it is handed as
// fast as I2S delivers them, so vTaskDelayUntil falls behind, the update rate
// slips below the design rate, and the DMA ring drifts steadily fuller. It
// does not cost audio on its own (there is ~12x of ring left to absorb the
// drift), which is why the orange tier is a warning and not a failure.
//
// At hopSamples=512 and 48 kHz this is 10.67 ms. It is deliberately derived
// rather than a tunable: it is a property of the hop and the sample rate, and
// the whole point of splitting hopSamples out of samplesFFT was that these two
// numbers answer different questions and must not be conflated again.
constexpr float FRAME_PERIOD_MS = (hopSamples * 1000.0f) / (float)SAMPLE_RATE;

// The FFT task's wake period, in ticks - this is what the old hand-set
// FFT_MIN_CYCLE was for, but derived instead of guessed. The task cannot
// usefully run faster than the audio arrives (a frame's worth of audio is
// FRAME_PERIOD_MS), and running it slower only makes the display lag.
//
// Rounded to whole ticks, because vTaskDelayUntil takes ticks: at 48 kHz with
// a 512-sample hop that is 11 ticks, so the task wakes at 91 Hz against a
// 93.75 Hz frame rate and the DMA ring creeps up by one sample per 24 frames -
// which the ring's 139 ms of headroom absorbs indefinitely.
constexpr uint32_t FFT_TASK_PERIOD_TICKS =
    (uint32_t)((FRAME_PERIOD_MS * (float)portTICK_PERIOD_MS) + 0.5f);

// The Blackman-Harris window used to be a local array in the FFT task, so the
// task stack had to cover it - 8 KB at N=2048, 16 KB at N=4096. It is a
// constant, read-only after init and touched once per frame, so it belongs in
// PSRAM with the other FFT buffers, not on the stack: the stack is the wrong
// place to spend 16 KB. The task prints its own high-water mark at startup, so
// a genuine overflow stays visible.
constexpr uint32_t FFT_TASK_STACK = 32 * 1024;

// Global magnitude scale applied to the spectrum before the bands are computed,
// so the result lands near 4096 at full scale. The sub-bin bands are measured on
// the time-domain signal, which happens BEFORE the FFT overwrites that buffer,
// so they carry the same factor by hand. It is a display scale only - it used to
// double as the bridge between the two band paths, which pinned the whole
// pipeline to one transform length. See the note above BAND_F_LOW.
constexpr float FFT_BIN_SCALE = 1.0f / 16.0f;

// dB display mapping. The window's span is a setting (arWindowRangeDb),
// shared by the 16 bars and the 31 RTA bars; the user's trim on where 0 dB
// lands is a runtime value in wled.h - TROYHACKS_DBREF ('d').
//
// The mapping is applied once, in arPublishBands():
//
//   v = mapf(smoothed_dB + valueAdj, winLo, winHi, 0.0f, 255.0f)
//
// where smoothed_dB is the calibrated band level plus the auto-range's offset,
// valueAdj is the user's trim and the input-level slider read as a dB trim,
// and the window's edges come from the shared window.
//
// The span is the "variance" control and it matters more than it looks. A
// constant-Q band reports power over a width proportional to its centre
// frequency, so for a spectrum of slope 1/f^a the displayed level falls as
// f^(1-a): pink (a=1) reads dead flat, and typical pop/rock (a~1.2) spans only
// about 10 dB across all 16 bands. Laid out over 66 dB that is 16% of the bar
// and reads as a dead line; over 48 dB it is 21% and reads like an RTA.
//
// And it has to hold the MATERIAL, not just the typical case: the music on
// this board measured 25 to 46 dB from the loudest band to the quietest, so a
// 24 dB window put six to ten of the sixteen bars on the floor at all times.

// These are the input and output vectors.  Input vectors receive computed results from FFT.
static float* vReal = nullptr;       // FFT sample inputs / freq output -  these are our raw result bins
static float* vImag = nullptr;       // imaginary parts

// The transform's working buffer: samplesFFT COMPLEX points, interleaved.
//
// The frame is transformed with a plain COMPLEX FFT of samplesFFT points - the
// windowed frame in the real slots, zeros in the imaginary ones - instead of
// ESP-DSP's real-FFT helper, for two reasons. Both are forced by the P4's
// build of ESP-DSP, and both were found by reading the shipped library.
//
// 1. The transform length must be a power of FOUR.
//
//    ESP-DSP's real FFT is a radix-4 transform of HALF the points, and its
//    bit-reversal tables exist only for 16, 64, 256, 1024 and 4096 points -
//    every one of them a power of four. samplesFFT/2 = 2048 is not on that
//    list, and the P4 cannot do it:
//      - dsps_fft4r_fc32_ansi_() rejects it outright (odd log2N);
//      - dsps_bit_rev4r_fc32_ae32() handles 16/64/256/1024/4096 and falls
//        through to the ANSI path for anything else, which rejects 2048 the
//        same way - so the bit reversal silently does nothing;
//      - the P4's arp4 assembly runs one radix-4 stage short for log2N = 11
//        (its group length goes 512, 128, 32, 8, 2 and then 0), so the last
//        radix-2 stage is missing and the spectrum comes out garbage.
//    A complex FFT of samplesFFT = 4096 points is 4^6, so both the transform
//    and the bit reversal are supported. (The old code used samplesFFT =
//    2048, i.e. 1024 complex points, which is why it worked.)
//
// 2. The unpack's twiddle is wrong by a factor of two.
//
//    The twiddle table is filled with cos/sin pairs at the angles
//    2*pi*i/table_size, i running over the PAIRS, so the pair at index m
//    carries the angle 2*pi*m/table_size. dsps_cplx2real_fc32() reads its
//    twiddle at pair index k*wind_step with wind_step = table_size/N, i.e.
//    the angle 2*pi*k/N. The real-FFT unpack needs e^(-j*pi*k/N) - half of
//    that - so the unpacked spectrum is not the frame's real spectrum.
//    Checked against a direct DFT: the even/odd packing with the half-step
//    twiddle reproduces the reference to 1e-13, the full-step one does not.
//
// The complex FFT sidesteps the unpack entirely: its output IS the frame's
// spectrum, in natural bin order, with the phase the interpolation needs.
//
// The transform itself is unchanged in kind: dsps_fft4r_fc32() on a complex
// sequence of N points is a radix-4 complex FFT, and the twiddle it reads is
// the one a complex DFT of N points wants. The bit-reversal that follows it
// puts the bins in natural order.
static float* fftBuf = nullptr;

// The Blackman-Harris window. Read-only after init, touched once per frame, so
// it lives in PSRAM beside the other FFT buffers rather than on the task stack.
static float* windowBuf = nullptr;

// ESP-DSP's radix-4 twiddle table: four times max_fft_size floats.
//
// The library allocates this itself, in PSRAM, when it is asked to initialise
// with a NULL buffer. It is asked for with a buffer of our own instead, so
// that the placement is ours to decide - see arAllocHotBuf() for why.
static float* fftTwiddle = nullptr;

// Where the transform's working set lives.
//
// The P4's PSRAM is shared with the display. The DSI controller streams the
// frame buffer out of PSRAM at the panel's refresh rate, and the USB mass
// storage cache writes into it too. The FFT task's own traffic is BURSTY: a
// 4096-point transform touches most of its working set inside about a
// millisecond, and a burst that takes most of PSRAM's bandwidth for a
// millisecond is enough to starve the DSI, which then reports an underrun and
// drops a frame. On screen that reads as a display that is not quite keeping
// up - the picture updates, but later than it should.
//
// So the two buffers the transform touches hardest - the frame itself and the
// twiddle table - are the two that could be asked for in internal RAM instead
// of PSRAM.  They are 64 KB between them, which is a lot of what internal RAM
// has to spare on this board, so the choice is worth making explicitly.  With
// AR_HOT_BUFFERS=1 they are asked for in internal RAM first, falling back to
// PSRAM if internal RAM cannot take them.  Everything else stays in PSRAM: its
// traffic is thinner, and spread thinly enough, not to matter.
//
// The placement is reported on the info page, because "which buffer is where"
// has to be checkable rather than assumed.
//
// MALLOC_CAP_INTERNAL means internal RAM and nothing else, so a successful
// allocation from it is in internal RAM by definition - and a failure means
// internal RAM could not take it, which is the case where the buffer has to
// go to PSRAM after all.
//
// The placement is a build-time switch rather than a fixed decision, because
// internal RAM is the scarce kind on this board and other things want it too.
// Build with -D AR_HOT_BUFFERS=1 to put both buffers in internal RAM instead.
//
// The default is 0 - both buffers in PSRAM - because internal RAM on this board
// is worth more to the web server than it is to the transform.  The web server
// refuses a client outright, with no answer at all, when the internal heap is
// under 8 KB or its largest free block is under 2 KB; the browser then shows
// "net::ERR_EMPTY_RESPONSE" and the GUI never loads.  With these two buffers
// in internal RAM the free heap sat at 7.9 KB - under the floor - and five of
// six parallel requests came back empty.
//
// And the transform does not miss the buffers.  Measured A/B on this board, the
// same build twice, medians over eight polls of the running average:
//
//                       internal      PSRAM
//   fft region          2975 us      2799 us
//   esp-dsp fft         1313 us      1319 us
//   windowing            523 us       433 us
//   bit-rev + magnitude  858 us       844 us
//   free internal heap   7 890 B     74 068 B
//
// PSRAM is not slower at all - the transform's cost is identical - and it hands
// back 66 KB of internal RAM, which is the difference between a GUI that loads
// and one that does not.
//
// The DSI underruns that first prompted the internal placement were traced to
// the USB mass storage cache writing into PSRAM at boot, not to the transform,
// so they are not a reason to hold on to internal RAM.
#ifndef AR_HOT_BUFFERS
#define AR_HOT_BUFFERS 0
#endif

static bool arFftBufInternal = false;
static bool arTwiddleInternal = false;

static void* arAllocHotBuf(size_t bytes, const char* name, bool &inInternal) {
  inInternal = false;
  void *p = nullptr;
#if AR_HOT_BUFFERS
  p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL);
  if (p != nullptr) inInternal = true;
#endif
  if (p == nullptr) {
    p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM);
    if (p == nullptr) return nullptr;
  }
  USER_PRINT("AR: "); USER_PRINT(name); USER_PRINT(": ");
  USER_PRINT(bytes); USER_PRINT(" bytes -> ");
  USER_PRINTLN(inInternal ? "internal RAM" : "PSRAM");
  return p;
}


constexpr float pinkcenter = 23.66;                         // sqrt(560) - center freq for scaling is 560 hz.
constexpr float binWidth = SAMPLE_RATE / (float)samplesFFT; // frequency range of each FFT result bin

// ---------------------------------------------------------------------------
// Constant-Q band layout
// ---------------------------------------------------------------------------
// 16 bands, log-spaced from 20 Hz to Nyquist. At 22050/512 that works out to
// 0.569 octave per band - between 1/2 and 1/3 octave, which is as fine as 16
// bands can be over that span. Band edges are the geometric midpoints between
// adjacent centres, so every FFT bin belongs to exactly one band (no gaps, no
// double counting) and every band ends up with the same Q.
//
// A band narrower than one FFT bin cannot be read off the FFT at all: at
// 43.07 Hz/bin a 40 Hz tone and an 80 Hz tone both land in bin 1. Bands 0-4 are
// in that situation at samplesFFT = 512. Those are evaluated by integrating the
// squared magnitude of the DFT across the band instead - see dftPowerAt() and
// captureSubBinBands().
//
// The two paths have to report the SAME quantity, or the display shows a step
// at whichever band happens to sit on the boundary. Both are unnormalised DFTs
// over the same window, so the relationship is exact rather than a matter of
// taste:
//
//   bin path    sqrt( sum over the band's bins of |X_k|^2 )
//   dft path    sqrt( integral over the band of |D(f)|^2 df )
//
// The DFT grid has spacing 2*pi/N in omega, so summing magnitudes squared over
// the whole spectrum approximates the integral of |X|^2 by the Riemann rule
// with step 2*pi/N; restricting to one band does the same, giving
//
//   sum_k |X_k|^2  =  (samplesFFT / SAMPLE_RATE) * integral over band
//
// and therefore, in amplitude, the dft path is scaled by
// sqrt(samplesFFT / SAMPLE_RATE). Measured against the binned path on
// white noise, across 14 bands and three transform lengths, the derived
// constant matched to 0.04 dB (N=512 -16.34 vs -16.38, N=1024 -13.33 vs
// -13.28, N=2048 -10.31 vs -10.31). It is computed in initBandLayout(), so
// it follows SAMPLE_RATE and samplesFFT automatically.
//
// This is what used to be a fitted constant. FFT_BIN_SCALE = 1/16 carried the
// band-width conversion implicitly, which made it correct at exactly one
// samplesFFT and silently wrong at any other - doubling N moved bands between
// the two paths (the sub-bin test is "narrower than 2 bins", which is
// N-relative) and the fitted constant stopped applying, putting bands 0-2
// ~20 dB below bands 3+. Nothing about the measurement depended on N; only the
// bridge did. FFT_BIN_SCALE is now purely the global display scale and bridges
// nothing.
//
// BAND_F_LOW is the lowest band EDGE, not the lowest centre - band 0 spans
// BAND_F_LOW to BAND_F_LOW * r, so it has a finite lower edge instead of running
// down to DC. 35 Hz lines up the bottom band with a typical 1/3-octave RTA's
// "40 Hz" bar (35.5-44.7 Hz): most of the overlap is on AudioReactive's side
// now, so a song with bass centred at 36-38 Hz (typical for bass-heavy mixes
// with a sub-bass shelf) lands inside band 0 instead of being clipped at the
// 40 Hz edge. The trade-off: the DC blocker is now further from band 0's
// centre, so band 0 has slightly more tilt to correct (initChainCalibration
// handles this; the Auto profile evaluates at the actual band centre, not
// at BAND_F_LOW). 40 Hz was the previous value; 35 Hz is what an RTA-aligned
// layout wants.
static constexpr float BAND_F_LOW = 35.0f;                  // lowest band edge, Hz

// The top of the 16-band layout, in Hz. ABSOLUTE, not Nyquist.
//
// The layout is a measurement scale - a 1/2-octave set from 35 Hz to 11 025 Hz
// - and a measurement scale does not move when the sample rate does. It used
// to be SAMPLE_RATE/2, which was the same thing only because the rate used to
// be 22 050 Hz; at 48 kHz, tying the top edge to Nyquist would have spread the
// bands to 24 kHz and moved every one of the 16 centres - silently re-tuning
// every WLED effect that reads these bands, and moving the top band above the
// range where the input chain is calibrated.
//
// So the layout keeps its 11 025 Hz top edge at any rate, and the bins above
// it are simply not used. The plan's second layout (the 31-band ISO 266 set,
// 19.95 Hz - 19 952.6 Hz) is the one that reaches further up.
static constexpr float BAND_F_HIGH = 11025.0f;               // top band edge, Hz

// ---------------------------------------------------------------------------
// Band layouts
// ---------------------------------------------------------------------------
//
// A band layout is a measurement scale: a set of constant-Q bands spanning a
// frequency range. It knows how many bands it has, where their centres are and
// where their edges are, and it knows nothing else - no FFT bins, no display
// scaling, no filter corrections. That is what lets a second layout exist
// beside the first without either one having to know about the other.
//
// The 31 third-octave bands are the scale a hardware RTA shows, and they have
// to be measured from the same audio at the same time to be compared with it
// bar for bar. The 16 bands the effects read are SIXTEEN OF THOSE - the same
// measurement, at two resolutions.
//
// The FFT-domain parts of a band - which bins it owns, whether it is narrower
// than a bin - are deliberately NOT in here. Those describe the 16-band
// layout's relationship with the transform, and they disappear with the
// sub-bin path.

// The largest layout we build: 16 for the display bands, 31 for the RTA set.
constexpr int AR_MAX_BANDS = 32;

struct BandLayout {
  int   count;                      // number of bands
  float fLow, fHigh;                // outer band EDGES of the layout, Hz
  float r;                          // ratio between neighbouring bands
  float centre[AR_MAX_BANDS];       // band centre, Hz
  float edgeLo[AR_MAX_BANDS];       // band lower edge, Hz
  float edgeHi[AR_MAX_BANDS];       // band upper edge, Hz
};

// The old 16 half-octave bands - the TRANSFORM's own instrument, kept because
// the estimator and the calibration are still built on them - and the 31
// third-octave bands the display and the RTA comparison both read. Both are
// built once, at init.
static BandLayout lay16;
static BandLayout lay31;

// A constant-Q layout whose edges sit on a geometric grid: edge k is
// fLow * r^k, and the centres are the geometric midpoints of consecutive
// edges, so every band has the same Q.
//
// The centres come out as fLow * r^(k+0.5), which is exactly what the layout
// has always used. The refactor must not perturb them by a single ULP: every
// WLED effect that reads these bands is tuned to these numbers, and a change
// in the last bit would move every bar on every display for no reason at all.
//
// fLow and fHigh are the layout's outer EDGES - band 0 spans fLow to fLow*r,
// and band count-1 ends exactly on fHigh.
static void layoutGeometric(BandLayout &L, int count, float fLow, float fHigh) {
  L.count = count;
  L.fLow  = fLow;
  L.fHigh = fHigh;
  L.r = powf(fHigh / fLow, 1.0f / (float)count);
  for (int k = 0; k < count; k++) {
    L.centre[k] = fLow * powf(L.r, (float)k + 0.5f);
    L.edgeLo[k] = fLow * powf(L.r, (float)k);
    L.edgeHi[k] = fLow * powf(L.r, (float)k + 1.0f);
  }
}

// A layout whose CENTRES are given and whose edges are derived as
// centre * 2^(±1/6).
//
// This is the ISO 266 third-octave series: 31 bands on the preferred numbers
// 19.95 Hz to 19952.6 Hz, each a third of an octave wide. The centres are the
// numbers an RTA is labelled with, so they are the ones that have to be
// right - and they are NOT a geometric grid. The preferred series steps by
// 10^(1/10) = 1.258925 while a true third of an octave is 2^(1/3) = 1.259921,
// a 0.08% difference that accumulates to 2.4% across the 30 steps of the set.
// Spelling the centres out is what keeps them exact.
//
// The edges are the standard ±1/6-octave ones. Adjacent bands therefore leave
// a 0.08% gap between one band's upper edge and the next band's lower edge -
// 0.016 Hz at the bottom of the range, 16 Hz at the top, both far below the
// width of the measurement kernel, so it costs nothing.
static constexpr float ISO_EDGE_DOWN = 0.890898718f;   // 2^(-1/6)
static constexpr float ISO_EDGE_UP   = 1.122462048f;   // 2^(+1/6)

static void layoutISOThirdOctave(BandLayout &L) {
  L.count = NUM_RTA_CHANNELS;
  L.r = powf(10.0f, 0.1f);         // 1.258925 - the preferred-number step
  for (int k = 0; k < L.count; k++) {
    L.centre[k] = powf(10.0f, (float)(13 + k) * 0.1f);   // 19.9526 Hz .. 19952.6 Hz
    L.edgeLo[k] = L.centre[k] * ISO_EDGE_DOWN;
    L.edgeHi[k] = L.centre[k] * ISO_EDGE_UP;
  }
  L.fLow  = L.edgeLo[0];           // 17.78 Hz
  L.fHigh = L.edgeHi[L.count - 1]; // 22396 Hz
}

// The sixteen display bands are sixteen of the thirty-one RTA bands.
//
// The RTA set is the MEASUREMENT - the integrated power over a third-octave
// band - and the sixteen bars the effects read are a SUBSET of it, on the same
// scale and in the same unit. The two views are then literally the same
// spectrum at two resolutions: a bar of the 16 is the same bar of the 31, at
// the same height, in the same window.
//
// The selection is the nearest third-octave centre to each of the old
// half-octave display bands, so the bar-to-frequency mapping stays where it
// was and the top of the range still reaches 10 kHz:
//
//   bar   old centre   RTA band   new centre   error
//     0     41.89         3         39.81      -4.97 %
//     1     60.02         5         63.10      +5.13 %
//     2     85.99         6         79.43      -7.62 %
//     3    123.19         8        125.89      +2.19 %
//     4    176.49         9        158.49     -10.18 %
//     5    252.85        11        251.19      -0.66 %
//     6    362.25        13        398.11      +9.91 %
//     7    518.98        14        501.19      -3.43 %
//     8    743.52        16        794.33      +6.83 %
//     9   1065.22        17        1000.00      -6.12 %
//    10   1526.10        19        1584.89      +3.85 %
//    11   2186.39        20        1995.26      -8.74 %
//    12   3132.37        22        3162.28      +0.95 %
//    13   4487.63        24        5011.87     +11.68 %
//    14   6429.27        25        6309.57      -1.86 %
//    15   9210.99        27       10000.00      +8.56 %
//
// The four lowest bars come out of the bottom bank - the FFT's 23.49 Hz
// kernel cannot separate a third-octave band at 40 Hz - and the other twelve
// out of the top bank.
static const uint8_t arGeqFromRta[NUM_GEQ_CHANNELS] = {
   3,  5,  6,  8,  9, 11, 13, 14, 16, 17, 19, 20, 22, 24, 25, 27
};

// The display band k's centre: it is the RTA band it was taken from.
//
// The layout has to be built before this can be asked, so every caller is one
// that runs after initBandLayout().
static inline float arGeqCentre(int k) {
  return lay31.centre[arGeqFromRta[k]];
}

// Forward declarations: the estimator's self-test below needs both of them,
// and they are defined further down the file. bandLayoutReady is zero
// initialised here and set by initBandLayout().
//
// VOLATILE, and so are the other flags that cross a task boundary. The
// analysis runs on the FFT task; the serial console and the JSON handler run
// on the main loop; the endpoint reads what the analysis decided. A plain
// bool gives the compiler permission to keep the value in a register across
// the frame, so the reader can be a whole frame behind - and the flags below
// are the ones that say what is RUNNING ("the bank drives", "the transform
// has a valid reading", "the self-test passed"). A flag that says what is
// running must not be a frame out of date.
//
// The floats are not marked volatile: a 32-bit float load and store are
// atomic on this core, so a trim or an offset is seen whole, just possibly
// one frame late - and late by one frame is the right answer for a setting
// the user just changed.
static volatile bool bandLayoutReady;
static void initBandLayout();

// ---------------------------------------------------------------------------
// The estimator: Dirichlet interpolation of the FFT bins
// ---------------------------------------------------------------------------
// The DTFT of a length-N frame is exactly the trigonometric interpolation of
// its N-point DFT, so the spectrum at any frequency can be evaluated from the
// bins around it:
//
//   X(f) = (1/N) * sum_k X[k] * G_N(2*pi*(k-u)/N),      u = f/df
//
// G_N is the unnormalised Dirichlet kernel. Truncating that sum to the bins
// around the nearest one is the approximation; the weights are baked once, at
// init, in double precision.
//
// In the numerically stable form
//
//   G_N(2*pi*(k-u)/N) = e^{j*pi*(k-u)*(1-1/N)} * sin(pi*(k-u))/sin(pi*(k-u)/N)
//
// the ratio has a removable singularity at k == u, where its limit is N: the
// estimator then reads that DFT bin exactly, which is right - the DTFT at a
// bin frequency IS that bin.
//
// Every value this produces is a ONE-SIDED power spectral density:
//
//   S1(f) = 2 * |X_w(f)|^2 / (fs * sum(w^2))            [units^2/Hz]
//
// so white noise of variance sigma^2 reads 2*sigma^2/fs at every frequency,
// pink noise falls 3 dB/octave, and a tone of power P reads P/ENBW.
//
// Measured against a direct DTFT of the same frame: 0.0000 dB at every band
// centre of both layouts, and at every +/-1 bin offset from it.

// How many bins either side of the nearest one the kernel reaches for.
//
// m = 6 was the plan's first guess and it is not enough. The lowest band
// centre (41.89 Hz) sits 3.58 bins from DC and the kernel's tail reaches the
// DC-adjacent bins, so the truncated sum loses 0.1332 dB there - a third of
// the whole flat-input budget, and it lands on the band the old code was
// already weakest in. m = 12 measures 0.0000 dB at every probe.
static constexpr int AR_KERNEL_HALF = 12;                      // bins either side
static constexpr int AR_KERNEL_TAPS = 2 * AR_KERNEL_HALF + 1;  // 25

// Slots: the 16 display bands, then the 31 RTA bands.
static constexpr int AR_EST_SLOTS = 48;

static float estWeight[AR_EST_SLOTS][AR_KERNEL_TAPS * 2];  // complex, interleaved
static int   estK0[AR_EST_SLOTS];                          // nearest bin to the centre
static float estNorm = 0.0f;                               // 1/(fs * sum(w^2))
static float estPoint[AR_EST_SLOTS];                       // point PSD at the centre
static float estPeak[NUM_GEQ_CHANNELS];                    // peak PSD over the band
static float estComp[NUM_GEQ_CHANNELS];                    // crest compensation, dB
static float estRta[AR_MAX_BANDS];                         // integrated band power
static float arRtaDbPub[AR_MAX_BANDS];                     // the published copy of estRtaDb[]
static float estBandInt[NUM_GEQ_CHANNELS];                 // integrated power over a display band
static int   estLo[NUM_GEQ_CHANNELS];                      // first bin of the band
static int   estHi[NUM_GEQ_CHANNELS];                      // last bin of the band

// The calibration.
//
// The input chain is a filter, and everything downstream of it measures what
// came OUT of the chain. To say anything about the signal that went IN, the
// chain's own response has to be divided out - once, here, and not again in
// every consumer.
//
//   cal_dB[k] = -20*log10(|H(f_c)|) + T(f_c)
//
// with T the target curve: 0 dB, so a flat input reads flat (the default), or
// +10*log10(f/1000), so a pink input reads flat. (A pink input's own PSD
// falls 3 dB per octave, so the target has to ADD that slope back to cancel
// it - see the correction below.)
//
// |H| is MEASURED, not modelled: a unit impulse goes through a fresh
// InputChain - the same code, the same coefficients, the same state machine -
// and the impulse response is transformed at the band centres. The analytic
// model of the chain is what the chain self-test compares the measurement
// against; using the measurement here means the calibration cannot drift away
// from what the chain actually does.
//
// The correction is clamped: a band whose chain response is more than
// CAL_CLAMP_DB down would be lifted by more than that, and what it would be
// lifting is the noise floor. The count of clamped bands is published.
static constexpr float CAL_CLAMP_DB = 12.0f;                // no more than this much boost
static constexpr float CAL_PINK_REF_HZ = 1000.0f;           // the pink target's reference
static float calDb[AR_EST_SLOTS];                           // the correction, dB
static float calChainDb[AR_EST_SLOTS];                      // the chain's response, dB
static int   calClamps = 0;                                 // bands that hit the clamp
static volatile bool  calReady = false;
static volatile bool  calTargetPink = false;                // T(f): flat (default) or pink

// Set when the calibration's target curve changes at runtime. The FFT task
// rebuilds the calibration and clears the flag: the rebuild measures an
// impulse response and writes the calibration's arrays, and the FFT task is
// the only thread that may do either.
static volatile bool arCalDirty = false;

// ---------------------------------------------------------------------------
// The RTA filter bank: the bottom eight bands.
//
// The FFT cannot separate the RTA's bottom bands. Its measurement kernel is
// 23.49 Hz wide and the narrowest band in the ISO 266 set is 4.62 Hz wide, so
// the bottom bars all read the same smeared measurement - five bars, one
// number. A filter bank can separate them, because each band gets its own
// filter with its own bandwidth.
//
// The narrow path tried this with a second, longer FFT and failed on the
// UPDATE RATE: the transform only ran once 4096 DECIMATED samples had
// accumulated, which is once every 682.7 ms. The bars were not slow, they
// were FROZEN for two-thirds of a second at a time - 1.47 updates a second.
// The R in RTA stands for realtime.
//
// The bank has no such gate. Its filters run continuously, every frame adds
// its new samples to them, and every band is recomputed EVERY FRAME. The
// filter's shape sets the resolution and the detector's time constant sets
// the variance; neither of them sets the update rate.
//
// This is what a hardware analyser is: one bandpass per band, running, with
// the bar showing the running power of its output.
//
// The measurement is the SHAPED power - the bandpass's output power, not the
// power inside the band's nominal edges. That is what an analyser shows, and
// it is the only form in which these bands can be compared with one bar for
// bar. It also gives the cleanest possible self-test contract: a tone at the
// band's own centre reads exactly its own power.
//
// The price is variance. A 125 ms detector at 20 Hz holds less than one cycle
// of the signal, so the bar carries a few dB of chi-square noise - exactly
// what a real analyser shows, and exactly what the detector's time constant
// is the knob for.
//
// The eight bands are the bottom of the ISO 266 set: 19.95 Hz to 100 Hz.
// ---------------------------------------------------------------------------

// The chain's measurement, forward-declared: the bank calibrates itself from
// the chain's MEASURED response at its own band centres, with the flags the
// LIVE chain runs.
static int  selfTestIrLength();
static void measureChainDb(float *outDb, const float *freqs, int nf, int irLen,
                           bool dcOn, bool lpfOn);

static constexpr int   AR_BANK_BANDS   = 8;      // the bottom 8 ISO 266 bands
static constexpr int   AR_BANK_DEC     = 16;     // 48 kHz -> 3 kHz
static constexpr float AR_BANK_FS      = (float)SAMPLE_RATE / (float)AR_BANK_DEC;
static constexpr float AR_BANK_AA_HZ   = 400.0f; // the anti-alias corner
static constexpr int   AR_BANK_AA_SECS = 2;      // a 4th-order Butterworth = 2 biquads

// The detector's time constant.
//
// It is a runtime setting because it is a TRADE and not a constant: a short
// one follows the music and a long one reads it more steadily. The numbers
// below are measured, with white noise at the bank rate, on this filter bank:
//
//   tau      the reading's fluctuation     the 63% step, band 0 -> band 7
//   125 ms   1.2 - 2.3 dB (1 sigma)         317 -> 163 ms
//    60 ms   1.6 - 2.9 dB                  253 -> 108 ms
//    40 ms   1.8 - 3.1 dB                  217 ->  79 ms
//    25 ms   2.0 - 3.4 dB                  192 ->  64 ms
//    10 ms   2.3 - 3.5 dB                  165 ->  48 ms
//     5 ms   2.6 - 3.7 dB                  140 ->  39 ms
//
// The trade is lopsided in the direction that matters here. Going from 125 ms
// to 5 ms costs about 1.3 dB of extra fluctuation and buys 180 ms of lag at
// the bottom band and 124 ms at the top of the eight. Going from 40 ms to
// 5 ms costs 0.8 dB of fluctuation and buys 77 ms at the bottom and 40 ms at
// the top.
//
// The default is 5 ms, because the whole point of this path is that it should
// move at the speed of the FFT-driven bands above it, and the lag is what made
// the bottom eight look dead next to them. The reading is noisier by less than
// a dB and half a decibel is worth far less than a tenth of a second. The
// serial 'k'/'K' keys walk it if that call is ever wrong.
//
// The floor is the FILTER, not the detector: the bottom band's envelope
// cannot rise faster than its own bandwidth allows, which is about 100 ms at
// 19.95 Hz however short the detector is. That is the physics of a narrow
// band at a low frequency, and no setting of this constant changes it.
static constexpr float AR_BANK_TAU_DEFAULT_MS = 5.0f;
static constexpr float AR_BANK_TAU_MIN_MS = 5.0f;
static constexpr float AR_BANK_TAU_MAX_MS = 500.0f;

// The bank's state: the anti-alias filter's delay lines, the band filters'
// delay lines, the detectors, and the decimation's phase.
//
// The delay lines are two floats per section because the sections are
// transposed direct form II, which needs no output buffer at all - the bank
// READS the sample stream and never writes to it. That matters: the stream it
// reads is the FFT's input, and a filter that needed an output buffer would
// have to be given one that does not overlap it.
struct BankPath {
  float   aaW[AR_BANK_AA_SECS][2];   // the anti-alias filter's delay lines
  float   secW[AR_BANK_BANDS][2][2]; // the band filters' delay lines
  float   det[AR_BANK_BANDS];        // the detectors' states
  uint8_t phase;                     // the decimation's phase
};

// The bank the FFT task runs.
static BankPath liveBank;

// The coefficients, baked once at init: b0, b1, b2, a1, a2 per section.
static float bankAaCoeffs[AR_BANK_AA_SECS][5];
static float bankSecCoeffs[AR_BANK_BANDS][2][5];

// What the init measured about the bank.
static float bankAaDb[AR_BANK_BANDS];   // the anti-alias filter's response at the band centres, dB
static float bankCalDb[AR_BANK_BANDS];  // the bank's calibration, dB
static float bankEnbw[AR_BANK_BANDS];   // the band filter's ENBW, Hz
static float bankDetAlpha = 0.0f;       // the detector's one-pole coefficient
static float bankTauMs = AR_BANK_TAU_DEFAULT_MS;  // the detector's time constant
static volatile bool  bankReady = false;

// Set the detector's time constant and the coefficient that goes with it.
//
// The coefficient is the one-pole's step size at the bank's 3 kHz rate:
// alpha = 1 - exp(-1/(tau * fs)). It is a function of the constant alone, so
// the two can never disagree - which is what would happen if the constant
// were changed without recomputing it.
static void bankSetTau(float ms) {
  if (ms < AR_BANK_TAU_MIN_MS) ms = AR_BANK_TAU_MIN_MS;
  if (ms > AR_BANK_TAU_MAX_MS) ms = AR_BANK_TAU_MAX_MS;
  bankTauMs = ms;
  bankDetAlpha = 1.0f - expf(-1.0f / (bankTauMs * 0.001f * AR_BANK_FS));
}

// The bank's own cost, in TIMING_UNIT_US, like the other timers.
static float bankTime = 0.0f;

// ---------------------------------------------------------------------------
// The bank's filters.
//
// They are hand-written rather than taken from ESP-DSP, for two reasons.
//
// The first is the output buffer: ESP-DSP's biquad has no in-place mode, so
// each section needs somewhere else to write, and the buffer the bank could
// use (the FFT's imaginary half) is the FFT's own. A transposed DF-II section
// keeps its state in two floats and produces its output in a register.
//
// The second is the cost. The bank runs 2 sections at 48 kHz over 512 samples
// and 16 sections at 3 kHz over 32 samples - 1536 evaluations a frame. At
// the hand-written recursion's measured ~130 ns per evaluation that is ~200 us
// a frame, against a 10.67 ms frame period.
//
// The sections are the same convention the file already uses everywhere:
// a0 = 1, and
//   y = b0*x + s1 ;  s1 = b1*x - a1*y + s2 ;  s2 = b2*x - a2*y
// ---------------------------------------------------------------------------

// An analog Butterworth prototype (cut-off 1 rad/s) through the bilinear
// transform.
//
// The prototype's poles are at exp(j*pi*(2k+N+1)/(2N)). Each conjugate pair
// becomes one second-order section; an odd order also has a real pole, which
// becomes a first-order section written in the biquad's form with b2 and a2
// zero. The cascade's gain at DC is 1.
//
// The pole loop skips the lower half plane, so each pair is placed once.
static int bankButterLowpass(float fc, float fs, int order, float out[][5], int maxSecs) {
  const float K  = tanf((float)M_PI * fc / fs);
  const float K2 = K * K;
  int n = 0;
  for (int k = 0; k < order && n < maxSecs; k++) {
    const float theta = (float)M_PI * ((float)order + 2.0f * (float)k + 1.0f)
                              / (2.0f * (float)order);
    const float px = cosf(theta);
    const float py = sinf(theta);
    if (py < -1e-6f) continue;          // the conjugate of a pole already placed
    const float sigma = -px;            // the pole is at -sigma +- j*|py|
    if (fabsf(py) < 1e-6f) {            // a real pole: a first-order section
      const float A = 1.0f + sigma * K;
      out[n][0] = K / A;
      out[n][1] = K / A;
      out[n][2] = 0.0f;
      out[n][3] = (1.0f - sigma * K) / A;
      out[n][4] = 0.0f;
    } else {
      const float A = 1.0f / K2 + 2.0f * sigma / K + 1.0f;
      out[n][0] = 1.0f / A;
      out[n][1] = 2.0f / A;
      out[n][2] = 1.0f / A;
      out[n][3] = (2.0f - 2.0f / K2) / A;
      out[n][4] = (1.0f / K2 - 2.0f * sigma / K + 1.0f) / A;
    }
    n++;
  }
  return n;
}

// A bandpass with its peak gain at 1.0, at f0.
static void bankRbjBpf(float f0, float fs, float Q, float *c) {
  const float w0    = 2.0f * (float)M_PI * f0 / fs;
  const float alpha = sinf(w0) / (2.0f * Q);
  const float a0    = 1.0f + alpha;
  c[0] = alpha / a0;
  c[1] = 0.0f;
  c[2] = -alpha / a0;
  c[3] = -2.0f * cosf(w0) / a0;
  c[4] = (1.0f - alpha) / a0;
}

// The squared magnitude of one section at f.
static float bankSecMag2(const float *c, float f, float fs) {
  const float w  = 2.0f * (float)M_PI * f / fs;
  const float cw = cosf(w), sw = sinf(w);
  const float c2 = cw * cw - sw * sw, s2 = 2.0f * cw * sw;
  // z = e^(-jw), so z^2 = c2 - j*s2
  const float nr = c[0] + c[1] * cw + c[2] * c2;
  const float ni = c[1] * (-sw) + c[2] * (-s2);
  const float dr = 1.0f + c[3] * cw + c[4] * c2;
  const float di = c[3] * (-sw) + c[4] * (-s2);
  return (nr * nr + ni * ni) / (dr * dr + di * di);
}

// The band's filters, baked.
//
// A 4th-order Butterworth bandpass is two RBJ bandpass sections, and its
// section Qs are
//   Q_k = (f0/W) * 1/(2 sin(pi(2k-1)/(4N)))
// with W the nominal width and N the order.
//
// The ISO 266 third-octave width is W = 0.23156333 * f0 in EVERY band, so
// f0/W = 4.3188 for all of them and both Qs are the same in all eight:
//   Q1 = 4.3188 / (2 sin(pi/8))  = 5.642
//   Q2 = 4.3188 / (2 sin(3pi/8)) = 2.337
// The width is taken from the layout's own edges rather than assumed, because
// the ISO set's centres step by 10^(1/10) and not by 2^(1/3).
static void bankInitCoefficients() {
  bankButterLowpass(AR_BANK_AA_HZ, (float)SAMPLE_RATE,
                     2 * AR_BANK_AA_SECS, bankAaCoeffs, AR_BANK_AA_SECS);
  for (int k = 0; k < AR_BANK_BANDS; k++) {
    const float fc    = lay31.centre[k];
    const float width = lay31.edgeHi[k] - lay31.edgeLo[k];
    const float ratio = fc / width;
    const float q1 = ratio / (2.0f * sinf((float)M_PI / 8.0f));
    const float q2 = ratio / (2.0f * sinf(3.0f * (float)M_PI / 8.0f));
    bankRbjBpf(fc, AR_BANK_FS, q1, bankSecCoeffs[k][0]);
    bankRbjBpf(fc, AR_BANK_FS, q2, bankSecCoeffs[k][1]);
  }
}

// The anti-alias filter's own response at the band centres.
//
// It is the anti-alias filter's response that has to be divided out, because
// it sits between the input and the measurement. Its passband is flat to
// 0.00 dB across all eight band centres (it is a 400 Hz corner), so what it
// mostly contributes is the guarantee that nothing from the 2.9-3.1 kHz
// region - the only place the decimation can alias into these bands - gets
// through: there it is 69-72 dB down.
//
// The response is computed from the coefficients the filter runs with, not
// from an impulse measurement, and that is a deliberate difference from the
// chain's calibration. The chain's LPF is ESP-DSP's biquad - a third-party
// implementation whose convention had to be checked against its model - so
// it is measured. This filter is the file's own transposed DF-II recursion,
// whose transfer function IS the coefficient set, and the bank's self-test
// checks the whole chain end to end against the contract, which is a stronger
// check on the model than an impulse would be.
static void bankMeasureAaDb() {
  for (int k = 0; k < AR_BANK_BANDS; k++) {
    float mag2 = 1.0f;
    for (int s = 0; s < AR_BANK_AA_SECS; s++) {
      mag2 *= bankSecMag2(bankAaCoeffs[s], lay31.centre[k], (float)SAMPLE_RATE);
    }
    bankAaDb[k] = (mag2 > 1e-12f) ? (float)(10.0f * log10f(mag2)) : -120.0f;
  }
}

// The bank's calibration: the chain's response and the anti-alias filter's
// response, both divided out, plus the target curve.
//
// The chain's response is MEASURED here, at the bank's own band centres, with
// the flags the LIVE chain runs - the same measurement the main path's
// calibration is built from, run again for these eight frequencies because
// the bank has to describe the chain that is really there.
//
// The band filter itself is NOT corrected for. It is the measurement, and its
// peak gain of 1.0 at the band centre is exactly what makes a centred tone
// read its own power.
static void bankInitCalibration() {
  float freqs[AR_BANK_BANDS];
  float chainDb[AR_BANK_BANDS];
  for (int k = 0; k < AR_BANK_BANDS; k++) freqs[k] = lay31.centre[k];
  measureChainDb(chainDb, freqs, AR_BANK_BANDS, selfTestIrLength(),
                 liveChainDcOn(), liveChainLpfOn());
  for (int k = 0; k < AR_BANK_BANDS; k++) {
    float c = -chainDb[k] - bankAaDb[k];
    if (calTargetPink) c += 10.0f * log10f(lay31.centre[k] / CAL_PINK_REF_HZ);
    if (c > CAL_CLAMP_DB) c = CAL_CLAMP_DB;
    else if (c < -CAL_CLAMP_DB) c = -CAL_CLAMP_DB;
    bankCalDb[k] = c;
  }
}

// The band filter's equivalent noise bandwidth: the integral of |H|^2 over
// all frequencies, in Hz.
//
// It is the width of the filter a flat input actually sees, and it is
// published so that a shaped measurement can be compared with a brick-wall
// one: the two differ by this ratio, which is 0.85 for every band (a flat
// input reads 0.71 dB low against a brick-wall band of the nominal width).
static void bankMeasureEnbw() {
  constexpr int n = 600;
  const float f0 = 0.01f, f1 = AR_BANK_FS * 0.5f;
  for (int k = 0; k < AR_BANK_BANDS; k++) {
    float tot = 0.0f;
    float fprev = f0;
    float prev = bankSecMag2(bankSecCoeffs[k][0], f0, AR_BANK_FS)
               * bankSecMag2(bankSecCoeffs[k][1], f0, AR_BANK_FS);
    for (int i = 1; i <= n; i++) {
      const float f = f0 * powf(f1 / f0, (float)i / (float)n);
      const float cur = bankSecMag2(bankSecCoeffs[k][0], f, AR_BANK_FS)
                      * bankSecMag2(bankSecCoeffs[k][1], f, AR_BANK_FS);
      tot += 0.5f * (prev + cur) * (f - fprev);
      prev = cur; fprev = f;
    }
    bankEnbw[k] = tot;
  }
}

// Clear the bank's state: the filters start from silence and the detectors
// from zero.
//
// It is called whenever the chain is rebuilt. The chain's response is part of
// the bank's calibration, so a chain change is a change of what the bank is
// measuring, and a filter that has just been handed a different signal should
// not carry the old one's history into the new one.
static void bankReset() {
  memset(&liveBank, 0, sizeof(liveBank));
}

// Build the bank: the coefficients, the anti-alias filter's measured
// response, the calibration, the ENBW.
//
// It runs after estInitCalibration(), because the bank's calibration is built
// from the chain's MEASURED response - the same number the main path's
// calibration uses.
static void bankInit() {
  if (!bandLayoutReady) initBandLayout();
  bankInitCoefficients();
  bankMeasureAaDb();
  bankInitCalibration();
  bankMeasureEnbw();
  // The current value, not the default: the constant may have been set from
  // the configuration before the bank was built.
  bankSetTau(bankTauMs);
  bankReset();
  bankReady = true;
}

// Run the bank over n fresh samples and update the eight detectors.
//
// The samples are read, never written. The filters are transposed direct form
// II, so each section's state is two floats and the output is a local
// variable - the bank needs no buffer of its own, and it cannot disturb the
// FFT's input.
static void bankProcess(BankPath *b, const float *in, int n) {
  const float alpha = bankDetAlpha;
  for (int i = 0; i < n; i++) {
    float x = in[i];
    // The anti-alias filter, at the full rate.
    for (int s = 0; s < AR_BANK_AA_SECS; s++) {
      const float *c = bankAaCoeffs[s];
      float *w = b->aaW[s];
      const float y = c[0] * x + w[0];
      w[0] = c[1] * x - c[3] * y + w[1];
      w[1] = c[2] * x - c[4] * y;
      x = y;
    }
    // One sample in AR_BANK_DEC is kept.
    if (++b->phase < AR_BANK_DEC) continue;
    b->phase = 0;
    // The eight band filters, and a detector on each one's output.
    for (int k = 0; k < AR_BANK_BANDS; k++) {
      float y = x;
      for (int s = 0; s < 2; s++) {
        const float *c = bankSecCoeffs[k][s];
        float *w = b->secW[k][s];
        const float o = c[0] * y + w[0];
        w[0] = c[1] * y - c[3] * o + w[1];
        w[1] = c[2] * y - c[4] * o;
        y = o;
      }
      const float p = y * y;
      b->det[k] += alpha * (p - b->det[k]);
    }
  }
}

// ---------------------------------------------------------------------------
// THE DISPLAY BANDS' OWN BOOKKEEPING
// ---------------------------------------------------------------------------
//
// The sixteen display bands are sixteen of the thirty-one RTA bands, so they
// have no measurement of their own any more. The display bank that used to
// measure them is gone.
//
// It went because it measured a DIFFERENT THING. The bank read a point PSD in
// units^2/Hz at the old half-octave centres; the display now reads an RTA
// band's integrated power in units^2 at a third-octave centre. A number that
// is not the number on the bar is worse than no number at all - and the bank
// cost 582 us a frame to produce it.
//
// It went for a second reason, which is physics rather than taste. The bank's
// advantage was FRESHNESS: its filters saw the last few milliseconds, while
// the transform's 4096-sample window was 42.7 ms old. But the display bands
// are now third-octave bands, and a narrower band charges more slowly: the
// bank's bottom band settled in about 18 ms, and a third-octave band at 40 Hz
// takes about 54 ms. Re-pointing the bank at the new bands would have cost
// exactly the freshness it was bought for.
//
// What is left here is bookkeeping, and the transform's own reading of the old
// half-octave layout - the transform's instrument, published in the endpoint's
// "transform_bands" object and not in the "geq" one, because the display bands are
// RTA bands now and the transform's half-octave reading is not one of them.

// The sixteen display bands. The count is the RTA's subset size.
static constexpr int AR_DISP_BANDS = NUM_GEQ_CHANNELS;

// The display smoothing, when the user's own settings are not set.
//
// BOTH views use them - the sixteen bars and the thirty-one RTA bars are
// smoothed the same way, so they stay the same picture at two resolutions.
static constexpr float AR_DISP_ATTACK_MS  = 8.0f;
static constexpr float AR_DISP_RELEASE_MS = 60.0f;

// How far the band moved since the last frame, in dB - and the last frame's
// value, so the next frame can measure the step.
static float dispMoveDb[NUM_GEQ_CHANNELS];
static float dispPrevDb[NUM_GEQ_CHANNELS];

// The transform's OWN reading of the old half-octave layout: the peak of the
// point PSD over the band, the same without the crest compensation, and the
// band's mean density.
//
// These are the transform's instrument - what the proof-of-concept path
// measures - and they are published in the endpoint's "transform_bands" object, not
// in the "geq" one: the display bands are RTA bands now, and the transform's
// half-octave reading is not one of them.
static float dispTransformDb[NUM_GEQ_CHANNELS];
static float dispTransformRawDb[NUM_GEQ_CHANNELS];
static float dispTransformMeanDb[NUM_GEQ_CHANNELS];

// ---------------------------------------------------------------------------
// The RTA's TOP bands, on their own filters
// ---------------------------------------------------------------------------
//
// The RTA's bottom eight bands already come from a filter bank - see the bank
// above - and the top twenty-three still come from the FFT.  That is what
// keeps the 4096-sample window alive: a window that long cannot see anything
// younger than 42.7 ms, and the FFT costs 2.8 to 3.5 ms a frame.
//
// This is that bank again, at the full rate, over the RTA's own layout.
//
// THE CONVENTION.  The bottom eight report the SHAPED power - the power
// through the bandpass - and their filter's ENBW is 0.85 of the nominal band
// width.  The top twenty-three have to have the same kernel, or the RTA view
// gets a step at band 8: a flat input would read 0.71 dB low in the bottom
// bands and something else entirely in the top ones.
//
// So this kernel is chosen by its ENBW, not by its -3 dB width.  The plain
// Butterworth design gives the right ENBW by itself below 1.6 kHz, but the
// bilinear transform's warping collapses the digital band as f0/fs rises - at
// 19 952 Hz it is a seventh of the width it should be - so the design is
// solved per band until the ENBW lands on the target.
//
// The search uses Parseval's identity, ENBW = fs/2 * sum(h^2), so it needs
// no integration and no statistics, and it runs on the filter's own impulse
// response - through the same recursion the bank runs.
//
// Measured on the design: every band lands within 0.001 dB of the target,
// the gain at the centre is 1.000, and the leakage into the neighbouring
// bands is -12.2 dB at the bottom and -7.8 dB at the top - the same shape as
// the bottom eight's -12.35 dB.
static constexpr int   AR_RTA_TOP_FIRST  = AR_BANK_BANDS;                 // 8
static constexpr int   AR_RTA_TOP_BANDS  = NUM_RTA_CHANNELS - AR_BANK_BANDS;  // 23
static constexpr float AR_RTA_ENBW_RATIO = 0.85f;   // the bottom bank's, to 3 places

// The 4th-order Butterworth bandpass's two pole Qs: 1/(2 sin(pi/8)) and
// 1/(2 sin(3 pi/8)).  The bottom bank uses the same two, and so does this
// one: the kernel's SHAPE is what has to match, not only its width.
static constexpr float AR_BANK_POLE_Q1 = 1.30656296f;
static constexpr float AR_BANK_POLE_Q2 = 0.54119610f;

// The bank's state: two delay lines per band (two sections each) and a
// detector.
struct RtaTopBank {
  float secW[AR_RTA_TOP_BANDS][2][2];
  float det[AR_RTA_TOP_BANDS];
};

static RtaTopBank liveRtaTop;
static float rtaSecCoeffs[AR_RTA_TOP_BANDS][2][5];
static float rtaEnbw[AR_RTA_TOP_BANDS];      // the band's ENBW, Hz (the integral)
static float rtaEnbwImp[AR_RTA_TOP_BANDS];   // ... and the impulse response's
static float rtaCalDb[AR_RTA_TOP_BANDS];     // the calibration, dB
static float rtaWidthHz[AR_RTA_TOP_BANDS];   // the measured -3 dB width, Hz
static float rtaRespMs[AR_RTA_TOP_BANDS];    // the measured 63% response time, ms
static float rtaDetAlpha = 0.0f;
static float rtaTauMs = AR_BANK_TAU_DEFAULT_MS;
static float rtaTime = 0.0f;                 // its own cost, in TIMING_UNIT_US
static volatile bool  rtaTopReady = false;
// Whether the detector's time constant has been set deliberately. Until it
// has, the top bank follows the bottom bank's, so the RTA's thirty-one bars
// behave alike out of the box.
static bool  rtaTauSet = false;

// The transform's OWN reading of the RTA's top bands, kept beside the bank's.
//
// The two are different measurements of the same audio - the transform
// integrates the spectrum over the band's nominal edges, the bank measures the
// power through its own filter - and the difference between them is the point
// of publishing both.
static float rtaTfPower[NUM_RTA_CHANNELS];

// THE PUBLISHED FRAME.
//
// The analysis runs on the FFT task and rewrites its arrays every 10.67 ms.
// The JSON endpoint runs on the main loop and reads them, so a reader that
// just looked at estRtaDb[] could see half of one frame and half of the
// next - a set of band levels that never existed.
//
// The serial plot already had this fixed (arRtaDbPub, under the publish
// lock). This is the same fix for the endpoint, and it carries the same
// reason: the numbers a reader gets must be ONE frame's numbers.
//
// What is copied is the PUBLISHED value - the dB the reader would have
// computed for itself - so the reader cannot disagree with the frame about
// which path drove the display, or about the level of a band. That is why
// the frame's decision travels with the numbers: rtaTopBankDrives is decided
// once per frame, and a reader that asked again at its own moment could get
// a different answer (the 'h'/'H' switch can be thrown between two reads)
// and then read a transform's number out of a bank-driven frame.
//
// The sixteen display bands need no decision of their own: they are sixteen
// of the thirty-one, so they follow whichever path drove the RTA band they
// came from.
struct ArFrameSnapshot {
  float rawDb[NUM_GEQ_CHANNELS];         // the 16 display bands, raw band power, dB
  float bandDb[NUM_GEQ_CHANNELS];        // the 16 display bands, calibrated, dB
  float dispTransformDb[NUM_GEQ_CHANNELS];   // the transform's reading, dB
  float dispTransformRawDb[NUM_GEQ_CHANNELS];// ... without the crest comp
  float dispTransformMeanDb[NUM_GEQ_CHANNELS];// ... and the band mean
  float rtaPowDb[AR_MAX_BANDS];          // the 31 RTA bands, integrated power, dB
  float rtaLevelDb[AR_MAX_BANDS];        // the 31 RTA bands, calibrated, dB
  float rtaTopPowDb[AR_RTA_TOP_BANDS];   // the top bank's power, dB
  float rtaTopTfDb[AR_RTA_TOP_BANDS];    // the transform's power, dB
  float rtaTopPowLin[AR_RTA_TOP_BANDS];  // ... and the same, linear, so a
  float rtaTopTfLin[AR_RTA_TOP_BANDS];   //     ratio can be taken of them
  bool  rtaTopBankDrives;                // the frame's decision, not a re-ask
};

// The snapshot before there has been a frame: every reading is "no reading".
//
// The sentinels are the file's own: -99 for a power that was not measured,
// -120 for a dB that was not measured. A zero would be a lie - 0 dB is a
// perfectly good level, and a reader cannot tell it from a measurement.
static ArFrameSnapshot arFrameSnapshotInit() {
  ArFrameSnapshot s;
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    s.rawDb[k] = -99.0f;  s.bandDb[k] = -120.0f;
    s.dispTransformDb[k] = -120.0f;
    s.dispTransformRawDb[k] = -120.0f;  s.dispTransformMeanDb[k] = -120.0f;
  }
  for (int k = 0; k < AR_MAX_BANDS; k++)       { s.rtaPowDb[k] = -99.0f; s.rtaLevelDb[k] = -120.0f; }
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++)   { s.rtaTopPowDb[k] = -99.0f; s.rtaTopTfDb[k] = -99.0f;
                                                  s.rtaTopPowLin[k] = 0.0f;  s.rtaTopTfLin[k] = 0.0f; }
  s.rtaTopBankDrives = true;
  return s;
}

static ArFrameSnapshot arFrameSnap = arFrameSnapshotInit();

// The A/B switch: the RTA's top bands come from the bank, or from the FFT.
//
// The bank is the default, and the reason is the one that was measured: the
// transform's reading cannot be newer than 42.7 ms, because that is how old
// its 4096-sample window is, and the bank's is a few milliseconds old.
//
// The switch exists so the two can be compared on the SAME audio, which is
// the only comparison worth anything: a switch changes the music, the room
// and the AGC.  Both run every frame, and the JSON publishes both readings.
//
// VOLATILE because it is written from the userloop task (the serial console,
// the JSON handler) and read on the FFT task.
static volatile bool  rtaUseBank = true;

// THE PROOF-OF-CONCEPT SWITCH: run the AudioReactive with no FFT at all.
//
// With the transform off, the whole analysis path is the filter banks: the
// thirty-one RTA bands come from the two RTA banks - and the sixteen display
// bands, being sixteen of those thirty-one, come with them - and the dominant
// frequency and the peak magnitude - the two things the transform still owned -
// are derived from the banks' own densities. There is no windowing, no FFT, no
// bit reversal and no magnitude fold in the frame.
//
// It is a RUNTIME switch on purpose. The point of the comparison is that both
// paths can be measured on the SAME audio, in the SAME frame, on the SAME
// board - a switch between two runs would compare two different pieces of
// music.
//
// The switch is the single key 'a', which TOGGLES. See
// audioReactiveTransformCommand().
//
// THE DEFAULT IS OFF. The filter banks are the whole measurement - the
// sixteen display bands, the thirty-one RTA bands, the dominant frequency
// and the peak magnitude - and the transform is the A/B reference that
// proves it.
//
// Measured on the board, 2026-10-27, on the same live audio:
//
//   The transform's error on a tone at an RTA band's centre, against truth:
//     20 Hz -7.08 dB, 25 Hz -6.06, 32 Hz -5.06, 40 Hz -4.06, 50 Hz -3.06,
//     63 Hz -2.07, 79 Hz -1.06, 100 Hz -0.06, and 0.00 above 125 Hz.
//   The bank's error on the same tones: 0.085 dB worst, all thirty-one
//   bands inside 0.09 dB.
//
// The transform's error is not a calibration error - it is a RESOLUTION
// limit. The transform's kernel is the window, 23.49 Hz wide, so every band
// narrower than that reads the same smeared number, and the bank's own
// filter - 4.6 Hz wide at the bottom - does not. Above about 100 Hz the two
// agree, because there the band is wider than the kernel and both resolve
// it. Below it, only the bank resolves it, and it is right.
//
// The cost of the transform: 2830 us of a 10670 us frame. With it off the
// AudioReactive's work per frame falls from 5.3 ms to 2.9 ms - 2.4 ms of
// CPU back - and the frame RATE does not change, because the frame is paced
// by the 10.67 ms hop and not by the work.
//
// It is VOLATILE because it is written from another task: the serial console
// handler and the JSON handler both run on the userloop task, and the frame
// reads it on the FFT task. Without volatile the compiler is free to keep the
// value in a register across the frame and act on a stale answer.
static volatile bool  arUseTransform = false;

// Whether the transform's numbers are current.
//
// The banks' numbers always are - they update every frame. The transform's
// are only current if the transform ran, so everything that reads a
// transform-derived value has to know whether it is looking at a live number
// or at the last one it saw.
static volatile bool  arTransformValid = false;

// The banks' own health, from their boot self-tests.
//
// The ready flags say a bank EXISTS. These say it WORKS: a bank that failed
// its own self-test is not a measurement, because its numbers do not mean
// what the contract they were built on says they mean.
//
// They are separate from the ready flags on purpose. The calibration is
// rebuilt at runtime and the rebuild calls the banks' init - which sets the
// ready flags again - so if health lived in the ready flags, a rebuild would
// quietly declare a failed bank healthy again.
static volatile bool  arBankHealthy   = true;
static volatile bool  arRtaTopHealthy = true;

// Whether the transform actually runs this frame.
//
// The switch says whether the user WANTS it. This says whether it HAPPENS.
//
// If any of the banks is missing - not built, or not ready - then the
// transform is the only measurement left, and a display with no measurement
// in it is worse than a slow one. So the transform turns itself back on in
// that case and the frame is measured the old way.
//
// If a bank is BUILT and READY but FAILED its own self-test, the transform
// takes the frame back too - with one exception. It cannot separate the RTA's
// bottom eight bands at all: its measurement kernel is 23.49 Hz wide and
// those bands are 4.62 Hz, so it would read the same smeared number into
// five or six of them. A bank that is out of contract by half a decibel is
// still a far better measurement of a 4.62 Hz band than a 23.49 Hz smeared
// one, so the bottom bank keeps those bands even when it has failed its
// test, and the failure is reported instead.
//
// With the banks healthy, which is the normal case and the case the boot
// self-tests assert, this is exactly the switch.
//
// The sixteen display bands need no separate case: they are sixteen of the
// thirty-one, so they are as healthy or as unhealthy as the RTA band they came
// from, and the transform takes them back at the same time it takes the RTA's
// top bands back.
static inline bool arTransformRuns() {
  if (arUseTransform) return true;
  if (!bankReady || !rtaTopReady) return true;
  if (!arRtaTopHealthy) return true;
  return false;
}

// Which measurement drives the RTA's top twenty-three.
static inline bool arRtaTopBankDrives(bool tfRuns) {
  return rtaTopReady && arRtaTopHealthy && (rtaUseBank || !tfRuns);
}

// The bank's own answers to the two questions the transform was answering:
// the dominant frequency and the peak magnitude.
//
// They are computed every frame, so the comparison is live: with the
// transform on, the transform's answer is what the effects use and the bank's
// is published beside it; with the transform off, the bank's IS the answer.
static float arBankPeakHz = 0.0f;       // the dominant frequency, Hz
static float arBankMagnitude = 0.0f;    // the peak magnitude, legacy scale

// The self-test's verdict.
static volatile bool  rtaTestDone = false;
static int   rtaTestFails = 0;
static float rtaTestWorst = 0.0f;        // the worst tone error, dB
static float rtaTestWorstF = 0.0f;
static float rtaTestWhiteWorst = 0.0f;   // the worst white-noise error, dB
static float rtaTestWhiteWorstF = 0.0f;
static float rtaTestEnbwWorst = 0.0f;    // the worst ENBW disagreement, dB
static float rtaTestEnbwWorstF = 0.0f;
static float rtaTestNbrWorst = -99.0f;   // the worst neighbour leakage, dB
static float rtaTestToneDb[AR_RTA_TOP_BANDS];
static float rtaTestWhiteDb[AR_RTA_TOP_BANDS];
static float rtaTestEnbwDb[AR_RTA_TOP_BANDS];
static float rtaTestNbrDb[AR_RTA_TOP_BANDS];
static float rtaTestTimeMs = 0.0f;

// THE HEAD-TO-HEAD: the transform against the banks, on the same tone.
//
// The banks' self-tests already prove that each bank reads a tone at its own
// band's centre as that tone's power. The transform's own answer to the same
// question is measured here, with the same tone and the same truth, so the
// two are directly comparable.
//
// The two answers should agree - both are supposed to be the band's
// integrated power - and they do not, and the size of the disagreement is the
// whole argument for the banks.
//
// xfTestTfDb[k]   the transform's error, dB, on a tone at RTA band k's centre
// xfTestBankDb[k] the bank's error, dB, on the same tone
// xfTestPredDb[k] what the transform's kernel width predicts its error to be
static float xfTestTfDb[NUM_RTA_CHANNELS];
static float xfTestBankDb[NUM_RTA_CHANNELS];
static float xfTestPredDb[NUM_RTA_CHANNELS];
static bool  xfTestHas[NUM_RTA_CHANNELS];   // was this band measured at all?
static int   xfTestBands = 0;              // how many bands were measured
static int   xfTestTfFails = 0;            // bands where the transform missed by > 1 dB
static int   xfTestBankFails = 0;          // bands where the bank missed by > 1 dB
static float xfTestTfWorst = 0.0f;         // the worst transform error, dB
static int   xfTestTfWorstBand = -1;
static float xfTestBankWorst = 0.0f;       // the worst bank error, dB
static int   xfTestBankWorstBand = -1;
static volatile bool  xfTestDone = false;

// The head-to-head's helpers, defined further down next to the estimator they
// use. The RTA top bank's self-test is above them, and it is one of the two
// places that measures the transform.
static float xfTransformBandDb(const float *frame, int k);
static void   xfRecord(int k, float tfDb, float bankDb, float toneDb);
static float xfKernelEnbw();

// One band's two sections, at a given scale.
//
// The scale multiplies both pole Qs, so it is the WIDTH of the kernel: a
// bigger scale is a narrower band.  The design is the bottom bank's, with the
// same two pole Qs - so the kernel's shape matches and only its width moves.
static void rtaTopDesign(int k, float scale) {
  const int j = AR_RTA_TOP_FIRST + k;
  const float fc    = lay31.centre[j];
  const float width = lay31.edgeHi[j] - lay31.edgeLo[j];
  const float ratio = fc / width;
  bankRbjBpf(fc, (float)SAMPLE_RATE, scale * AR_BANK_POLE_Q1 * ratio, rtaSecCoeffs[k][0]);
  bankRbjBpf(fc, (float)SAMPLE_RATE, scale * AR_BANK_POLE_Q2 * ratio, rtaSecCoeffs[k][1]);
}

// The band's ENBW, from its impulse response.
//
// Parseval says the integral of |H|^2 over the spectrum is fs times the sum
// of h^2, so the one-sided integral - half of it - is fs/2 times the sum.  The
// impulse goes through the SAME recursion the bank runs, so this measures the
// code and not a model of it.
//
// The record is 4096 samples - 85.3 ms - and it is long enough: the shortest
// band's impulse decays as exp(-pi*f0*t/Q) = exp(-65 t), and the energy left
// in the tail at 85 ms is a part in ten thousand.
static float rtaTopEnbwByImpulse(int k) {
  const float *c0 = rtaSecCoeffs[k][0];
  const float *c1 = rtaSecCoeffs[k][1];
  float a1 = 0.0f, a2 = 0.0f, b1 = 0.0f, b2 = 0.0f;
  float h2 = 0.0f;
  for (int i = 0; i < 4096; i++) {
    const float x = (i == 0) ? 1.0f : 0.0f;
    const float y = c0[0] * x + a1;
    a1 = c0[1] * x - c0[3] * y + a2;
    a2 = c0[2] * x - c0[4] * y;
    const float z = c1[0] * y + b1;
    b1 = c1[1] * y - c1[3] * z + b2;
    b2 = c1[2] * y - c1[4] * z;
    h2 += z * z;
  }
  return 0.5f * (float)SAMPLE_RATE * h2;
}

// Solve the band: the scale whose ENBW lands on the target.
//
// The ENBW falls as the scale rises, so the search is a bisection on the
// geometric mean of the bracket.  The plain design is tried first and
// accepted when it is already on the target, which is the case for every band
// below 1.6 kHz - and that is most of the work saved.
static float rtaTopSolveScale(int k) {
  const int j = AR_RTA_TOP_FIRST + k;
  const float width = lay31.edgeHi[j] - lay31.edgeLo[j];
  const float target = AR_RTA_ENBW_RATIO * width;
  rtaTopDesign(k, 1.0f);
  const float e0 = rtaTopEnbwByImpulse(k);
  if (fabsf(10.0f * log10f(e0 / target)) < 0.02f) return 1.0f;
  float lo = 0.02f, hi = 50.0f;
  for (int i = 0; i < 14; i++) {
    const float mid = sqrtf(lo * hi);
    rtaTopDesign(k, mid);
    if (rtaTopEnbwByImpulse(k) > target) lo = mid; else hi = mid;
  }
  return sqrtf(lo * hi);
}

// The bands, baked.
static void rtaTopInitCoefficients() {
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    rtaTopDesign(k, rtaTopSolveScale(k));
    rtaEnbwImp[k] = rtaTopEnbwByImpulse(k);
  }
}

// The band's ENBW, the other way: the integral of |H|^2 over the spectrum.
//
// This is the number the reading is divided by, and the self-test checks it
// against the impulse response's - a check on the number and not on a model
// of it.
static void rtaTopMeasureEnbw() {
  constexpr int n = 1200;
  const float fs = (float)SAMPLE_RATE;
  const float f0 = 0.01f, f1 = fs * 0.5f;
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    float tot = 0.0f;
    float fprev = f0;
    float prev = bankSecMag2(rtaSecCoeffs[k][0], f0, fs)
               * bankSecMag2(rtaSecCoeffs[k][1], f0, fs);
    for (int i = 1; i <= n; i++) {
      const float f = f0 * powf(f1 / f0, (float)i / (float)n);
      const float cur = bankSecMag2(rtaSecCoeffs[k][0], f, fs)
                      * bankSecMag2(rtaSecCoeffs[k][1], f, fs);
      tot += 0.5f * (prev + cur) * (f - fprev);
      prev = cur; fprev = f;
    }
    rtaEnbw[k] = tot;
  }
}

// The band's digital -3 dB width, MEASURED.
//
// It is the check on the design: the ENBW is the target, and this says how
// wide the half-power points actually ended up.
static void rtaTopMeasureWidth() {
  constexpr int MAXSTEP = 20000;
  const float fs = (float)SAMPLE_RATE;
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    const float *c0 = rtaSecCoeffs[k][0];
    const float *c1 = rtaSecCoeffs[k][1];
    const float fc = lay31.centre[AR_RTA_TOP_FIRST + k];
    const float peak = bankSecMag2(c0, fc, fs) * bankSecMag2(c1, fc, fs);
    const float step = fc * 0.0005f;
    float lo = 0.0f;
    for (int i = 1; i <= MAXSTEP; i++) {
      const float f = fc - (float)i * step;
      if (f <= 0.0f) break;
      if (bankSecMag2(c0, f, fs) * bankSecMag2(c1, f, fs) <= 0.5f * peak) { lo = f; break; }
    }
    float hi = fs * 0.5f;
    for (int i = 1; i <= MAXSTEP; i++) {
      const float f = fc + (float)i * step;
      if (f >= fs * 0.5f) break;
      if (bankSecMag2(c0, f, fs) * bankSecMag2(c1, f, fs) <= 0.5f * peak) { hi = f; break; }
    }
    rtaWidthHz[k] = hi - lo;
  }
}

// The bank's calibration: the chain's measured response, divided out.
//
// It is the same correction the transform's RTA bands get - calDb[] holds the
// chain's MEASURED response at every band centre of both layouts, and the
// chain is in front of this bank exactly as it is in front of the transform.
// So rtaCalDb[] IS calDb[]'s RTA half, and nothing about the calibration
// changes when the source does.
//
// The band filter itself is NOT corrected for.  It is the measurement, and its
// peak gain of 1.0 at the centre is what makes a centred tone read its own
// power.
static void rtaTopInitCalibration() {
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    rtaCalDb[k] = calDb[NUM_GEQ_CHANNELS + AR_RTA_TOP_FIRST + k];
  }
}

// The detector's time constant and the coefficient that goes with it.
static void rtaTopSetTau(float ms) {
  if (ms < AR_BANK_TAU_MIN_MS) ms = AR_BANK_TAU_MIN_MS;
  if (ms > AR_BANK_TAU_MAX_MS) ms = AR_BANK_TAU_MAX_MS;
  rtaTauMs = ms;
  rtaDetAlpha = 1.0f - expf(-1.0f / (rtaTauMs * 0.001f * (float)SAMPLE_RATE));
}

// The band's 63% response time, MEASURED.
//
// The same measurement the display bank makes: an impulse goes in, the
// detector's expected value rises as the cumulative energy fills up, and the
// 63% point says how old the content on the bar is.
static void rtaTopMeasureResponse() {
  constexpr int N = 4096;
  const float alpha = rtaDetAlpha;
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    const float *c0 = rtaSecCoeffs[k][0];
    const float *c1 = rtaSecCoeffs[k][1];
    float det = 0.0f, energy = 0.0f;
    float a1 = 0.0f, a2 = 0.0f, b1 = 0.0f, b2 = 0.0f, x = 1.0f;
    for (int i = 0; i < N; i++) {
      const float y = c0[0] * x + a1;
      a1 = c0[1] * x - c0[3] * y + a2;
      a2 = c0[2] * x - c0[4] * y;
      const float z = c1[0] * y + b1;
      b1 = c1[1] * y - c1[3] * z + b2;
      b2 = c1[2] * y - c1[4] * z;
      x = 0.0f;
      energy += z * z;
      det += alpha * (energy - det);
    }
    const float finalV = det;
    float t63 = 0.0f;
    if (finalV > 0.0f) {
      det = 0.0f; energy = 0.0f;
      a1 = a2 = b1 = b2 = 0.0f; x = 1.0f;
      for (int i = 0; i < N; i++) {
        const float y = c0[0] * x + a1;
        a1 = c0[1] * x - c0[3] * y + a2;
        a2 = c0[2] * x - c0[4] * y;
        const float z = c1[0] * y + b1;
        b1 = c1[1] * y - c1[3] * z + b2;
        b2 = c1[2] * y - c1[4] * z;
        x = 0.0f;
        energy += z * z;
        det += alpha * (energy - det);
        if (det >= 0.63f * finalV) {
          t63 = (float)i * 1000.0f / (float)SAMPLE_RATE;
          break;
        }
      }
    }
    rtaRespMs[k] = t63;
  }
}

// Clear the bank's state: the filters start from silence and the detectors
// from zero.
static void rtaTopReset() {
  memset(&liveRtaTop, 0, sizeof(liveRtaTop));
}

// Build the bank: the coefficients, the ENBW, the width, the calibration, the
// measured response times.
static void rtaTopInit() {
  if (!bandLayoutReady) initBandLayout();
  rtaTopInitCoefficients();
  rtaTopMeasureEnbw();
  rtaTopMeasureWidth();
  rtaTopInitCalibration();
  // The RTA's detector: the same time constant as the bottom eight bands, so
  // the thirty-one bars behave alike. The FIRST build takes the bottom bank's
  // value; after that it keeps its own, because a change to the RTA's
  // detector has to stick across a chain rebuild.
  if (!rtaTauSet) { rtaTauMs = bankTauMs; rtaTauSet = true; }
  rtaTopSetTau(rtaTauMs);
  rtaTopMeasureResponse();
  rtaTopReset();
  rtaTopReady = true;
}

// Run the bank over n fresh samples and update the twenty-three detectors.
//
// The samples are read, never written: the filters are transposed direct form
// II, so each section's state is two floats and the output is a local
// variable - the bank needs no buffer of its own, and it cannot disturb the
// FFT's input.
//
// The bands are the OUTER loop and the samples the inner one, so each band's
// coefficients and delay line stay in the same place for all 512 samples.
static void rtaTopProcess(RtaTopBank *b, const float *in, int n) {
  const float alpha = rtaDetAlpha;
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    const float *c0 = rtaSecCoeffs[k][0];
    const float *c1 = rtaSecCoeffs[k][1];
    float *w0 = b->secW[k][0];
    float *w1 = b->secW[k][1];
    float det = b->det[k];
    for (int i = 0; i < n; i++) {
      const float x = in[i];
      const float y = c0[0] * x + w0[0];
      w0[0] = c0[1] * x - c0[3] * y + w0[1];
      w0[1] = c0[2] * x - c0[4] * y;
      const float z = c1[0] * y + w1[0];
      w1[0] = c1[1] * y - c1[3] * z + w1[1];
      w1[1] = c1[2] * y - c1[4] * z;
      const float p = z * z;
      det += alpha * (p - det);
    }
    b->det[k] = det;
  }
}

// The RTA top bank's self-test.
//
// The display bank's test is the model, and the checks are the same three:
//
//   1. THE ENBW, from the impulse response against the integral.  Parseval
//      gives the first exactly; the code uses the second.  They have to
//      agree, and the ratio to the nominal width is what the bottom eight
//      bands are set to - so this is the check that the two halves of the RTA
//      are the same measurement.
//
//   2. A TONE at a band's centre.  The filter's peak gain is 1.0 there, so
//      the detector must read the tone's own power, A^2/2.
//
//   3. WHITE NOISE.  A flat input of variance v reads back as the shaped
//      power 2v*ENBW/fs - the power through a filter of that noise bandwidth.
//      This is the number the bottom eight bands read too.
//
// The neighbours are read as well, and REPORTED rather than asserted: the
// kernel is wider than the layout's band, so it leaks, and the amount says
// how much the bands overlap.
// A scratch buffer for the self-tests, taken from PSRAM and handed back when
// the test that asked for it ends.
//
// The self-tests each need a frame's worth of samples - 16 KB apiece, and
// there are four of them across the two tests.  As plain function-local
// statics they sit in the internal heap for the rest of the board's life, and
// the internal heap is what the web server's client objects are allocated
// from.  This board does not have that much to spare: with the self-test
// buffers resident the free heap sat at 8 KB, which is under the web
// server's own 8 KB floor, so it dropped every incoming connection without
// answering it and the GUI failed to load with "net::ERR_EMPTY_RESPONSE".
//
// PSRAM is where the FFT working set already lives (see the note there), and
// there is megabytes of it spare.
class PSRAMScratch {
public:
  explicit PSRAMScratch(size_t count) {
    _p = (float *)heap_caps_calloc_prefer(count, sizeof(float), 1,
                                          MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
    if (!_p) {
      // No PSRAM and no room in the internal heap either.  The self-test
      // will fault on the first write; say so rather than faulting quietly.
      USER_PRINTLN("AR: no memory for the self-test's scratch buffer");
    }
  }
  ~PSRAMScratch() { if (_p) heap_caps_free(_p); }
  PSRAMScratch(const PSRAMScratch &) = delete;
  PSRAMScratch &operator=(const PSRAMScratch &) = delete;
  float *get() const { return _p; }
  float &operator[](size_t i) { return _p[i]; }
private:
  float *_p = nullptr;
};

static void rtaTopSelfTest() {
  if (rtaTestDone) return;
  rtaTestDone = true;
  if (!bandLayoutReady) initBandLayout();
  if (!rtaTopReady) {
    USER_PRINTLN("AR: RTA top-bank self-test skipped - the bank is not built");
    return;
  }

  const uint64_t t0 = esp_timer_get_time();

  const float amp    = 0.5f;
  const float tonePow = amp * amp * 0.5f;             // the tone's own power
  const float toneDb  = 10.0f * log10f(tonePow);

  constexpr int N = 512;                              // the chunk the bank runs in
  static float buf[N];
  // The last frame of the tone, for the transform's side of the head-to-head.
  //
  // This bank runs no input chain, so the signal the transform is given here
  // is the tone itself - which is also the signal the bank is given, so the
  // two are asked the same question about the same samples.
  PSRAMScratch ring(samplesFFT);
  PSRAMScratch frame(samplesFFT);
  int   ringPos = 0;
  RtaTopBank tb;                                      // the bank's own state

  int fails = 0;
  float worst = 0.0f, worstF = 0.0f;
  float whiteWorst = 0.0f, whiteWorstF = 0.0f;
  float enbwWorst = 0.0f, enbwWorstF = 0.0f;
  float nbrWorst = -99.0f;

  USER_PRINTLN("\n=== AudioReactive RTA top-bank self-test ===========================");
  USER_PRINTF("  %d bands, %.0f Hz, detector %.0f ms\n",
              AR_RTA_TOP_BANDS, (double)SAMPLE_RATE, (double)rtaTauMs);

  // ---- 1. the ENBW, from the impulse response --------------------------
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    const float errDb = 10.0f * log10f(rtaEnbw[k] / rtaEnbwImp[k]);
    rtaTestEnbwDb[k] = errDb;
    if (fabsf(errDb) > 0.20f) fails++;
    if (fabsf(errDb) > fabsf(enbwWorst)) {
      enbwWorst = errDb; enbwWorstF = lay31.centre[AR_RTA_TOP_FIRST + k];
    }
  }

  // ---- 2. a tone at each band's centre ---------------------------------
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    const float fc = lay31.centre[AR_RTA_TOP_FIRST + k];
    memset(&tb, 0, sizeof(tb));

    // The tone, by rotation.  sinf() per sample would cost more than the
    // twenty-three filters it feeds.
    const float dw = (float)(2.0 * (double)M_PI * (double)fc / (double)SAMPLE_RATE);
    const float cs = cosf(dw);
    const float sn = sinf(dw);
    float cr = 1.0f, ci = 0.0f;

    // Time to settle, and time to average.
    //
    // The settle has to cover the FILTER, not just the detector: the filter
    // decays as exp(-pi*ENBW*t).  The average has to cover the detector's
    // ripple at twice the tone's frequency, which at the lowest band here is
    // 252 Hz - a 4 ms period, so 200 ms is fifty of them.
    const float settleSecs = fmaxf(0.12f, 6.0f / rtaEnbw[k]);
    const float avgSecs    = 0.2f;
    const int settleChunks = (int)(settleSecs * (float)SAMPLE_RATE / (float)N);
    const int avgChunks    = (int)(avgSecs    * (float)SAMPLE_RATE / (float)N);

    float acc[AR_RTA_TOP_BANDS] = {0.0f};
    int   accN = 0;
    for (int c = 0; c < settleChunks + avgChunks; c++) {
      for (int i = 0; i < N; i++) {
        buf[i] = amp * cr;
        const float nr = cr * cs - ci * sn;
        ci = cr * sn + ci * cs;
        cr = nr;
      }
      rtaTopProcess(&tb, buf, N);
      // Keep the last frame of the tone for the transform.
      for (int i = 0; i < N; i++) {
        ring[ringPos] = buf[i];
        ringPos++;
        if (ringPos >= samplesFFT) ringPos = 0;
      }
      if (c >= settleChunks) {
        for (int j = 0; j < AR_RTA_TOP_BANDS; j++) acc[j] += tb.det[j];
        accN++;
      }
    }

    float raw[AR_RTA_TOP_BANDS];
    for (int j = 0; j < AR_RTA_TOP_BANDS; j++) raw[j] = acc[j] / (float)accN;

    // The band itself: the detector must read the tone's own power.
    const float errDb = 10.0f * log10f(raw[k] / tonePow);
    rtaTestToneDb[k] = errDb;
    const bool ok = (errDb > -0.25f) && (errDb < 0.25f);
    if (!ok) fails++;
    if (fabsf(errDb) > fabsf(worst)) { worst = errDb; worstF = fc; }

    // The neighbours: how far the tone reaches into the bands either side.
    float nbrDb = -99.0f;
    for (int s = 0; s < 2; s++) {
      const int j = (s == 0) ? k - 1 : k + 1;
      if (j < 0 || j >= AR_RTA_TOP_BANDS) continue;
      const float d = 10.0f * log10f(raw[j] / raw[k]);
      if (d > nbrDb) nbrDb = d;
    }
    rtaTestNbrDb[k] = nbrDb;
    if (nbrDb > nbrWorst) nbrWorst = nbrDb;

    // THE HEAD-TO-HEAD, the top bank's half of it.
    //
    // No chain runs here, so neither side is calibrated and the truth is the
    // tone's own power. The transform sees the last frame of the tone, which
    // is the tone.
    for (int i = 0; i < samplesFFT; i++) frame[i] = ring[(ringPos + i) % samplesFFT];
    const float tfDb   = xfTransformBandDb(frame.get(), AR_RTA_TOP_FIRST + k);
    const float bankDb = (raw[k] > 0.0f) ? 10.0f * log10f(raw[k]) : -120.0f;
    xfRecord(AR_RTA_TOP_FIRST + k, tfDb, bankDb, toneDb);

    USER_PRINTF("  band %2d %9.2f Hz  enbw %+6.3f dB  tone %+7.3f dB %s  nbr %+7.2f dB"
                "  | transform %+7.3f dB %s  bank %+7.3f dB %s\n",
                k, (double)fc, (double)rtaTestEnbwDb[k], (double)errDb,
                ok ? "PASS" : "FAIL", (double)nbrDb,
                (double)(tfDb - toneDb), (fabsf(tfDb - toneDb) <= 1.0f) ? "  " : "!!",
                (double)(bankDb - toneDb), (fabsf(bankDb - toneDb) <= 0.35f) ? "  " : "!!");
  }

  // ---- 3. white noise ---------------------------------------------------
  //
  // A flat input of variance v reads back as the shaped power 2v*ENBW/fs -
  // the power through a filter of that noise bandwidth.  The precision is
  // bounded by the filter's own bandwidth, and that is physics rather than a
  // defect, so the tolerance is four times the expected scatter, computed per
  // band from that band's own ENBW.
  {
    memset(&tb, 0, sizeof(tb));
    uint32_t seed = 0x7654321u;
    const float var = 1.0f / 3.0f;                       // uniform in [-1, 1)
    const float settleSecs = 0.3f;
    const float avgSecs = 1.0f;
    const int settleChunks = (int)(settleSecs * (float)SAMPLE_RATE / (float)N);
    const int avgChunks    = (int)(avgSecs    * (float)SAMPLE_RATE / (float)N);

    float acc[AR_RTA_TOP_BANDS] = {0.0f};
    int accN = 0;
    for (int c = 0; c < settleChunks + avgChunks; c++) {
      for (int i = 0; i < N; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        buf[i] = ((float)(seed & 0xFFFFFF) / 8388608.0f) - 1.0f;   // [-1, 1)
      }
      rtaTopProcess(&tb, buf, N);
      if (c >= settleChunks) {
        for (int j = 0; j < AR_RTA_TOP_BANDS; j++) acc[j] += tb.det[j];
        accN++;
      }
    }
    for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
      const float expect = 2.0f * var * rtaEnbw[k] / (float)SAMPLE_RATE;
      const float errDb = 10.0f * log10f((acc[k] / (float)accN) / expect);
      rtaTestWhiteDb[k] = errDb;
      const float nEff = avgSecs * rtaEnbw[k];
      const float tolDb = 4.0f * 4.342944819f * sqrtf(2.0f / (nEff > 1.0f ? nEff : 1.0f));
      if (fabsf(errDb) > tolDb) fails++;
      if (fabsf(errDb) > fabsf(whiteWorst)) {
        whiteWorst = errDb; whiteWorstF = lay31.centre[AR_RTA_TOP_FIRST + k];
      }
    }
  }

  rtaTestFails        = fails;
  rtaTestWorst        = worst;
  rtaTestWorstF       = worstF;
  rtaTestWhiteWorst   = whiteWorst;
  rtaTestWhiteWorstF  = whiteWorstF;
  rtaTestEnbwWorst    = enbwWorst;
  rtaTestEnbwWorstF   = enbwWorstF;
  rtaTestNbrWorst     = nbrWorst;
  rtaTestTimeMs       = (float)((double)(esp_timer_get_time() - t0) / 1000.0);

  USER_PRINTF("  ENBW vs the impulse response: worst %+.3f dB at %.2f Hz\n",
              (double)enbwWorst, (double)enbwWorstF);
  USER_PRINTF("  tone at the centre:          worst %+.3f dB at %.2f Hz\n",
              (double)worst, (double)worstF);
  USER_PRINTF("  white noise:                 worst %+.3f dB at %.2f Hz\n",
              (double)whiteWorst, (double)whiteWorstF);
  USER_PRINTF("  neighbour leakage:           worst %+.2f dB\n", (double)nbrWorst);
  USER_PRINTF("  %s   self-test time: %.0f ms\n",
              fails ? "FAIL" : "PASS", (double)rtaTestTimeMs);
  USER_PRINTLN("=== end of RTA top-bank self-test =================================");

  // THE HEAD-TO-HEAD's verdict. This is the last of the two self-tests that
  // feed it, so this is where it is complete.
  xfTestDone = true;
  if (xfTestBands > 0) {
    USER_PRINTLN("\n=== AudioReactive head-to-head: the transform against the banks =======");
    USER_PRINTF("  %d bands measured.  The transform's kernel is %.2f Hz wide.\n",
                xfTestBands, (double)xfKernelEnbw());
    USER_PRINTF("  transform: %2d of %d bands off by more than 1.00 dB   worst %+7.3f dB at band %d\n",
                xfTestTfFails, xfTestBands, (double)xfTestTfWorst, xfTestTfWorstBand);
    USER_PRINTF("  banks:     %2d of %d bands off by more than 0.35 dB   worst %+7.3f dB at band %d\n",
                xfTestBankFails, xfTestBands, (double)xfTestBankWorst, xfTestBankWorstBand);
    USER_PRINTLN("   band      Hz    transform      bank   predicted");
    for (int k = 0; k < NUM_RTA_CHANNELS; k++) {
      if (!xfTestHas[k]) continue;
      USER_PRINTF("  %4d %9.2f   %+8.3f   %+8.3f   %+8.3f\n",
                  k, (double)lay31.centre[k], (double)xfTestTfDb[k],
                  (double)xfTestBankDb[k], (double)xfTestPredDb[k]);
    }
    USER_PRINTLN("=== end of the head-to-head ===========================================");
  }
  USER_FLUSH();
}


// Which measurement the TRANSFORM reads.
//
//   false (default) - the POINT PSD: the level in a narrowband at the band
//                     centre, reported as units^2/Hz. A tone at the centre
//                     reads exactly its own level.
//   true            - the INTEGRATED band power: the energy over the whole
//                     band, reported as units^2.
//
// The two are different quantities in different units, so they do not just
// differ by a scale factor - the bar heights differ as well. What changes is
// the SHAPE, because a wide band accumulates more energy than a narrow one.
//
// The default is the point measurement, because it is the one where a single
// tone reads correctly: at the centre of a wide band a tone is 20 dB below
// the band's integrated power.
//
// This is the TRANSFORM's switch now, and only the transform's. The sixteen
// display bands are sixteen of the thirty-one RTA bands, and those are always
// integrated band powers - the unit a hardware analyser's bar shows - so this
// setting does not touch what is on the screen. It changes the transform's own
// reading of the old half-octave layout, which is published in the
// endpoint's "transform_bands" object.
static volatile bool  freqMeasureIntegrated = false;

// The per-band trim, in dB.
//
// This is where an external box's response gets compensated - the Sigma DSP
// in the signal path, say - when it cannot be measured by the self-test.
//
// It is applied to the MEASUREMENT, before the window, exactly as the RTA
// view's own per-band trim (rtaTrimDb) is: a per-band correction is a
// property of the band, not of the display, and a correction that only moved
// the bars would be lost the moment the window was fixed.
//
// All zeros by default, which means "no trim".
//
// There is only ONE trim now, and it is the RTA's rtaTrimDb[]. The sixteen
// display bands are sixteen of the thirty-one, so a per-band correction for
// one of them is the correction for that RTA band - there is no second array
// to keep in step with the first.

// The calibrated levels, in dB. These are what the display and the RTA read.
static float estBandDb[NUM_GEQ_CHANNELS];                   // the RTA band's level, dB
static float estRtaDb[AR_MAX_BANDS];                        // integrated power, units^2

static void estInitCalibration();                           // defined below, with the chain's measurement

// ---------------------------------------------------------------------------
// Level, noise floor, gate, AGC.
//
// Four quantities, deliberately kept apart. The old code had ONE number doing
// all four jobs, and every deadlock came from that: the "floor" was a
// peak-minus-floor difference, the gate read a value the floor had already
// zeroed, and the AGC chased a level the gate kept switching off.
//
//   level - the broadband level of the FRESH HOP (the newest hopSamples
//           samples), measured after the input chain and before the window,
//           in dB re full scale (0 dB = 32767). A level: not a frame peak,
//           and not a difference between two other quantities.
//   floor - the noise floor, in the same dB. It snaps DOWN instantly when the
//           level drops below it, and creeps UP with tau = 10 s, and only
//           while the level is still within 3 dB of it - i.e. below 2x the
//           floor. While the signal is present the floor is frozen.
//   gate  - open when the level is `soundSquelch` dB above the floor, shut
//           4 dB below that (hysteresis). Decided HERE, in the FFT task,
//           from the frame being processed - not in the userloop, from a
//           frame-old value.
//   agc   - a single dB offset, applied to the DISPLAY bands and to nothing
//           else. It targets the loudest band at the display's 0 dB.
//
// Nothing here reads its own output: the level and the floor are separate
// numbers, the gate reads those two, and the AGC reads the bands. The
// deadlock the old code had is therefore structurally impossible.
// ---------------------------------------------------------------------------

static constexpr float AR_LEVEL_FS       = 32767.0f;   // 0 dB = full scale
static constexpr float AR_LEVEL_MIN_DB   = -100.0f;    // the level's floor, dB
static constexpr float AR_LEVEL_DB_LO    = -60.0f;     // the published level's range
static constexpr float AR_LEVEL_DB_HI    = 0.0f;
static constexpr float AR_FLOOR_TAU_MS   = 10000.0f;   // the floor's creep time constant
static constexpr float AR_FLOOR_2X_DB    = 3.0103f;    // 2x in amplitude = 3.01 dB
static constexpr float AR_GATE_CLOSE_DB  = 4.0f;       // the gate's hysteresis
static constexpr float AR_AGC_ATTACK_MS  = 20.0f;      // the auto-range's attack
static constexpr float AR_AGC_RELEASE_MS = 6000.0f;    // the auto-range's release
static constexpr float AR_AGC_CLAMP_DB   = 120.0f;     // the offset's travel (a runaway guard only)
//
// The release is SLOW on purpose. A fast one is what made the display pump:
// the bars crept up between the loud passages and were knocked down again at
// every one of them. And holding it instead - which is what this used to do
// - is worse: the offset is a running MINIMUM, so one quiet passage latched
// the whole display at zero and nothing brought it back but a passage louder
// than the loudest one ever was. Six seconds recovers a quiet passage
// without the creep.
static constexpr float AR_TRIM_MAX_DB    = 24.0f;      // the display trim's travel

// The display window's dB span - ONE span, for BOTH views.
//
// The 16 bands and the 31 RTA bands are two views of ONE spectrum, so they
// are drawn in ONE window: the same span, the same auto-range, the same
// fixed mode. The 16-band display used to have a span of its own
// (TROYHACKS_DBSPAN, a user setting) and an auto-range of its own, and the
// two drifted apart: a 24 dB window against a spectrum that spans 25 to 46 dB
// put half the sixteen bars on the floor at all times.
//
// The span is HOW FAR DOWN the window reaches below its top - the analyser's
// "Scale Range". It is a setting, because the floor of a spectrum display is
// a choice about what you want to look at, not a property of the material: a
// wide window holds the whole dynamic range and spends most of its height on
// the bottom of the noise, and a narrow one spends all of it on the shape.
//
// 40 dB is the DEFAULT - Troy's choice, from watching the bars on this
// board's own material. It is narrower than the span the material actually
// occupies (measured: the 16 bands run 41 to 46 dB from the loudest to the
// quietest, the 31 RTA bands 36 to 43 dB), and that is the point: the bottom
// of the spectrum is clipped off, the solid body of the bars goes with it,
// and the shape is stretched over the whole height.
//
// A wider window shows more of the noise and less of the shape. A narrower
// one shows more shape and clips more. There is no correct number - it is
// what the material looks like, and it is a setting for that reason.
//
// Narrowing it does exactly what it says on the tin: everything more than
// that far below the loudest band reads zero, so the solid bottom of the bars
// is clipped away and the shape above it is stretched over the whole height.
// The auto-range still decides WHERE the top is - the span only says how far
// down the window reaches. That is the analyser's "Scale Top" (the
// auto-ranging button) and "Scale Range" (this number), and they are the two
// numbers his RTA has.
static constexpr float AR_WINDOW_DBSPAN    = 40.0f;   // the shared window's default dB span
static constexpr float AR_WINDOW_TARGET_DB = 0.0f;    // the loudest band is held here

// ---------------------------------------------------------------------------
// The display's dB WINDOW - ONE window, for both views.
//
// The auto-range is the peak hold above, and it has no absolute scale at all:
// its top is wherever the last peak put it, so two runs of the same material
// can be drawn in two different places, and a reading of "-30 dB" on the bars
// means nothing on its own.
//
// A hardware analyser does not have that problem, because its window IS
// absolute: a grid in dB, and the bars sit on it. So the display needs a
// second mode - a FIXED window - where the floor and the top are two numbers
// in dB and nothing moves.
//
//   auto  - the peak hold. The window is arWindowRangeDb wide and its
//           position is decided by the signal. Good for SHAPE, useless for
//           LEVEL.
//   fixed - an absolute window: a floor in dB and a top in dB, on the same
//           scale as the analyser they are being compared with. Good for
//           LEVEL, and the window can be lined up with the analyser's own
//           grid so the two read the same numbers.
//
// The fixed window is the answer to "how do we set the decibel range so it
// matches the RTA and use accurate decibels to change the floor and the top
// height". The display trims do NOT apply in fixed mode: they are display
// artefacts, and an absolute read-out that carried them would not be absolute.
// The calibration knobs that DO apply - the reference offset and the per-band
// trim below - are applied to the MEASUREMENT, before the window, which is
// what makes the fixed window an absolute one.
// ---------------------------------------------------------------------------
static volatile bool  arAutoRange = true;      // false = the fixed window is in use
static float arWindowFloorDb = -60.0f;         // the fixed window's floor, dB
static float arWindowTopDb   =  0.0f;          // the fixed window's top, dB
// The AUTO window's span: how far below the auto-ranged top the window
// reaches. This is the only number that moves the floor of the auto-range -
// the top is the peak hold's, and it is not a setting.
//
// It is the analyser's "Scale Range". The auto-range is the "Scale Top".
static float arWindowRangeDb = AR_WINDOW_DBSPAN;
static constexpr float AR_WINDOW_FLOOR_MIN = -120.0f;  // the floor's travel
static constexpr float AR_WINDOW_FLOOR_MAX =  40.0f;
static constexpr float AR_WINDOW_SPAN_MIN  =   6.0f;   // the window's minimum width
static constexpr float AR_WINDOW_SPAN_MAX  = 120.0f;
static constexpr float AR_RTA_REF_MAX   =  60.0f;   // the reference offset's travel

// The reference offset: the difference between our dB and the analyser's.
//
// One number, and it can be measured without knowing anything about the
// microphone: feed the SAME signal to the analyser's input and to the board's
// input, and the microphone's unknown sensitivity cancels out. What is left is
// the analyser's own reference - which is a CHOICE, not a measurement, and
// which is exactly what this number records.
//
// It is applied to the band levels themselves and not to the display, so the
// RTA band levels published on /json/ar are already on the analyser's scale
// and can be compared with it directly.
static float rtaRefDb = 0.0f;

// The per-band trim, in dB.
//
// The reference offset fixes the SCALE; this fixes the SHAPE. An analyser's
// third-octave filters are not exactly the ISO 266 ones - a different Q, a
// different bandwidth - so our bands and its bands do not have the same shape
// even with the scale right. These 31 numbers are the residual, one per band,
// and they are found by comparing the two on a signal that excites every band
// (pink noise, or a slow sweep).
//
// It is applied in the same place as the reference offset - to the measured
// level, before the auto-range - so the auto-range sees the corrected shape
// and the published value is the corrected one.
static float rtaTrimDb[AR_MAX_BANDS] = {0.0f};
static int   rtaTrimBand = 0;            // the band the serial trim works on

static float arLevelDb   = AR_LEVEL_MIN_DB;   // the fresh hop's level, dB re full scale
static float arFloorDb   = AR_LEVEL_MIN_DB;   // the noise floor, dB
static float arAgcDb     = 0.0f;              // the spectrum's auto-range offset, dB
static float arRtaSmth[NUM_RTA_CHANNELS] = {0.0f};  // the RTA bars' smoothing, dB
static volatile bool  arGateOpen  = false;   // the gate's state
static bool  arTrackInit = false;             // the trackers have their first value
static uint32_t arTrackMs = 0;                // when the trackers last ran
static float arFrameDtMs = 0.0f;              // the time since the previous frame
static float arBandSmth[NUM_GEQ_CHANNELS];    // the smoothed band levels, dB

// The broadband level of the fresh hop: the RMS of the newest hopSamples
// samples, after the input chain and before the window.
//
// The RMS is the right quantity for a level. The frame peak depends on where
// the waveform happens to be at the frame's edge; the old "peak minus floor"
// difference depends on the floor, which made the level and the gate two
// readings of the same number - and the gate then read a value the floor had
// already zeroed.
static float arHopLevelDb(const float *frame, int n) {
  // The accumulator is float, not double. The P4 has no double-precision
  // hardware - every double operation is a libgcc soft-float call - and this
  // runs 512 times a frame, so the double version was ~48k soft-float
  // multiply-adds a second on the hot path for no measurable gain. The sum is
  // O(1)-bounded (512 terms of a value in [-1, 1]), so float32's relative
  // error stays around 1e-6, which is 1e-5 dB on the level - five orders of
  // magnitude below anything the level tracker or the gate can act on.
  float acc = 0.0f;
  int used = 0;
  for (int i = 0; i < n; i++) {
    const float v = frame[i];
    // Skip the same artefacts the peak detector skips: one corrupted sample
    // would dominate an RMS, because the RMS squares its input.
    if ((v > (float)(INT16_MAX - 1024)) || (v < (float)(INT16_MIN + 1024))) continue;
    acc += v * v;
    used++;
  }
  if (!used) return AR_LEVEL_MIN_DB;
  const float rms = sqrtf(acc / (float)used);
  if (rms < 1e-4f) return AR_LEVEL_MIN_DB;
  return 20.0f * log10f(rms / AR_LEVEL_FS);
}

// The noise floor and the gate.
//
// The floor is a minimum statistic with a slow upward creep: it follows the
// level down at full speed and follows it up with a 10 s time constant, and
// only while the level is still close to it. A signal 10 dB above the floor
// is a signal, and must not pull the floor up - that was the old bug, where
// the floor climbed onto a sustained tone until the gate closed on it.
//
// The seed matters: a floor initialised AT the level would close the gate on
// whatever happened to be playing at boot, and the gate would then stay shut
// for as long as the signal never dropped. The floor is therefore seeded
// BELOW the level, by the gate's own threshold plus a margin, so the gate
// starts open and the floor finds the true noise floor the first time the
// input goes quiet - which is also the only time the floor is meaningful.
static void arTrackLevel(float levelDb) {
  const uint32_t now = millis();
  float dt = 0.0f;
  if (arTrackInit && (now > arTrackMs)) dt = (float)(now - arTrackMs);
  arTrackMs = now;
  arFrameDtMs = dt;

  if (!arTrackInit) {
    arFloorDb   = levelDb - (float)soundSquelch - 5.0f;
    arTrackInit = true;
  }

  if (levelDb < arFloorDb) {
    arFloorDb = levelDb;                          // snap down, instantly
  } else if (levelDb < (arFloorDb + AR_FLOOR_2X_DB)) {
    if (dt > 0.0f) {                             // creep up, slowly
      const float k = 1.0f - expf(-dt / AR_FLOOR_TAU_MS);
      arFloorDb += k * (levelDb - arFloorDb);
    }
  }

  // The gate: open `soundSquelch` dB above the floor, shut 4 dB below that.
  const float openDb  = arFloorDb + (float)soundSquelch;
  const float closeDb = openDb - AR_GATE_CLOSE_DB;
  if (arGateOpen) { if (levelDb < closeDb) arGateOpen = false; }
  else            { if (levelDb >= openDb) arGateOpen = true; }
}

// The auto-range's step: a PEAK HOLD that comes back.
//
// The loudest band sets the top of the display's range - the offset is
// whatever brings it to AR_WINDOW_TARGET_DB - and the offset stays there
// while the signal stays below it.
//
// It does NOT stay there for good. The offset is a running minimum, so
// holding it means one quiet passage latches the whole display at zero and
// nothing brings it back but a passage louder than the loudest one ever
// was: the bars stay dead until the music is louder than it has ever been.
// That is not a peak meter, it is a tripwire.
//
// So the hold has a release, and the release is SLOW. The fast release this
// replaces is what made the display pump: a bar went max, the offset cut
// down to it, the release walked the offset back up over the next second and
// a half, every bar crept up with it, and the next strong bar knocked it
// down again - max, creep, knock down, forever. Six seconds is slow enough
// that the creep is not the thing you notice, and fast enough that a quiet
// passage recovers.
//
// The offset still cuts DOWN at once, so a loud passage re-anchors the
// display immediately and never pegs it.
static inline float arAutoRangeStep(float current, float target, float dtMs) {
  if (dtMs <= 0.0f) return target;
  const float tau = (target >= current) ? AR_AGC_RELEASE_MS : AR_AGC_ATTACK_MS;
  const float k = 1.0f - expf(-dtMs / tau);
  return current + k * (target - current);
}

// ---------------------------------------------------------------------------
// The window, and the auto-range that moves it.
//
// ONE mechanism for BOTH views, and now ONE offset value as well: the sixteen
// display bands are sixteen of the thirty-one, so they are the same quantity
// in the same unit, and one dB is the same size in both. The mechanism, the
// target, the clamp, the timing and the window they are drawn in are the same,
// and so is the offset that moves it.
// ---------------------------------------------------------------------------

// The auto-range's offset: the number that brings the view's loudest band to
// the top of the window.
//
// It is FROZEN while there is no signal, and in fixed mode it is not used at
// all: a fixed window that moved with the signal would not be absolute.
static inline float arAutoRangeOffset(float &offset, float loudestDb, float dtMs) {
  if (loudestDb <= -119.0f) return offset;       // no signal: the offset stays
  const float target = AR_WINDOW_TARGET_DB - loudestDb;
  offset = arAutoRangeStep(offset, target, dtMs);
  if (offset >  AR_AGC_CLAMP_DB) offset =  AR_AGC_CLAMP_DB;
  if (offset < -AR_AGC_CLAMP_DB) offset = -AR_AGC_CLAMP_DB;
  return offset;
}

// The window's edges, and the trim that goes with them.
//
// AUTO: the loudest band is held at the top, so the window is
// [-arWindowRangeDb, 0] and the auto-range's offset moves the whole picture.
// The display trims apply on top, as they always did.
//
// FIXED: the window is the user's, in absolute dB, and NOTHING moves it - not
// the auto-range, not the display trims.
static inline void arWindowEdges(float trimAdj, float &lo, float &hi, float &valueAdj) {
  if (arAutoRange) {
    lo = -arWindowRangeDb;
    hi =  0.0f;
    valueAdj = trimAdj;
  } else {
    lo = arWindowFloorDb;
    hi = arWindowTopDb;
    valueAdj = 0.0f;
  }
}

// The codec's name, spelled the way the settings page spells it.
//
// The info page and the /json/ar endpoint both want it, and they have to
// agree - a codec called "ES8311" in one and "ES8311 (esp_codec_dev)" in the
// other is a bug waiting to happen. So it is computed here, once.
static const char* arCodecName(uint8_t type) {
  switch (type) {
    case 0:   return PSTR("Generic Analog");
    case 1:   return PSTR("Generic I2S");
    case 2:   return PSTR("ES7243");
    case 3:   return PSTR("SPH0654");
    case 4:   return PSTR("Generic I2S with MCLK");
    case 5:   return PSTR("Generic I2S PDM");
    case 51:  return PSTR("Legacy I2S PDM");
    case 6:   return PSTR("ES8388");
    case 7:   return PSTR("WM8978");
    case 8:   return PSTR("AC101");
    case 9:   return PSTR("ES8311");
    case 10:  return PSTR("ES8311 (esp_codec_dev)");
    case 11:  return PSTR("ES8388 (esp_codec_dev)");
    case 12:  return PSTR("ES8374 (esp_codec_dev)");
    case 13:  return PSTR("ZL38063 (esp_codec_dev)");
    case 14:  return PSTR("ES8389 (esp_codec_dev)");
    case 15:  return PSTR("ES7210 (esp_codec_dev)");
    case 16:  return PSTR("ES7243 (esp_codec_dev)");
    case 17:  return PSTR("ES7243E (esp_codec_dev)");
    case 18:  return PSTR("CJC8910 (esp_codec_dev)");
    case 254: return PSTR("None - network receive only");
    default:  return PSTR("Unknown");
  }
}

// Put n numbers into a JSON array.
//
// The endpoint's whole point is that the values are NUMBERS: a script can
// subtract them, sort them and average them without parsing a string first.
static void arJsonFloats(JsonArray arr, const float *v, int n) {
  for (int i = 0; i < n; i++) arr.add((double)v[i]);
}

static void arJsonBytes(JsonArray arr, const uint8_t *v, int n) {
  for (int i = 0; i < n; i++) arr.add((int)v[i]);
}

// Reset the display's auto-ranges to the state they are in on boot.
//
// Both offsets are peak holds, so they only ever travel one way: once a loud
// passage has set the top of the range, it stays there until something
// exceeds it. That is what an analyser does, but it also means the range is
// stuck at whatever the loudest thing to happen was - a different input, a
// different gain setting, or simply the start of the session all leave the
// top where the last peak put it.
//
// This puts both offsets back to zero, which is where they start on boot: the
// display starts low, and the next loud band earns the top again.
//
// The band VALUES are untouched. The offsets are display artefacts, and the
// published measurements never had them in the first place.
//
// The serial 'A' command resets the display's manual trim as well - it lives
// in wled.h, next to the command that changes it, so that half of the reset
// stays with the commands that make it.
void audioReactiveResetAutoRange() {
  arAgcDb    = 0.0f;
  // And back to the auto-range as a MODE. The fixed window is a mode, not an
  // offset, so leaving the display in it after a reset would keep showing an
  // absolute window that the user had asked to get rid of.
  arAutoRange = true;
}

// ---------------------------------------------------------------------------
// The RTA window's controls, from the serial console.
//
// Single characters, because the display is tuned while it is watched: every
// key changes one number and prints what it is now.
//
//   b / B   the window's floor, down / up by 1 dB
//   u / U   the window's top, down / up by 1 dB
//   s / S   the auto window's span, narrower / wider by 1 dB
//   o / O   the reference offset, down / up by 1 dB
//   n / p   the band the trim applies to, next / previous
//   y / Y   that band's trim, down / up by 1 dB
//   k / K   the filter bank's detector, shorter / longer by 5 ms
//   h / H   the RTA's top 23 bands: the filter bank, or the transform
//
// The window is ONE window - the 16 bands and the 31 RTA bands are drawn in
// it - so touching the floor or the top switches BOTH to the FIXED window:
// the point of setting a floor is to have a floor. 'A' goes back to the
// auto-range.
//
// 's'/'S' is the one window control that does NOT switch to the fixed
// window: the span belongs to the AUTO window, and the auto-range is what
// decides where the top is.
//
// The reference offset and the per-band trim are CALIBRATION, not display
// settings, so they survive 'A' - 'A' resets the display's auto-range, not
// the measurement.
// ---------------------------------------------------------------------------
void audioReactiveRtaCommand(char c) {
  if (!bandLayoutReady) initBandLayout();
  const int n = lay31.count;

  switch (c) {
    case 'b': case 'B': {
      arWindowFloorDb += (c == 'B') ? 1.0f : -1.0f;
      if (arWindowFloorDb > arWindowTopDb - AR_WINDOW_SPAN_MIN) arWindowFloorDb = arWindowTopDb - AR_WINDOW_SPAN_MIN;
      if (arWindowFloorDb < AR_WINDOW_FLOOR_MIN)          arWindowFloorDb = AR_WINDOW_FLOOR_MIN;
      if (arWindowTopDb  > AR_WINDOW_FLOOR_MAX)           arWindowTopDb  = AR_WINDOW_FLOOR_MAX;
      arAutoRange = false;
      USER_PRINTF("window floor now %.1f dB (window %.1f dB to %.1f dB, fixed, both views)\n",
                  (double)arWindowFloorDb, (double)arWindowFloorDb, (double)arWindowTopDb);
      break;
    }
    case 'u': case 'U': {
      arWindowTopDb += (c == 'U') ? 1.0f : -1.0f;
      if (arWindowTopDb < arWindowFloorDb + AR_WINDOW_SPAN_MIN) arWindowTopDb = arWindowFloorDb + AR_WINDOW_SPAN_MIN;
      if (arWindowTopDb > AR_WINDOW_FLOOR_MAX)             arWindowTopDb = AR_WINDOW_FLOOR_MAX;
      if (arWindowFloorDb < AR_WINDOW_FLOOR_MIN)           arWindowFloorDb = AR_WINDOW_FLOOR_MIN;
      arAutoRange = false;
      USER_PRINTF("window top now %.1f dB (window %.1f dB to %.1f dB, fixed, both views)\n",
                  (double)arWindowTopDb, (double)arWindowFloorDb, (double)arWindowTopDb);
      break;
    }
    case 's': case 'S': {
      // The AUTO window's span: how far below the auto-ranged top the window
      // reaches. The analyser's "Scale Range", and the one number that moves
      // the floor of the auto-range.
      //
      // 's' narrows it, 'S' widens it, by 1 dB each. Narrowing clips the
      // bottom of the spectrum off the display - everything more than that
      // far below the loudest band reads zero - so the solid body of the bars
      // goes and the shape above it is stretched over the whole height.
      // Widening brings the bottom back.
      //
      // It does NOT switch to the fixed window: the span is the auto window's
      // own width, and the auto-range still decides where the top is. In the
      // fixed window the span is implied by the floor and the top, so this
      // number waits there for the return to auto ('A').
      arWindowRangeDb += (c == 'S') ? 1.0f : -1.0f;
      if (arWindowRangeDb < AR_WINDOW_SPAN_MIN) arWindowRangeDb = AR_WINDOW_SPAN_MIN;
      if (arWindowRangeDb > AR_WINDOW_SPAN_MAX) arWindowRangeDb = AR_WINDOW_SPAN_MAX;
      if (arAutoRange) {
        USER_PRINTF("auto window span now %.0f dB below the top\n", (double)arWindowRangeDb);
      } else {
        USER_PRINTF("auto window span now %.0f dB below the top (the display is in the fixed window, so this waits for 'A')\n",
                    (double)arWindowRangeDb);
      }
      break;
    }
    case 'o': case 'O': {
      rtaRefDb += (c == 'O') ? 1.0f : -1.0f;
      if (rtaRefDb >  AR_RTA_REF_MAX) rtaRefDb =  AR_RTA_REF_MAX;
      if (rtaRefDb < -AR_RTA_REF_MAX) rtaRefDb = -AR_RTA_REF_MAX;
      USER_PRINTF("RTA reference offset now %+.1f dB (our dB = the analyser's dB %+.1f)\n",
                  (double)rtaRefDb, (double)rtaRefDb);
      break;
    }
    case 'n': case 'p': {
      rtaTrimBand += (c == 'n') ? 1 : -1;
      if (rtaTrimBand < 0)       rtaTrimBand = 0;
      if (rtaTrimBand >= n)      rtaTrimBand = n - 1;
      USER_PRINTF("RTA trim band %d (%.1f Hz), trim %+.1f dB\n",
                  rtaTrimBand, (double)lay31.centre[rtaTrimBand], (double)rtaTrimDb[rtaTrimBand]);
      break;
    }
    case 'y': case 'Y': {
      rtaTrimDb[rtaTrimBand] += (c == 'Y') ? 1.0f : -1.0f;
      if (rtaTrimDb[rtaTrimBand] >  24.0f) rtaTrimDb[rtaTrimBand] =  24.0f;
      if (rtaTrimDb[rtaTrimBand] < -24.0f) rtaTrimDb[rtaTrimBand] = -24.0f;
      USER_PRINTF("RTA band %d (%.1f Hz) trim now %+.1f dB\n",
                  rtaTrimBand, (double)lay31.centre[rtaTrimBand], (double)rtaTrimDb[rtaTrimBand]);
      break;
    }
    case 'k': case 'K': {
      // The bank's detector: how fast the bottom 8 bars follow the signal.
      //
      // 'k' makes it shorter (faster, more fluctuation), 'K' longer (slower,
      // steadier). The trade is measured, not guessed - see
      // AR_BANK_TAU_DEFAULT_MS for the table.
      //
      // The floor is 5 ms, and it is the DEFAULT: at that setting the bottom
      // band reads a steady tone about +/-3 dB about its mean, and the bars
      // move at the speed of the FFT bands above them. Shorter than that and
      // the squaring ripple starts to dominate the reading; longer and the
      // bars lag again.
      const float before = bankTauMs;
      const float delta = (c == 'K') ? 5.0f : -5.0f;
      bankSetTau(bankTauMs + delta);
      // The RTA's TOP bank walks with it: the thirty-one bars are one view and
      // they should move together - and so do the sixteen display bands,
      // because they are sixteen of the thirty-one.
      rtaTauMs = bankTauMs;
      rtaTauSet = true;
      rtaTopSetTau(rtaTauMs);
      if (bankTauMs <= AR_BANK_TAU_MIN_MS + 0.01f && before > AR_BANK_TAU_MIN_MS + 0.01f) {
        USER_PRINTF("RTA bank detector now %.0f ms - that is the floor, it cannot go shorter\n",
                    (double)bankTauMs);
      } else {
        USER_PRINTF("RTA bank detector now %.0f ms (%.0f ms at the 3 kHz rate)\n",
                    (double)bankTauMs, (double)bankTauMs);
      }
      break;
    }
    case 'h': case 'H': {
      // The RTA's top twenty-three bands: the bank or the transform.
      //
      // 'h' hands them to the bank - each measured by its own filter, on the
      // freshest audio in the frame - and 'H' hands them back to the
      // transform and its 42.7 ms window.
      //
      // It is a switch and not a trim, because the two measurements are
      // different things: the transform integrates the spectrum over the
      // band's nominal edges, the bank measures the power through its own
      // filter. Only one of them can be the number on the bar at a time.
      //
      // Both run every frame, so both readings are published either way and
      // the two can be compared on the same audio without switching.
      const bool want = (c == 'h');
      if (want == rtaUseBank) break;
      rtaUseBank = want;
      if (want) {
        USER_PRINTF("RTA top bands: FILTER BANK - %.1f ms at the bottom band, %.1f ms at the top\n",
                    (double)rtaRespMs[0], (double)rtaRespMs[AR_RTA_TOP_BANDS - 1]);
      } else if (arTransformRuns()) {
        USER_PRINTLN("RTA top bands: TRANSFORM - the FFT's 42.7 ms window");
      } else {
        // The choice is KEPT - it takes effect the moment the transform is
        // switched on - but the message has to say what is on the bars NOW,
        // because the bank is what is driving them and claiming otherwise
        // would be a lie the bars could not back up.
        USER_PRINTLN("RTA top bands: still the FILTER BANK - the transform is off (press a to switch it on)");
      }
      break;
    }
    default:
      break;
  }
}


// The proof-of-concept switch: the transform on or off.
//
// With the transform OFF the AudioReactive runs on the filter banks alone:
// the thirty-one RTA bands from the two RTA banks - and the sixteen display
// bands, being sixteen of those thirty-one, from the same two banks - and
// the dominant frequency and the peak magnitude from the banks' own
// densities. There is no windowing, no transform, no bit reversal and no
// magnitude fold in the frame.
//
// It is a RUNTIME switch, and that is the whole point: switching between two
// runs would compare two different pieces of music, because the music, the
// room and the AGC all move while you press the key. This way both paths run
// on the SAME audio, in the SAME frame.
//
// Nothing is reset when it is switched: the banks have been running all along
// - they update every frame whether or not they drive - so their filters are
// never stale and there is no settling transient to wait out.

// Set the transform's state and say so. The shared entry point: the serial
// key and the JSON API both come through here, so the two can never disagree
// about what the switch did.
//
// What is RUNNING is not always what was ASKED for. A bank that failed its
// self-test forces the transform back on, because then it is the only
// measurement left - and the print has to say THAT, not "off". A claim the
// bars cannot back up is worse than no print at all.
void audioReactiveTransformSet(bool want) {
  arUseTransform = want;
  const bool running = arTransformRuns();

  if (want && running) {
    USER_PRINTLN("Transform ON: the FFT runs as well, and the banks are the comparison");
    USER_PRINTLN("  it costs about 2.4 ms a frame, and below 100 Hz it is LESS");
    USER_PRINTLN("  accurate than the banks it is being compared against");
  } else if (!want && running) {
    USER_PRINTLN("Transform OFF was asked for, but a bank cannot be trusted:");
    USER_PRINTLN("  the transform keeps running - it is the only measurement left");
    USER_PRINTLN("  (the boot log says which bank failed its self-test)");
  } else {
    USER_PRINTLN("Transform OFF: the filter banks are the whole measurement");
    USER_PRINTLN("  no FFT, no windowing, no bit reversal in the frame");
    USER_PRINTLN("  the 16 display bands and the 31 RTA bands come from the banks");
    USER_PRINTLN("  the dominant frequency is the loudest RTA band's centre");
    USER_PRINTLN("  the peak magnitude is that band's density, on the legacy scale");
  }
}

// The serial key.
//
// ONE key, and it TOGGLES: 'a', for AudioReactive. 'x' was the obvious letter
// and its upper-case partner the obvious second key, but 'X' is already
// WLEDMM's "force reconnect" (wled_serial.cpp) and that branch comes first, so
// 'X' would never arrive here - and 'x' itself has traditionally meant
// "reconnect network", a key people already press. The print says which way
// the switch went.
void audioReactiveTransformCommand(char c) {
  (void)c;
  audioReactiveTransformSet(!arUseTransform);
}

// The display's auto-range: one dB offset, applied last.
//
// It targets the loudest band at AR_WINDOW_TARGET_DB - the display's 0 dB, the
// top of the bar - so the display uses its full range without any per-band
// gain. The per-band auto-gain the old code had is gone, and so is the ramp
// across the bands it produced.
//
// It is the SAME mechanism as the 31-band view's (arAutoRangeOffset), and it
// draws in the same window: there is no AGC of the 16-band's own any more.
//
// The offset comes DOWN fast (the signal got louder - that is the attack) and
// it comes back up slowly (arAutoRangeStep's release). It is FROZEN while the
// gate is shut: an offset chasing a signal that is not there would wind up to
// +60 dB and then slam the display when the signal came back. And it is not
// used at all in fixed mode, because a fixed window does not move.
//
// The offset is large in absolute terms - the band levels are PSDs in dB, and
// a full-scale tone reads about +74 dB - so the clamp is generous. It is a
// runaway guard, not a range limit.
static void arUpdateAgc(float dtMs) {
  if (!arGateOpen) return;                    // frozen while the gate is shut
  if (!arAutoRange) return;                   // a fixed window does not move
  // The loudest band of the WHOLE set - all thirty-one, not just the sixteen
  // that are drawn.
  //
  // The sixteen bars are a subset of the thirty-one, so a peak hold over the
  // subset would put the top of the window somewhere the RTA view does not
  // have it, and the two views would stop being the same picture. One
  // spectrum, one window, one top.
  float maxDb = -120.0f;
  for (int k = 0; k < NUM_RTA_CHANNELS; k++)
    if (estRtaDb[k] > maxDb) maxDb = estRtaDb[k];
  arAutoRangeOffset(arAgcDb, maxDb, dtMs);
}

// The 16 display bands.
//
// The value is the RTA band's calibrated level - the integrated power over
// that third-octave band - plus the AGC's dB offset and the display's dB
// span, and nothing else. There is no per-band gain, no profile, no histogram
// normalisation and no floor subtraction: every one of those MUTATES the band
// value, and a band value that has been mutated is no longer a measurement.
//
// The smoothing is in dB, because that is the domain the value lives in: a
// bar that rises fast and falls slowly is what a spectrum display looks like,
// and doing it on the byte would make the fall depend on where the bar
// happened to be.
// The user's input-level slider, as a DISPLAY trim.
//
// The old code applied it to the linear magnitude at the very end, which is
// a dB offset of the same size, so that is what it is here. It moves the
// bar, not the measurement: the calibrated band value is what it was before
// the trim, and the info page's "GEQ band level (dB)" row shows that.
//
// Both display views use it - the GEQ's 16 bars and the RTA's 31 - so that
// one slider moves both of them the same amount.
static float arInputTrimDb() {
  float postGain = (float)inputLevel / 128.0f;
  if (postGain < 1.0f) postGain = ((postGain - 1.0f) * 0.8f) + 1.0f;
  if (postGain < 1e-4f) postGain = 1e-4f;
  return 20.0f * log10f(postGain);
}

static void arPublishBands(float dtMs) {
  // The smoothing.
  //
  // BOTH paths use the user's rise and fall, so the switch between them is a
  // comparison of the measurement and not of the display. The bank's content
  // is 5-16 ms old where the transform's is 42.7 ms old, and that difference
  // is the whole of what the bank buys - the smoothing on top of it is the
  // same on both sides.
  const float attack  = (attackTime > 0) ? (float)attackTime : AR_DISP_ATTACK_MS;
  const float release = (decayTime  > 0) ? (float)decayTime : AR_DISP_RELEASE_MS;

  const float trimDb = arInputTrimDb();

  // The window the bars are drawn in - the SAME window the 31-band view uses.
  //
  // AUTO: the loudest band is held at the top, so the window is
  // [-arWindowRangeDb, 0] and the auto-range's offset moves the whole
  // picture. The display trims apply on top, as they always did.
  //
  // FIXED: the window is the user's, in absolute dB, and NOTHING moves it -
  // not the auto-range, not the display trims.
  float winLo, winHi, valueAdj;
  arWindowEdges(TROYHACKS_DBREF + trimDb, winLo, winHi, valueAdj);

  // The frame is assembled here, off to the side. It is only copied into the
  // published buffer under the publish lock, so the lock is held for a memcpy
  // and not for the mapping maths.
  uint8_t frame[NUM_GEQ_CHANNELS];

  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    // How far this band moved since the last frame.
    //
    // This is the freshness measured on the LIVE signal, rather than
    // predicted from the filter's shape: a band whose content is 42.7 ms old
    // cannot move as far between two frames as one whose content is 5 ms old.
    //
    // A band that has no reading yet (-120) does not count as a move: the
    // step from silence to signal is not the signal changing.
    const bool live = (estBandDb[k] > -119.0f) && (dispPrevDb[k] > -119.0f);
    const float move = live ? fabsf(estBandDb[k] - dispPrevDb[k]) : 0.0f;
    dispPrevDb[k] = estBandDb[k];
    dispMoveDb[k] = (dispMoveDb[k] * 7.0f + move) / 8.0f;

    const float db = (arGateOpen && (estBandDb[k] > -119.0f))
                     ? (estBandDb[k] + (arAutoRange ? arAgcDb : 0.0f)) : -120.0f;
    float &sm = arBandSmth[k];
    const float tau = (db > sm) ? attack : release;
    if (dtMs > 0.0f) {
      const float a = 1.0f - expf(-dtMs / tau);
      sm += a * (db - sm);
    } else {
      sm = db;
    }
    if (sm < -120.0f) sm = -120.0f;

    // The display's dB span: 0 dB is the top of the bar. The auto-range holds
    // the loudest band at 0 dB, so the span is how much of the spectrum's
    // shape fits below the top. TROYHACKS_DBREF is the user's trim on top of
    // that - the same job the old linear gain coefficient did, but in dB, and
    // with both ends of its travel usable.
    //
    // The per-band trim is NOT here: it is part of the measurement now, the
    // same as the RTA view's own, so that a fixed window keeps it.
    float v = mapf(sm + valueAdj, winLo, winHi, 0.0f, 255.0f);
    if (v < 0.0f)   v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    fftCalc[k]   = v;
    fftAvg[k]    = v;
    frame[k]     = (uint8_t)(v + 0.5f);
  }

  // Publish: the frame is complete, so copy it into the buffer the effects are
  // NOT reading and then swap the pointer that u_data[2] holds. One aligned
  // pointer store - atomic on both cores.
  {
    ArPublishGuard guard;
    uint8_t *back = fftResultBuf[fftResultIdx ^ 1];
    memcpy(back, frame, NUM_GEQ_CHANNELS);
    fftResultIdx ^= 1;
    fftResult = fftResultBuf[fftResultIdx];
    if (arUmData != nullptr) arUmData->u_data[2] = fftResult;
  }
}

// The 31 third-octave bands, as an analyser's bars.
//
// The RTA set is the MEASUREMENT. The sixteen display bands are sixteen of
// these, so there is one offset for the whole spectrum - arAgcDb, the one
// arUpdateAgc() computes over all thirty-one - and the two views are drawn in
// the same window at the same scale.
//
// The auto-range holds the loudest band at the top of the display, the way a
// hardware analyser does, so the display can be read against one bar for bar.
// It is one dB offset, fast down and slow up, frozen while the gate is shut.
//
// The measurement is ALWAYS the integrated one: an integrated power is what
// an analyser's bar shows, and there is no other candidate for it. The
// sixteen display bands are sixteen of these thirty-one, so they are in the
// same unit and at the same scale as the bars here.
static void arPublishRta(float dtMs) {
  const float attack  = (attackTime  > 0) ? (float)attackTime  : AR_DISP_ATTACK_MS;
  const float release = (decayTime   > 0) ? (float)decayTime   : AR_DISP_RELEASE_MS;
  const float trimDb  = arInputTrimDb();

  // The window the bars are drawn in, and the offsets that go with it. The
  // SAME window the 16-band display uses.
  float winLo, winHi, valueAdj;
  arWindowEdges(TROYHACKS_DBREF + trimDb, winLo, winHi, valueAdj);

  uint8_t frame[NUM_RTA_CHANNELS];

  for (int k = 0; k < NUM_RTA_CHANNELS; k++) {
    // The auto-range's offset is the peak hold's, and it only exists in auto
    // mode - in fixed mode the levels are where they are.
    const float db = (arGateOpen && (estRtaDb[k] > -119.0f))
                     ? (estRtaDb[k] + (arAutoRange ? arAgcDb : 0.0f)) : -120.0f;
    float &sm = arRtaSmth[k];
    const float tau = (db > sm) ? attack : release;
    if (dtMs > 0.0f) {
      const float a = 1.0f - expf(-dtMs / tau);
      sm += a * (db - sm);
    } else {
      sm = db;
    }
    if (sm < -120.0f) sm = -120.0f;

    // The same mapping the GEQ bars use, over the RTA's own window.
    float v = mapf(sm + valueAdj, winLo, winHi, 0.0f, 255.0f);
    if (v < 0.0f)   v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    frame[k] = (uint8_t)(v + 0.5f);
  }

  // Publish, the same way as the GEQ bands: fill the buffer the effects are
  // NOT reading, then swap the pointer. A reader that copies NUM_RTA_CHANNELS
  // bytes always gets one whole frame.
  {
    ArPublishGuard guard;
    uint8_t *back = rtaResultBuf[rtaResultIdx ^ 1];
    memcpy(back, frame, NUM_RTA_CHANNELS);
    rtaResultIdx ^= 1;
    rtaResult = rtaResultBuf[rtaResultIdx];
    // The board-local publication: the effects that draw an analyser read
    // these. Same shape as the buffer above - fill the frame nobody is
    // reading, then swap the index - and it stays off the shared um_data
    // structure, which must not grow.
    memcpy(arRtaBands[arRtaBandsIdx ^ 1], frame, NUM_RTA_CHANNELS);
    arRtaBandsIdx ^= 1;
    arRtaBandsValid = true;
  }
}

// Publish a set of band bytes that came from somewhere else - the UDP sound
// sync receive path, and the dynamics limiter. It goes through the same double
// buffer, so the effects still never see a half-written frame.
static void arPublishRaw(const uint8_t *src) {
  ArPublishGuard guard;
  uint8_t *back = fftResultBuf[fftResultIdx ^ 1];
  memcpy(back, src, NUM_GEQ_CHANNELS);
  fftResultIdx ^= 1;
  fftResult = fftResultBuf[fftResultIdx];
  if (arUmData != nullptr) arUmData->u_data[2] = fftResult;
}

// The normalisation constant: 1/(fs * sum(w^2)).
//
// The whole PSD unit hangs off it, so it is measured from the window buffer
// that is actually applied rather than from the window's formula - the two
// cannot drift apart.
static void estSetNorm(const float *w, int n) {
  double s2 = 0.0;
  for (int i = 0; i < n; i++) s2 += (double)w[i] * (double)w[i];
  estNorm = (float)(1.0 / ((double)SAMPLE_RATE * s2));
}

// Bake the interpolation weights for one layout. base is the first slot.
static void estInitLayout(const BandLayout &L, int base) {
  const double N = (double)samplesFFT;
  for (int k = 0; k < L.count; k++) {
    // u in bins, in double. u reaches 1703 at the top of the RTA set, where a
    // float's ULP is 1.2e-4 bins - enough to put a 0.6% error into the weight
    // of the nearest bin, and the whole point of this estimator is that it
    // agrees with a direct DTFT.
    const double u = (double)L.centre[k] / (double)binWidth;
    const int k0 = (int)(u + 0.5);          // u is positive; round half up
    estK0[base + k] = k0;
    for (int t = 0; t < AR_KERNEL_TAPS; t++) {
      const double d = (double)(k0 + t - AR_KERNEL_HALF) - u;
      double re = 1.0, im = 0.0;
      if (fabs(d) > 1e-9) {
        const double mag = sin(M_PI * d) / (N * sin(M_PI * d / N));
        const double ph  = M_PI * d * (1.0 - 1.0 / N);
        re = mag * cos(ph);
        im = mag * sin(ph);
      }
      estWeight[base + k][2 * t]     = (float)re;
      estWeight[base + k][2 * t + 1] = (float)im;
    }
  }
}

// The crest compensation for the display bands.
//
// This is the TRANSFORM's compensation, and it belongs to the transform's own
// reading of the old half-octave layout: the transform reads the PEAK of the
// point PSD over the band, and the price of the peak is a crest bias: the
// maximum of K effectively independent samples sits H_K = ln K + gamma +
// 1/(2K) times above their mean, and K grows with the band width, so flat
// white noise reads with a ramp across the bands.
//
// The sixteen display bands are sixteen of the thirty-one RTA bands now, and
// the RTA reads an INTEGRATED power, so there is no peak and no crest bias on
// the bars any more.
//
// The compensation is that factor, per band. It is FITTED, not guessed: the
// offline validator measures the bias over 256 frames of white noise and fits
// K = 0.6 * M for a band of M bins - 0.6 rather than the 0.5 the window's ENBW
// would suggest, because adjacent bins are correlated and cluster. The
// residual is 0.11 dB RMS, 0.37 dB peak to peak.
static float estCrestDb(int nbins) {
  const float k = fmaxf(1.5f, 0.6f * (float)nbins);
  const float h = logf(k) + 0.5772156649f + 0.5f / k;   // the harmonic number
  return -10.0f * log10f(h);
}

// The point PSD at one slot's centre, from the FFT's interleaved (re, im)
// spectrum.
static float estPointPsd(const float *spec, int slot) {
  const int k0 = estK0[slot];
  const float *w = estWeight[slot];
  float re = 0.0f, im = 0.0f;
  for (int t = 0; t < AR_KERNEL_TAPS; t++) {
    const int bin = k0 - AR_KERNEL_HALF + t;
    float xr, xi;
    if (bin < 0) {
      // X[-n] = conj(X[n]) for a real frame
      xr =  spec[2 * (-bin)];
      xi = -spec[2 * (-bin) + 1];
    } else if (bin >= samplesFFT) {
      // Past the end of the transform: the spectrum wraps, and because the
      // frame is real the wrapped value is the conjugate of the mirrored one.
      const int m = bin % samplesFFT;
      xr =  spec[2 * m];
      xi = -spec[2 * m + 1];
    } else {
      // Bin 0 is DC, and it is used as it is.
      //
      // An earlier version replaced it with bin 1's COMPLEX value, reasoning
      // that "a residual offset cannot lift the bottom band". That is not a
      // small error: the lowest RTA centre (19.95 Hz) is bin 1.70, so the
      // kernel's tap at bin 0 carries 4.8% of the main tap's weight, and
      // substituting a different bin's value there moved the three lowest
      // centres by +1.26, -0.49 and -0.29 dB - measured on the hardware, and
      // reproduced exactly offline.
      //
      // The place to remove an offset is the chain, and the chain does it: the
      // DC blocker's transfer function is (1 - z^-1)/(1 - R z^-1), whose gain at
      // 0 Hz is exactly zero. The estimator's job is to report the spectrum it
      // is handed, not to be kind about it.
      xr = spec[2 * bin];
      xi = spec[2 * bin + 1];
    }
    re += w[2 * t] * xr - w[2 * t + 1] * xi;
    im += w[2 * t] * xi + w[2 * t + 1] * xr;
  }
  return 2.0f * (re * re + im * im) * estNorm;
}

// One frame's worth of band values, from the FFT's interleaved (re, im)
// spectrum.
//
// THIS IS THE TRANSFORM HALF. Everything it produces needs a spectrum, and
// nothing it produces reaches a bar when the transform is switched off: the
// bands themselves are the banks' (see estFinalise()). What it does produce
// is the transform's own reading of the same audio, kept for the A/B.
//
// Must run while the spectrum is still COMPLEX: the fold that turns it into
// magnitudes throws the imaginary parts away, and the interpolation needs them.
static void estComputeTransform(const float *spec) {
  const int halfN = samplesFFT >> 1;

  // The point PSD at every band centre of both layouts.
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++)
    estPoint[k] = estPointPsd(spec, k);
  for (int k = 0; k < lay31.count; k++)
    estPoint[NUM_GEQ_CHANNELS + k] = estPointPsd(spec, NUM_GEQ_CHANNELS + k);

  // The peak of the point PSD over each display band. This is what the display
  // bands read: a point measurement AT the band centre cannot see a tone that
  // is inside the band but away from the centre - the DTFT there really is
  // -121 dB - so a 5 kHz tone in band 13 (centre 4488 Hz) would light nothing
  // at all. The neighbours read 90 dB down.
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    float best = 0.0f;
    for (int b = estLo[k]; b <= estHi[k]; b++) {
      const float re = spec[2 * b], im = spec[2 * b + 1];
      const float p = 2.0f * (re * re + im * im) * estNorm;
      if (p > best) best = p;
    }
    estPeak[k] = best;
  }

  // The RTA set reads the INTEGRATED power over each band - what a hardware
  // analyser's bar shows. For a band narrower than three bins the integral
  // cannot be taken from bins at all, so it comes from the interpolated
  // spectrum at the centre times the width.
  //
  // This is the TRANSFORM's own answer, and it is kept in rtaTfPower[] - not
  // in estRta[]. estRta[] belongs to the banks now: estFinalise() puts the
  // banks' readings there, and it does so whether or not the transform ran.
  // Keeping the two apart is what lets the comparison run on the same audio
  // in the same frame, and it is what lets the transform be switched off
  // without taking the RTA view down with it.
  //
  // The bottom eight bands are computed here too, even though the transform
  // cannot resolve them: they are the clearest demonstration of why the
  // banks exist. The transform's measurement kernel is 23.49 Hz wide and the
  // narrowest band in the set is 4.62 Hz, so the transform reads a tone at
  // the band's centre as P * (4.62 / 23.49) - nearly 7 dB low - and the
  // A/B shows it.
  for (int k = 0; k < lay31.count; k++) {
    const float fLo = lay31.edgeLo[k], fHi = lay31.edgeHi[k];
    if ((fHi - fLo) >= 3.0f * binWidth) {
      const int lo = max(1, (int)ceilf(fLo / binWidth));
      const int hi = min(halfN - 1, (int)floorf(fHi / binWidth));
      float acc = 0.0f;
      for (int b = lo; b <= hi; b++) {
        const float re = spec[2 * b], im = spec[2 * b + 1];
        acc += re * re + im * im;
      }
      rtaTfPower[k] = 2.0f * acc * estNorm * binWidth;
    } else {
      rtaTfPower[k] = estPoint[NUM_GEQ_CHANNELS + k] * (fHi - fLo);
    }
  }

  // The same integral over the DISPLAY bands, for the "integrated" measurement
  // mode. It is computed every frame whether or not the mode is selected,
  // because it is eight lines and a fraction of the frame, and having it
  // always there means the mode can be switched without a rebuild.
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    const float fLo = lay16.edgeLo[k], fHi = lay16.edgeHi[k];
    if ((fHi - fLo) >= 3.0f * binWidth) {
      const int lo = max(1, (int)ceilf(fLo / binWidth));
      const int hi = min(halfN - 1, (int)floorf(fHi / binWidth));
      float acc = 0.0f;
      for (int b = lo; b <= hi; b++) {
        const float re = spec[2 * b], im = spec[2 * b + 1];
        acc += re * re + im * im;
      }
      estBandInt[k] = 2.0f * acc * estNorm * binWidth;
    } else {
      estBandInt[k] = estPoint[k] * (fHi - fLo);
    }
  }

  // The calibrated levels, in dB. This is where the chain's response comes out,
  // and it is the only place it comes out: everything downstream - the AGC, the
  // bars, the RTA view - reads these numbers and never re-derives a correction
  // of its own.
  //
  // The display band carries the crest compensation as well: its value is the
  // PEAK of the point PSD over the band, and the peak of K effectively
  // independent samples sits H_K above their mean, so without the compensation
  // a flat input would read with a ramp across the bands - the same shape as
  // the ramp the old code had, arrived at from the other direction.
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    // The transform's own reading of this band.
    //
    // The integrated mode reports band POWER, so there is no crest
    // compensation to apply: the sum over the band already includes the
    // spread, and a peak-based compensation on top of it would be wrong.
    const float t = freqMeasureIntegrated
                  ? ((estBandInt[k] > 0.0f)
                       ? 10.0f * log10f(estBandInt[k]) + calDb[k] : -120.0f)
                  : ((estPeak[k] > 0.0f)
                       ? 10.0f * log10f(estPeak[k]) + estComp[k] + calDb[k] : -120.0f);
    // The RAW reading: the same number without the crest compensation, so the
    // live difference between the two paths can be taken apart into the
    // compensation and the peak-versus-mean difference.
    dispTransformRawDb[k] = freqMeasureIntegrated
                          ? ((estBandInt[k] > 0.0f)
                               ? 10.0f * log10f(estBandInt[k]) + calDb[k] : -120.0f)
                          : ((estPeak[k] > 0.0f)
                               ? 10.0f * log10f(estPeak[k]) + calDb[k] : -120.0f);
    // The band-AVERAGE reading, in the same density unit: the mean over the
    // layout's own edges rather than the peak inside them.
    dispTransformMeanDb[k] = (estBandInt[k] > 0.0f)
        ? (10.0f * log10f(estBandInt[k] / (lay16.edgeHi[k] - lay16.edgeLo[k])) + calDb[k])
        : -120.0f;
    dispTransformDb[k] = t;
  }
}

// The transform's measurement kernel's equivalent noise bandwidth, in Hz.
//
// ENBW = fs * sum(w^2) / (sum(w))^2 - the width of the rectangle that passes
// the same noise power as this window does.
//
// It is measured from the window buffer that is actually applied, so it
// cannot drift away from the kernel. At 4096 points of Blackman-Harris and
// 48 kHz it is 23.49 Hz, and every RTA band narrower than that is one the
// transform cannot measure.
static float xfKernelEnbw() {
  double sw = 0.0, sw2 = 0.0;
  for (int i = 0; i < samplesFFT; i++) {
    sw  += (double)windowBuf[i];
    sw2 += (double)windowBuf[i] * (double)windowBuf[i];
  }
  if (sw <= 0.0 || sw2 <= 0.0) return 0.0f;
  return (float)((double)SAMPLE_RATE * sw2 / (sw * sw));
}

// The transform's answer to the question the banks are also asked: "what is
// the power in RTA band k?".
//
// It runs the REAL path - the real window, the real transform, the real
// estimator - on a frame of samples, and returns the reading in dB, BEFORE
// any calibration: the caller adds the calibration its own path uses.
//
// The frame is the last samplesFFT samples of the signal the band path sees,
// at the full rate, windowed exactly as the live path windows it. So the
// transform and the bank are asked the same question about the same signal.
//
// This is used by the self-tests only. It writes the estimator's globals, so
// it must not run while the FFT task is running.
static float xfTransformBandDb(const float *frame, int k) {
  for (int i = 0; i < samplesFFT; i++) {
    fftBuf[2 * i]     = frame[i] * windowBuf[i];
    fftBuf[2 * i + 1] = 0.0f;
  }
  dsps_fft4r_fc32(fftBuf, samplesFFT);
  dsps_bit_rev4r_fc32(fftBuf, samplesFFT);
  estComputeTransform(fftBuf);
  const float p = rtaTfPower[k];
  return (p > 0.0f) ? 10.0f * log10f(p) : -120.0f;
}

// Record one band's head-to-head result.
//
// 'tfDb'    the transform's reading of the tone, dB, already calibrated
// 'bankDb'  the bank's reading of the same tone, dB, already calibrated
// 'toneDb'  the truth: the tone's own power, dB
static void xfRecord(int k, float tfDb, float bankDb, float toneDb) {
  if (k < 0 || k >= NUM_RTA_CHANNELS) return;
  xfTestBands++;
  xfTestHas[k] = true;
  const float tfErr   = tfDb   - toneDb;
  const float bankErr = bankDb - toneDb;
  xfTestTfDb[k]   = tfErr;
  xfTestBankDb[k] = bankErr;
  // What the kernel's width predicts: the transform reads the point PSD at
  // the centre and multiplies it by the band's width, so for a band narrower
  // than the kernel its reading is the tone's power times (width / ENBW).
  const float width = lay31.edgeHi[k] - lay31.edgeLo[k];
  const float enbw  = xfKernelEnbw();
  xfTestPredDb[k]  = (enbw > 0.0f) ? 10.0f * log10f(width / enbw) : 0.0f;
  // Two different tolerances, because the two answers are expected to be
  // different. The bank is a filter whose peak gain is 1.0 at the centre, so
  // it should read a centred tone as that tone's power to a fraction of a dB.
  // The transform's kernel is 23.49 Hz wide whatever the band is, so it is
  // given a dB: the question here is only whether it is in the ballpark.
  if (fabsf(tfErr) > 1.0f) xfTestTfFails++;
  if (fabsf(bankErr) > 0.35f) xfTestBankFails++;
  if (xfTestTfWorstBand < 0 || fabsf(tfErr) > fabsf(xfTestTfWorst)) {
    xfTestTfWorst = tfErr;
    xfTestTfWorstBand = k;
  }
  if (xfTestBankWorstBand < 0 || fabsf(bankErr) > fabsf(xfTestBankWorst)) {
    xfTestBankWorst = bankErr;
    xfTestBankWorstBand = k;
  }
}

// The part of the estimator that does not need a spectrum.
//
// Everything the banks measure is finished here: the RTA bands take the
// banks' readings, the display bands take sixteen of those, and the RTA's dB
// values and the published copy are made.
//
// It runs EVERY FRAME, whether or not the transform ran. That is the whole
// point of the split: the transform's readings are the A/B comparison, and
// the banks' readings are the measurement. Taking the measurement out of the
// function that needs a spectrum is what makes "no FFT at all" possible
// without taking the display and the RTA view down with it.
//
// tfRuns is the frame's answer to "did the transform run", handed in by the
// caller. It is NOT asked for again here: the switch can be thrown from
// another task at any moment, and a frame that asked the question three
// times could get three different answers and disagree with itself.
static void estFinalise(bool tfRuns) {
  // The bottom eight RTA bands come from the filter bank, not from the FFT.
  //
  // The transform's measurement kernel is 23.49 Hz wide and the narrowest band
  // in the set is 4.62 Hz wide, so the FFT cannot separate these bands at all:
  // it would read the same smeared number into five or six of them. The bank
  // gives each band its own filter with its own bandwidth, and it updates
  // EVERY FRAME - which is the whole point, because the narrow path's bank
  // froze for 682.7 ms at a time.
  //
  // The bank reports the SHAPED power - the bandpass's output power, not the
  // power inside the band's nominal edges. That is what a hardware analyser
  // shows, and it is the only form in which these bars can be compared with
  // one bar for bar.
  //
  // The bank's own health is deliberately NOT consulted here. The transform
  // cannot separate these bands at all, so there is nothing to fall back to:
  // a bank that is out of contract by half a decibel is still a far better
  // measurement of a 4.62 Hz band than a 23.49 Hz smeared one. A failed bank
  // is reported; it is not replaced with something worse.
  if (bankReady) {
    for (int k = 0; k < AR_BANK_BANDS; k++) estRta[k] = liveBank.det[k];
  } else if (arTransformValid) {
    for (int k = 0; k < AR_BANK_BANDS; k++) estRta[k] = rtaTfPower[k];
  }

  // The top twenty-three come from their own bank, on the same convention:
  // the SHAPED power - the bandpass's own output power - and the same kernel,
  // so the RTA's thirty-one bands are one measurement all the way up.
  //
  // The bank runs at the full rate and has no anti-alias filter, so its
  // calibration is the chain's alone - which is exactly calDb[]'s RTA half,
  // and is why nothing about the calibration changes when the source does.
  const bool rtaTopActive = arRtaTopBankDrives(tfRuns);
  arFrameSnap.rtaTopBankDrives = rtaTopActive;
  if (rtaTopActive) {
    for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
      estRta[AR_RTA_TOP_FIRST + k] = liveRtaTop.det[k];
    }
  } else if (arTransformValid) {
    for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
      estRta[AR_RTA_TOP_FIRST + k] = rtaTfPower[AR_RTA_TOP_FIRST + k];
    }
  }

  for (int k = 0; k < lay31.count; k++) {
    // The bank's bands carry the bank's own calibration: the chain's response
    // and the anti-alias filter's response are both divided out of them.
    const float cal = (bankReady && k < AR_BANK_BANDS)
                    ? bankCalDb[k] : calDb[NUM_GEQ_CHANNELS + k];
    // ... and then the REFERENCE offset and the per-band trim go on, HERE,
    // on the measurement and not on the display.
    //
    // This is the difference between a calibration and a trim: a calibration
    // corrects the number, so everything that reads the number - the bars, the
    // auto-range, the info page, the serial plot - reads the corrected one.
    // A display trim moves a bar and nothing else, and it is already one
    // layer further out (TROYHACKS_DBREF, the input-level trim).
    estRtaDb[k] = (estRta[k] > 0.0f)
                  ? 10.0f * log10f(estRta[k]) + cal + rtaRefDb + rtaTrimDb[k]
                  : -120.0f;
  }

  // THE SIXTEEN DISPLAY BANDS ARE SIXTEEN OF THE THIRTY-ONE.
  //
  // There is no measurement of their own any more. The value IS the RTA
  // band's value: the integrated power over that third-octave band,
  // calibrated, with the reference offset and the per-band trim already on
  // it. So the sixteen bars and the thirty-one RTA bars are the same
  // numbers, and the sixteen are a subset of them - which is what makes the
  // two views one spectrum at two resolutions.
  //
  // It is also why there is no second trim array. A per-band correction
  // belongs to the BAND, and a band is a band whichever view draws it, so
  // the display's trim is the RTA's rtaTrimDb[] - the sixteen entries that
  // belong to the sixteen bands that are shown.
  //
  // There is no display bank any more: it measured a different quantity in a
  // different unit, at the old half-octave centres, so it did not describe
  // what is drawn. It has been removed.
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    estBandDb[k] = estRtaDb[arGeqFromRta[k]];
  }

  // Publish the RTA values for the serial plot.
  //
  // The plot runs on the main thread and the FFT task rewrites estRtaDb[]
  // every 10.67 ms, so a reader that just looked at estRtaDb[] could see half
  // of one frame and half of the next. The copy is made under the publish
  // lock, and the reader takes the same lock, so the plot always sees ONE
  // frame.
  //
  // What is published is the dB, not the linear power, because the plot wants
  // the CALIBRATED numbers - the ones the info page shows and the ones the
  // analyser is compared against - and it must not re-derive them from the
  // linear power, because the calibration lives in this frame and nowhere else.
  {
    ArPublishGuard guard;
    memcpy(arRtaDbPub, estRtaDb, sizeof(arRtaDbPub));
  }
}

// Copy this frame's published numbers, so a reader on another task can take
// ONE frame's worth of them.
//
// It is called at the end of the frame, after every stage that writes the
// numbers, so the copy cannot be a frame behind. The reader takes the same
// publish lock, so it cannot catch the copy half-done.
//
// The lock is a spin lock and the copy takes a few microseconds, so the
// frame waits for a reader that happens to be in the middle of its copy -
// and the reader waits for the frame. Neither is long.
static void arPublishFrameSnapshot() {
  ArPublishGuard guard;
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    arFrameSnap.bandDb[k] = estBandDb[k];
    // The transform's own reading of the old half-octave layout, so the
    // endpoint's "transform_bands" object is one frame's worth of it.
    arFrameSnap.dispTransformDb[k]     = dispTransformDb[k];
    arFrameSnap.dispTransformRawDb[k]  = dispTransformRawDb[k];
    arFrameSnap.dispTransformMeanDb[k] = dispTransformMeanDb[k];
    // The RAW band power: the integrated power over the band, before the
    // calibration, the reference offset and the trim go on. It is the RTA
    // band's own number - the display bands ARE RTA bands - so the two are
    // the same measurement seen from two sides.
    const float p = estRta[arGeqFromRta[k]];
    arFrameSnap.rawDb[k] = (p > 0.0f) ? 10.0f * log10f(p) : -99.0f;
  }
  for (int k = 0; k < AR_MAX_BANDS; k++) {
    arFrameSnap.rtaPowDb[k]   = (estRta[k] > 0.0f) ? 10.0f * log10f(estRta[k]) : -99.0f;
    arFrameSnap.rtaLevelDb[k] = estRtaDb[k];
  }
  for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
    const int j = AR_RTA_TOP_FIRST + k;
    arFrameSnap.rtaTopPowLin[k] = liveRtaTop.det[k];
    arFrameSnap.rtaTopTfLin[k]  = rtaTfPower[j];
    arFrameSnap.rtaTopPowDb[k] = (liveRtaTop.det[k] > 0.0f) ? 10.0f * log10f(liveRtaTop.det[k]) : -99.0f;
    arFrameSnap.rtaTopTfDb[k]  = (rtaTfPower[j]   > 0.0f) ? 10.0f * log10f(rtaTfPower[j])   : -99.0f;
  }
}

// ---------------------------------------------------------------------------
// THE DOMINANT FREQUENCY AND THE PEAK MAGNITUDE, FROM THE BANKS.
//
// With no transform there is no spectrum to find a peak in, so the banks
// answer the question instead - and the question is a loose one.
//
// Every effect that reads FFT_MajorPeak takes its log and maps it to a pixel
// or a colour index: mode_freqmap() does log10f(FFT_MajorPeak) and scales it
// onto the segment, mode_freqpixels() and mode_freqwave() do the same for the
// colour, mode_gravfreq() and mode_rocktaves() for the position. NONE of them
// needs the frequency to be better than the band it came from - a band centre
// IS a frequency, and it lands in the same octave as the transform's answer.
//
// The DENSITY is compared, not the power. The transform's peak was the
// loudest BIN, and a bin is a density measurement - all the bins are the same
// width, so comparing them compares densities. A band's POWER grows with its
// width, so comparing powers would always pick the widest band and always
// answer "the bottom of the spectrum".
//
// The density is the band's shaped power over its ENBW. That is the same
// quantity, in the same units (power per Hz), as the estimator's point PSD:
// for an input of density S, a filter of ENBW E passes S*E, so the band's
// power over E is S.
//
// The MAGNITUDE is the transform's own measurement brought back onto the
// legacy scale. The transform's magnitude was the largest FFT bin, |X(k)|,
// times FFT_BIN_SCALE; and the estimator's point PSD is that same bin
// magnitude in density form:
//
//     S1 = 2 * |X|^2 * estNorm      =>      |X| = sqrt(S1 / (2 * estNorm))
//
// so the bank can produce the transform's magnitude from its density without
// knowing anything about the window: estNorm carries the window's
// normalisation, and it is computed at init whether or not the transform ever
// runs.
//
// What this cannot be is the transform's magnitude to the decibel, and it is
// worth saying why. The transform's kernel is the WINDOW - 23.49 Hz wide -
// and the bank's kernel is the BAND, 3.9 Hz wide at the bottom and 4 000 Hz
// wide at the top. For a signal spread over the spectrum the two read the
// same density, because a spread spectrum has no detail for a narrow kernel
// to resolve. For a single TONE they differ by the ratio of the two kernel
// widths, and the bank is the one that resolves it: the tone's power is
// spread over 3.9 Hz by the bank and over 23.49 Hz by the window. That is
// the whole point of the banks, and it is not a defect of the comparison.
//
// The band's ENBW, in Hz - the bottom eight from the bottom bank, the top
// twenty-three from the top bank.
static float arBandEnbw(int k) {
  if (k < AR_BANK_BANDS) return bankEnbw[k];
  const int j = k - AR_RTA_TOP_FIRST;
  if (j < 0 || j >= AR_RTA_TOP_BANDS) return 0.0f;
  return rtaEnbw[j];
}

// The band's density, in dB - its shaped power over its ENBW.
static float arBandDensityDb(int k) {
  const float e = arBandEnbw(k);
  if (e <= 0.0f || estRta[k] <= 0.0f) return -120.0f;
  return 10.0f * log10f(estRta[k] / e);
}

// The bank's magnitude at an arbitrary frequency, on the transform's scale.
//
// The beat detector asks for "the magnitude in bin n", and with the
// transform that was vReal[n] - the FFT's magnitude in that bin. With no
// transform there are no bins, so the answer is the bank's magnitude at that
// bin's frequency: the density is interpolated in dB between the two RTA
// bands that bracket it, because a spectrum moves in dB against log
// frequency and not in dB per Hz.
//
// At a band's own centre the interpolation is exact - it returns that band's
// density and no other.
static float arBankBinMag(float fHz) {
  if (!bandLayoutReady || estNorm <= 0.0f) return 0.0f;
  if (fHz < lay31.centre[0]) fHz = lay31.centre[0];
  if (fHz > lay31.centre[lay31.count - 1]) fHz = lay31.centre[lay31.count - 1];

  // Which band? The centres are 10^((13+k)*0.1), so the index follows from
  // the decade - no search, no loop.
  int k = (int)floorf((log10f(fHz) - 1.3f) * 10.0f + 0.5f);
  if (k < 0) k = 0;
  if (k > lay31.count - 1) k = lay31.count - 1;

  float dens;
  const float d0 = arBandDensityDb(k);
  if (k >= lay31.count - 1) {
    dens = powf(10.0f, d0 * 0.1f);
  } else {
    const float d1 = arBandDensityDb(k + 1);
    const float l0 = log10f(lay31.centre[k]);
    const float l1 = log10f(lay31.centre[k + 1]);
    const float t = (log10f(fHz) - l0) / (l1 - l0);
    dens = powf(10.0f, (d0 + (d1 - d0) * t) * 0.1f);
  }
  return FFT_BIN_SCALE * sqrtf(dens / (2.0f * estNorm));
}

// The bank's dominant frequency and peak magnitude.
//
// Computed every frame, so the A/B is live.
static void arBankPeak() {
  if (!bandLayoutReady) return;

  int   bestBand = -1;
  float bestDb   = -1e30f;
  for (int k = 0; k < lay31.count; k++) {
    if (estRta[k] <= 0.0f) continue;          // no reading in this band
    const float d = arBandDensityDb(k);
    if (d > bestDb) { bestDb = d; bestBand = k; }
  }

  if (bestBand < 0) {
    // Nothing in the spectrum at all.
    arBankPeakHz = 0.0f;
    arBankMagnitude = 0.0f;
    if (!arTransformValid) {
      FFT_MajorPeak = 1.0f; FFT_Magnitude = 0.0f;
      // The smoother runs on EVERY frame in the transform path, so it has to
      // run here too. Left frozen, it would publish the last peak for the
      // whole silence - the effects would read a peak frequency that is not
      // there, and it would never come back.
      FFT_MajPeakSmth = FFT_MajPeakSmth + 0.42f * (FFT_MajorPeak - FFT_MajPeakSmth);
    }
    return;
  }

  const float f = lay31.centre[bestBand];
  arBankPeakHz = f;

  // The magnitude, on the transform's scale.
  float mag = 0.0f;
  if (estNorm > 0.0f) {
    const float dens = estRta[bestBand] / arBandEnbw(bestBand);
    mag = FFT_BIN_SCALE * sqrtf(dens / (2.0f * estNorm));

    // The same pink correction the transform applies to its peak magnitude,
    // so the two are the same shape and not just the same units.
    if ((f > (binWidth/1.25f)) && (f < (SAMPLE_RATE/2.2f)) && (mag > 4.0f)) {
      const unsigned peakBin = constrain((int)((f + binWidth/2.0f) / binWidth), 0, samplesFFT - 1);
      float peakFreq = peakBin * binWidth + binWidth/2.0f;
      const float fold = SAMPLE_RATE * 0.42f;
      if (peakFreq > fold) peakFreq = fold - 0.25f * (peakFreq - fold); // suppress noise and aliasing
      if (peakFreq > 0.0f) mag *= fmaxf(pinkcenter / sqrtf(peakFreq), 1.0f);
    }
  }
  arBankMagnitude = mag;

  if (!arTransformValid) {
    // The transform produced no answer this frame, so the banks answer for it.
    //
    // The test is the transform's OWN validity flag, not the switch: the
    // transform can be switched on and still produce nothing (a bank that
    // has failed its self-test turns it off again), and in that case the
    // bank's answer is the one the effects should get.
    FFT_MajorPeak = constrain(f, 1.0f, NYQUIST_HZ);
    FFT_Magnitude = mag;
    FFT_MajPeakSmth = FFT_MajPeakSmth + 0.42f * (FFT_MajorPeak - FFT_MajPeakSmth);   // I like this "swooping peak" look
  }
}

// Zero the estimator's TRANSFORM values - this frame had no spectrum to
// measure.
//
// estRta[] is NOT cleared: it belongs to the banks now, and the banks update
// every frame whether or not the transform ran. Clearing it here would take
// the RTA view down every time the transform was skipped.
static void estClearTransform() {
  for (int k = 0; k < AR_EST_SLOTS; k++) estPoint[k] = 0.0f;
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) { estPeak[k] = 0.0f; estBandInt[k] = 0.0f; }
  for (int k = 0; k < NUM_RTA_CHANNELS; k++) rtaTfPower[k] = 0.0f;
  for (int k = 0; k < AR_DISP_BANDS; k++) {
    dispTransformDb[k] = -120.0f;
    dispTransformRawDb[k] = -120.0f;
    dispTransformMeanDb[k] = -120.0f;
  }
  arTransformValid = false;
}

// ---------------------------------------------------------------------------
// The estimator's self-test: the REAL estimator on a REAL tone.
//
// The offline validator proves the estimator against a direct DTFT, in Python.
// This is the same check on the hardware: the ESP-DSP transform, the real
// window, the real baked weights.
//
// The unit contract says a tone of power P reads P/ENBW, so for a tone of
// peak amplitude A (power A^2/2) the expected point PSD is
//
//   S1 = A^2 * (sum w)^2 / (2 * fs * sum w^2)
//
// and every band centre has to read exactly that. The display bands also have
// to leave their neighbours alone: a tone at one band centre must not light
// the next one.
//
// It runs in the FFT task, before the first frame, because the transform is
// not reentrant and the P4's FFT peripheral has one set of registers.
// ---------------------------------------------------------------------------
static volatile bool  estTestDone = false;
static volatile bool  estTestSkipped = false;
static int   estTestFails = 0;
static float estTestWorst = 0.0f;     // worst |error| over the centres, dB
static float estTestWorstF = 0.0f;    // and the frequency it happened at
static int   estTestNbrFails = 0;     // neighbour checks that failed
static int   estTestNbrChecks = 0;    // neighbour checks that were decidable
static float estTestWorstNbr = 0.0f;  // worst neighbour disagreement, dB
static int   estTestUnresolvable = 0; // neighbour pairs the kernel cannot separate

// The truth for the self-test: the DTFT of the frame itself, summed directly.
//
// This is deliberately blind to everything the estimator uses. It does not know
// the window's formula, the interpolation weights, the normalisation or the
// transform - it is the sum. That matters, because the estimator's only
// approximation is the truncation of the Dirichlet kernel, and a reference that
// shared any of the estimator's machinery could not see it.
//
// The rotation is a recurrence, renormalised every 64 samples: an un-normalised
// rotation's phase error grows with the sample count, and this runs over 4096
// samples at frequencies up to 9.2 kHz.
//
// It runs in SINGLE precision, and that is a deliberate choice rather than a
// shortcut. The P4 has a single-precision FPU and no double-precision one, so
// every double operation here is a soft-float library call - and this routine
// runs 141 times over 4096 samples at every boot. The renormalisation removes
// the magnitude drift, so what is left is a phase error of a few microradians
// over the whole window: a frequency error of well under a millihertz, which
// is a hundredth of a bin. The result is used as the TRUTH for the neighbour
// checks, whose tolerance is 1 dB; the error here is a hundredth of a
// millidecibel.
//
// frame is the interleaved (re, im) buffer the transform is about to consume -
// only the real slots hold samples. It must be called BEFORE the transform
// overwrites it.
static double estDirectPsd(const float *frame, double fHz) {
  const float dw = (float)(-2.0 * M_PI * fHz / (double)SAMPLE_RATE);
  const float cw = cosf(dw), sw = sinf(dw);
  float cr = 1.0f, ci = 0.0f;
  float re = 0.0f, im = 0.0f;
  for (int n = 0; n < samplesFFT; n++) {
    re += frame[2 * n] * cr;
    im += frame[2 * n] * ci;
    const float nr = cr * cw - ci * sw;
    ci = cr * sw + ci * cw;
    cr = nr;
    if ((n & 63) == 63) {
      const float m = sqrtf(cr * cr + ci * ci);
      cr /= m;
      ci /= m;
    }
  }
  return (double)(2.0f * (re * re + im * im) * estNorm);
}

static void estimatorSelfTest() {
  if (estTestDone) return;
  estTestDone = true;
  if (!bandLayoutReady) initBandLayout();
  if (estNorm <= 0.0f || windowBuf == nullptr || fftBuf == nullptr) {
    estTestSkipped = true;
    USER_PRINTLN("AR: estimator self-test skipped - no window");
    return;
  }

  double sumW = 0.0, sumW2 = 0.0;
  for (int i = 0; i < samplesFFT; i++) {
    sumW  += (double)windowBuf[i];
    sumW2 += (double)windowBuf[i] * (double)windowBuf[i];
  }
  const double A = 0.5;
  const double expected = A * A * sumW * sumW / (2.0 * (double)SAMPLE_RATE * sumW2);

  USER_PRINTLN("=== AudioReactive estimator self-test ==========================");
  USER_PRINTF("    a %.2f-amplitude tone reads %.6g units^2/Hz at its own centre\n",
              (double)A, expected);
  const uint64_t tStart = esp_timer_get_time();

  int fails = 0;
  float worst = 0.0f, worstF = 0.0f;
  int nbrFails = 0, nbrChecks = 0, unresolvable = 0;
  float worstNbr = 0.0f;
  // Where the self-test's time went. It runs before the first frame is
  // published, so its cost IS the display's waiting time, and a number on the
  // log is worth more than an argument about where it goes.
  uint64_t genUs = 0, truthUs = 0, xformUs = 0;
  const int total = NUM_GEQ_CHANNELS + lay31.count;
  for (int slot = 0; slot < total; slot++) {
    const bool isRta = (slot >= NUM_GEQ_CHANNELS);
    const int k = isRta ? slot - NUM_GEQ_CHANNELS : slot;
    const float f = isRta ? lay31.centre[k] : lay16.centre[k];

    // A synthetic tone at the band centre, windowed exactly as the live path
    // windows it, and transformed exactly as the live path transforms it:
    // a COMPLEX transform of samplesFFT points, the frame in the real slots.
    //
    // The tone is generated by ROTATION, not by a cos() per sample. The old
    // form asked libm for 4096 double-precision cosines per slot, and there
    // are 78 slots: on a core with no double FPU that was most of this
    // self-test's cost. The rotation is the recurrence the truth computation
    // uses, in single precision, renormalised every 64 samples - the magnitude
    // is exact, and the phase error over the window is a few microradians.
    uint64_t t0 = esp_timer_get_time();
    {
      const float dw = (float)(2.0 * M_PI * (double)f / (double)SAMPLE_RATE);
      const float cw = cosf(dw), sw = sinf(dw);
      float cr = 1.0f, ci = 0.0f;
      for (int n = 0; n < samplesFFT; n++) {
        fftBuf[2*n]     = (float)A * cr * windowBuf[n];
        fftBuf[2*n + 1] = 0.0f;
        const float nr = cr * cw - ci * sw;
        ci = cr * sw + ci * cw;
        cr = nr;
        if ((n & 63) == 63) {
          const float m = sqrtf(cr * cr + ci * ci);
          cr /= m;
          ci /= m;
        }
      }
    }
    genUs += esp_timer_get_time() - t0;

    // The truth, taken from the frame itself before the transform overwrites
    // it: the DTFT at this band's centre, and at the centres of its two
    // neighbours. Only the display bands have neighbours worth checking - the
    // RTA's bottom five centres are 0.4 bins apart and share one measurement.
    double ownRef = 0.0;
    double nbrRef[2] = {0.0, 0.0};
    int    nbrIdx[2] = {-1, -1};
    t0 = esp_timer_get_time();
    if (!isRta) {
      ownRef = estDirectPsd(fftBuf, (double)f);
      for (int s = 0; s < 2; s++) {
        const int j = (s == 0) ? k - 1 : k + 1;
        if (j < 0 || j >= NUM_GEQ_CHANNELS) continue;
        nbrIdx[s] = j;
        nbrRef[s] = estDirectPsd(fftBuf, (double)lay16.centre[j]);
      }
    }
    truthUs += esp_timer_get_time() - t0;

    t0 = esp_timer_get_time();
    dsps_fft4r_fc32(fftBuf, samplesFFT);
    dsps_bit_rev4r_fc32(fftBuf, samplesFFT);
    estComputeTransform(fftBuf);
    xformUs += esp_timer_get_time() - t0;

    const float got = estPoint[slot];
    const float errDb = (got > 0.0f) ? 10.0f * log10f(got / (float)expected) : -99.0f;
    const bool ok = (errDb > -0.15f) && (errDb < 0.15f);
    if (!ok) fails++;
    if (fabsf(errDb) > fabsf(worst)) { worst = errDb; worstF = f; }

    USER_PRINTF("  %s %8.2f Hz   point %+7.3f dB %s\n", isRta ? "RTA" : "GEQ",
                (double)f, (double)errDb, ok ? "PASS" : "FAIL");

    // The neighbours.
    //
    // The old criterion was "a tone at one centre must leave the next centre
    // 20 dB down", and it was unachievable at the bottom: the measurement
    // kernel's ENBW is 23.49 Hz and the bottom display band centres are 18.1
    // Hz apart, so a tone at 41.89 Hz is physically inside band 1's main
    // lobe. No reading could have satisfied it.
    //
    // What CAN be asserted is that the estimator agrees with the truth about
    // how much the neighbour sees - and that, where the physics allows the
    // bands to be separated at all, the neighbour really is at least 20 dB
    // down. The truth is the frame's own DTFT, so the assertion is about the
    // estimator and not about a model of the window.
    for (int s = 0; s < 2; s++) {
      const int j = nbrIdx[s];
      if (j < 0) continue;
      const float isoEst = (estPoint[j] > 0.0f && got > 0.0f)
                           ? 10.0f * log10f(estPoint[j] / got) : -99.0f;
      const float isoRef = (nbrRef[s] > 0.0 && ownRef > 0.0)
                           ? 10.0f * log10f((float)(nbrRef[s] / ownRef)) : -99.0f;
      const char *verdict;
      bool nOk = true;
      if (isoRef > -20.0f) {
        // The two centres are closer together than the kernel is wide: the
        // neighbour sees the tone's own skirt, and no measurement at this
        // transform length could say otherwise.
        verdict = "unresolvable";
        unresolvable++;
      } else if (isoRef > -80.0f) {
        // Above the estimator's own truncation floor, so the estimator has to
        // agree with the truth: the truncated Dirichlet sum reproduces the
        // frame's spectrum to within a fraction of a dB while the value it is
        // reading is still well above the noise.
        nOk = (fabsf(isoEst - isoRef) <= 1.0f) && (isoEst <= -20.0f);
        verdict = nOk ? "PASS" : "FAIL";
        nbrChecks++;
        if (fabsf(isoEst - isoRef) > fabsf(worstNbr)) worstNbr = isoEst - isoRef;
      } else {
        // Below that floor: the truth is at the level where the truncated
        // kernel's own leakage (the tone arriving through the tail of the
        // sum) dominates, so an absolute comparison is meaningless. The tone
        // must still not light the neighbour.
        nOk = (isoEst <= -20.0f);
        verdict = nOk ? "PASS" : "FAIL";
        nbrChecks++;
      }
      if (!nOk) nbrFails++;
      USER_PRINTF("         neighbour %8.2f Hz  %+7.1f dB (truth %+7.1f dB)  %s\n",
                  (double)lay16.centre[j], (double)isoEst, (double)isoRef, verdict);
    }
  }
  estTestFails = fails;
  estTestWorst = worst;
  estTestWorstF = worstF;
  estTestNbrFails = nbrFails;
  estTestNbrChecks = nbrChecks;
  estTestWorstNbr = worstNbr;
  estTestUnresolvable = unresolvable;
  USER_PRINTF("  %d of %d centres FAIL, worst %+0.3f dB at %.2f Hz\n",
              fails, total, (double)worst, (double)worstF);
  USER_PRINTF("  %d of %d neighbour checks FAIL, worst disagreement %+0.3f dB\n",
              nbrFails, nbrChecks, (double)worstNbr);
  if (unresolvable) {
    USER_PRINTF("  %d neighbour pairs unresolvable - the kernel is wider than the band gap\n",
                unresolvable);
  }
  USER_PRINTF("  self-test time: tone %.1f ms, truth %.1f ms, transform %.1f ms, wall %.1f ms\n",
              (double)genUs / 1000.0, (double)truthUs / 1000.0, (double)xformUs / 1000.0,
              (double)(esp_timer_get_time() - tStart) / 1000.0);
  USER_PRINTLN("=== end of estimator self-test ==================================");
  USER_FLUSH();
}

// The per-bin band index arrays are GONE.
//
// bandLo[], bandHi[], bandSubBin[], bandNpts[], subBinMag[] and
// bandPowerScale[] described how to read a band OFF THE FFT BINS: which bins
// belong to the band, whether the band was narrower than a bin, how many DFT
// sample points to integrate across it, and the scale factor that put the two
// paths in the same units.
//
// None of that is needed any more. The estimator evaluates the frame's DTFT
// directly at the band centre (Dirichlet interpolation of the FFT bins), so
// there is no "narrower than a bin" case, no second path, and no unit
// reconciliation between two paths. The bands' frequencies and edges live in
// the BandLayout structs (lay16, lay31); the values live in estBandDb[].
//
// dftTwiddle[], dftOffset[] and dftRows[] went with them: they were the
// precomputed exp(j*2*pi*f*n/fs) rows for the direct-DFT integral that
// computed the sub-bin bands. 64 KB of PSRAM and a second copy of the
// spectrum, gone.

// ---------------------------------------------------------------------------
// Input chain, and the calibration that inverts it
// ---------------------------------------------------------------------------
// These two define the signal path that runs before the FFT. They live at file
// scope, not inside the FFT task, so initChainCalibration() below can evaluate
// their response from the same numbers the filters use - otherwise the profile
// and the filter could drift apart silently.
//
// Both are specified by the -3 dB corner they place in the signal path, not as
// bare coefficients. A coefficient like 0.990 only means "35 Hz" at one particular
// SAMPLE_RATE: change the rate and the DC blocker silently slides up into band 1,
// taking the bass with it. dcBlockerRFor() below solves the coefficient from the
// corner every time, so a corner in Hz is the thing you set.
//
// The DC blocker's corner is NOT tied to BAND_F_LOW, which it used to be. It sat
// at 0.873 * BAND_F_LOW = 34.92 Hz, "just below the bottom band", and the intent
// was to leave band 0 alone - but a one-pole highpass is only 3 dB down AT its
// corner, so 5 Hz below the band edge is not "just below", it is most of the way
// to it. That put the blocker inside band 0, attenuating it by 2.45 dB at 40 Hz
// and 1.85 dB at the 47.7 Hz centre, with 1.08 dB of tilt across the band. It
// also made the "Auto" profile invert a 2 dB dip, which amplifies the noise
// floor to get there.
//
// At 20 Hz the corner is a clear octave below anything measured, so band 0 comes
// out essentially flat: 0.95 dB at its lower edge, 0.69 at centre, 0.47 dB of
// tilt. A true DC blocker has zero gain at 0 Hz for any R < 1, so DC removal is
// unaffected by where the corner sits - this only changes how much of the
// 20-40 Hz rumble passes through, and no band lives up there. The only cost is
// settling, 40 ms to 1% against 23 ms, which is irrelevant on a filter that runs
// continuously from startup. Lower it further (15 or 10 Hz) if band 0 still
// looks tilted; the settling cost stays harmless and nothing else moves.
//
// The corner is now 8 Hz, which is 0.4 x the lowest centre of the highest
// layout (19.95 Hz). At 20 Hz the blocker was still inside the RTA's bottom
// band: -3.0 dB at 19.95 Hz, so the correction had to lift that band by 3 dB
// and the band with the least signal in it was the one most amplified. At 8 Hz
// the cost is -0.65 dB at 19.95 Hz and -0.15 dB at 41.9 Hz, and the settling
// time (60 ms to 1%) is still irrelevant on a filter that runs continuously.
constexpr float DC_BLOCKER_HZ = 8.0f;                        // -3 dB corner, Hz
constexpr float INPUT_FILTER_Q = 0.734f;

// Coefficient for the one-pole DC blocker y[n] = (x[n]-x[n-1]) + R*y[n-1], solved
// from its -3 dB corner. |H|^2 = 0.5 reduces to cos(w) = (3-R^2)/(4-2R); solving
// that quadratic and keeping the root below 1 gives the expression below.
static inline float dcBlockerRFor(float fHz) {
  const float c = cosf(2.0f * (float)M_PI * fHz / (float)SAMPLE_RATE);
  return c - sqrtf((c - 1.0f) * (c - 3.0f));
}
static const float DC_BLOCKER_R = dcBlockerRFor(DC_BLOCKER_HZ);

static float inputLpfHz = 0.0f;                             // set by initBandLayout()
static float coeffs_lpf[5];                                 // generated in initChainCalibration()

// ---------------------------------------------------------------------------
// The input chain, as an object.
//
// The chain is the part of the signal path that runs before the FFT: a one-pole
// DC blocker, then a biquad lowpass. It used to live as file-scope state - a
// static xm1/ym1 pair inside runDCBlocker(), a static w_lpf[] delay line
// inside FFTcode() - which made a second copy of it impossible. That matters,
// because the only way to find out whether the chain does what the model says
// it does is to run the chain and measure it, and you cannot measure the live
// chain without disturbing it.
//
// Everything the chain needs is in one struct now, so the live chain and the
// self-test's chain are the same code on different state. chainProcess() below
// is the ONLY filter implementation - there is no second copy to drift.
//
// The coefficients are deliberately NOT in the struct: coeffs_lpf[] is global
// and shared with the "Auto" profile, and the self-test wants to measure the
// chain that will actually run, not a copy of its coefficients.
// ---------------------------------------------------------------------------
struct InputChain {
  float dc_x1 = 0.0f;                                    // DC blocker: x[n-1]
  float dc_y1 = 0.0f;                                    // DC blocker: y[n-1]
  float lpf_w[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};       // biquad delay line
  bool  dcOn  = false;                                   // DC blocker in the chain?
  bool  lpfOn = false;                                   // input LPF in the chain?
};

// The chain the FFT task runs. Its two flags are refreshed from the live
// settings every frame, so the 'b'/'w' A/B switches still work.
static InputChain liveChain;

// Start a chain from scratch: no history, and the LPF's delay line cleared.
static void chainInit(InputChain *c, bool dcOn, bool lpfOn) {
  c->dc_x1 = 0.0f;
  c->dc_y1 = 0.0f;
  for (int i = 0; i < 5; i++) c->lpf_w[i] = 0.0f;
  c->dcOn  = dcOn;
  c->lpfOn = lpfOn;
}

// Run n samples through the chain, in place.
//
// scratch is the biquad's output buffer: ESP-DSP's biquad has no in-place
// mode, so it needs somewhere else to write. scratch must hold n floats and
// must not overlap buf.
//
// The DC blocker is float throughout. Its IIR pole is at z = DC_BLOCKER_R, so
// the pole magnitude is < 1 by ~0.006 and float32 precision is enough to keep
// the noise floor from drifting: rounding error per step is eps * |ym1| /
// (1 - R) ~ 175 * eps * |ym1|, well below 1e-4 for any normal |ym1| < 1.
// SR_HIRES_TYPE (double) was used here historically; on a single-precision FPU
// it costs ~2 emulated FMAs per sample (~300 us/frame at samplesFFT=2048) for
// no measurable benefit.
static void chainProcess(InputChain *c, float *buf, int n, float *scratch) {
  if (c->dcOn) {
    for (int i = 0; i < n; i++) {
      const float value = buf[i];
      const float filtered = (value - c->dc_x1) + DC_BLOCKER_R * c->dc_y1;
      c->dc_x1 = value;
      c->dc_y1 = filtered;
      buf[i] = filtered;
    }
  }
  if (c->lpfOn) {
    dsps_biquad_f32(buf, scratch, n, coeffs_lpf, c->lpf_w);
    memcpy(buf, scratch, sizeof(float) * n);
  }
}

// The "frequency profile" tables are GONE.
//
// The old code had a per-band gain applied to the measured spectrum, chosen
// from a set of hand-measured microphone profiles (fftResultPink[][]) or
// "derived" at runtime (derivedPink[], binEqualise[]). Both are the same idea:
// a per-band gain that mutates the band value, so what the bar shows is no
// longer a measurement of the signal.
//
// The new path corrects the chain in dB, once, from the chain's MEASURED
// impulse response (estInitCalibration()), and applies it as an additive dB
// offset to the band level - not as a gain on a magnitude. A tone at a band
// centre therefore reads the same number whatever the band width and whatever
// the chain did to it, and a flat input reads flat.
//
// The old tables are not converted to dB offsets here: they were fitted to the
// OLD estimator's scale (an unnormalised band sum), so re-expressing them
// against the new unit would be a guess. Re-measuring a microphone against the
// new path is a separate job, and until then the profile dropdown has nothing
// honest to offer.

// Create FFT object

#define sqrt(x) sqrtf(x)             // little hack that reduces FFT time by 10-50% on ESP32 (as alternative to FFT_SQRT_APPROXIMATION)
#define sqrt_internal sqrtf          // see https://github.com/kosme/arduinoFFT/pull/83

// Helper functions

// float version of map()
static float mapf(float x, float in_min, float in_max, float out_min, float out_max){
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// The old bin-averaging helpers are GONE.
//
// fftAddAvgLin(), fftAddAvgRMS(), fftAddAvg() and fftBandEnergy() all worked
// on the raw bin magnitudes: a sum or a mean of |X[k]| over a set of bins.
// Three things are wrong with that, and they are the reasons the whole
// estimator was replaced:
//
//   - a mean divides by the bin count, so every wide band reads 1/sqrt(N) low
//     - a pure measurement artifact, worth 7.8x between band 1 and band 14;
//   - a sum grows with the bin count, which is the 17.9 dB ramp across the
//     bands that used to be written up in audio_source.h;
//   - neither is a PSD, so neither can be put in dB against a known scale.
//
// The new estimator evaluates the DTFT at the band centre (see the Dirichlet
// interpolation below) and reports 10*log10 of a normalised one-sided PSD.
// manualGainFactor() went with them: the input-level slider is now a dB
// display trim applied in arPublishBands(), and the "gain" config key is a
// deprecated no-op.

// Build the 16 constant-Q band layout. Derived, not hand-typed, so it follows
// SAMPLE_RATE and samplesFFT automatically. Call once after buffers exist.
//
// The bottom band gets a FINITE lower edge at BAND_F_LOW, exactly like an IEC
// 61260 fractional-octave analyser (whose lowest 1/3-octave band is 20-25 Hz,
// never running down to DC). Anchoring the layout at 0 Hz instead looks
// harmless but is not: a log layout always puts the bottom band's lower edge at
// DC, and no analysis window can measure content near 0 Hz - at 512 points a
// 23 ms window sees essentially nothing below a few Hz. That band then
// under-reads by 10 dB, and no amount of extra transform length fixes it
// (measured: still -6.9 dB at 4096 points, because the limit is the band
// running into DC, not the resolution).
//
// With edges at BAND_F_LOW * r^k and centres at the geometric midpoints, every
// band has the same Q and band 0 is a real band you can actually measure.
static void initBandLayout() {
  // The two layouts. lay16 is the one the effects read; lay31 is the RTA's
  // scale, built here so that both come off the same audio in the same frame.
  layoutGeometric(lay16, NUM_GEQ_CHANNELS, BAND_F_LOW, BAND_F_HIGH);
  layoutISOThirdOctave(lay31);

  // The estimator. Both layouts get their interpolation weights baked here,
  // and the display bands get the bin range their peak is taken over.
  //
  // The display bands' bin range is the bins INSIDE the band edges, not the
  // bins the old nearest-centre assignment gave them: that assignment hands
  // band 0 the DC-adjacent bins as well, which are below the band and would
  // put a tone that is not in the band into the peak.
  estInitLayout(lay16, 0);
  estInitLayout(lay31, NUM_GEQ_CHANNELS);
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    const int lo = max(1, (int)ceilf(lay16.edgeLo[k] / binWidth));
    const int hi = min((samplesFFT >> 1) - 1, (int)floorf(lay16.edgeHi[k] / binWidth));
    estLo[k] = lo;
    estHi[k] = hi;
    estComp[k] = estCrestDb(hi - lo + 1);
  }

  // The input LPF rides the TOP of the highest layout - the RTA's top edge -
  // and not the top of the 16-band display layout. That is the whole point of
  // the change: the old corner (1.077 x the display's top centre, 9963 Hz) sat
  // INSIDE band 15 (7695-11025 Hz), so band 15 fell from -0.64 dB to -82 dB
  // across its own width and the top band was dead by construction - and it
  // forced the bin-dropping hack in EQ_SIGNAL_FLOOR.
  //
  // At the RTA's top edge (22396 Hz) the filter is flat to -0.01 dB at the RTA's
  // top centre (19952 Hz) and -2.65 dB at its top edge, and the 16-band layout
  // does not touch it at all.
  inputLpfHz = lay31.edgeHi[lay31.count - 1];

  bandLayoutReady = true;
}

// SQUARED magnitude of the windowed DFT, evaluated against one precomputed row
// of twiddles from dftTwiddle[].
//
// For a band narrower than one FFT bin this is the only honest way to measure
// it. Reading the nearest bin instead puts a 20 Hz tone and a 40 Hz tone in the
// same place and reports them as one band. Returned squared so the band
// integral below can be accumulated in the power domain, which is the domain
// the binned path works in.
//
// The direct-DFT band measurement is GONE.
//
// dftPowerAt() evaluated the squared magnitude of the windowed DFT against one
// precomputed row of twiddles, and captureSubBinBands() integrated it across
// the band for the bands that were narrower than an FFT bin. Both are dead:
// the estimator evaluates the frame's DTFT at the band centre directly (see
// the Dirichlet interpolation above), so there is no "too narrow to read off
// the FFT" case and no band integral to take.
//
// What is lost with them is the reason the whole sub-bin path existed: it was
// there because a bin sum could not place a tone at the right frequency when
// the band was narrower than a bin. The interpolation has no such limitation -
// it places the tone at the right frequency whatever the transform length, and
// it costs 8 us for 47 centres instead of 23 ms for a handful of DFT rows.

// |H| of a biquad at an arbitrary frequency. coeffs are ESP-DSP order
// {b0,b1,b2,a1,a2} with a0 folded in (all five already divided by a0).
static float biquadMagAt(const float *c, float freqHz) {
  const double w = 2.0 * M_PI * (double)freqHz / (double)SAMPLE_RATE;
  const double cw = cos(w), sw = sin(w);
  const double nr = (c[0] + c[2]) * cw + c[1];
  const double ni = (c[0] - c[2]) * sw;
  const double dr = (1.0 + c[4]) * cw + c[3];
  const double di = (1.0 - c[4]) * sw;
  const double den = dr * dr + di * di;
  if (den < 1e-20) return 0.0f;
  return (float)sqrt((nr * nr + ni * ni) / den);
}

// |H| of the single-pole DC blocker: y[n] = x[n] - x[n-1] + R y[n-1]
// H(z) = (1 - z^-1) / (1 - R z^-1); both numerator and denominator are scaled by
// e^jw first so they become real/imaginary pairs rather than complex powers.
static float dcBlockerMagAt(float freqHz, float R) {
  const double w = 2.0 * M_PI * (double)freqHz / (double)SAMPLE_RATE;
  const double cw = cos(w), sw = sin(w);
  const double nr = 1.0 - cw, ni = sw;          // (cw-1) + j sw, sign of Re is irrelevant
  const double dr = cw - R,    di = sw;          // (cw-R) + j sw
  const double den = dr * dr + di * di;
  if (den < 1e-20) return 0.0f;
  return (float)sqrt((nr * nr + ni * ni) / den);
}

// Build the chain's coefficients and the calibration that goes with them.
//
// The name used to be initDerivedProfile(), because it filled the "Auto"
// profile - derivedPink[] (one scalar per band) and binEqualise[] (one gain per
// bin), both being 1/|H_chain|. Both are gone: they were multiplicative
// corrections on a magnitude, applied in two places, and the band value they
// corrected was not a PSD.
//
// What is left is the calibration proper: an ADDITIVE dB offset per band, built
// from the chain's MEASURED impulse response (estInitCalibration()). One
// correction, in dB, applied once, after the estimator and before the AGC.
//
// The order matters: the LPF corner comes from the band layout, the LPF's
// coefficients come from the corner, and the calibration is measured from the
// coefficients. So the layout has to exist first, and the calibration has to be
// rebuilt whenever the chain is rebuilt.
static void initChainCalibration(bool cold) {
  // Layout first: the LPF corner is derived from it.
  if (!bandLayoutReady) initBandLayout();
  dsps_biquad_gen_lpf_f32(coeffs_lpf, inputLpfHz / SAMPLE_RATE, INPUT_FILTER_Q);

  // The calibration is measured from THIS chain - the same coefficients, the
  // same code - so it has to be rebuilt whenever the chain is rebuilt.
  estInitCalibration();

  if (cold) {
    // The bank is calibrated from the same MEASURED chain response, and it is
    // reset: a chain change is a change of what the bank is measuring, and the
    // filters should not carry the old chain's history into the new one.
    bankInit();

    // The RTA's top twenty-three, the same way again.
    rtaTopInit();
  } else {
    // A RUNTIME rebuild changes the CALIBRATION and not the FILTERS.
    //
    // The chain's coefficients are generated from the same corner and the
    // same Q, so they are the same coefficients; the banks' filters are
    // built from the same band centres and the same bandwidths, so they are
    // the same filters. Nothing about what the banks MEASURE has changed -
    // only the offsets that divide the chain's response out of the
    // readings.
    //
    // So the banks are recalibrated and left alone. Rebuilding them and
    // RESETTING their state would drop the thirty-one bars to the floor for
    // a frame and put a settling transient into a comparison that did not
    // change - and it would throw away the detectors' history, which is
    // exactly the history that makes the change visible as the offset it
    // is, instead of as a dropout followed by a re-convergence.
    bankInitCalibration();
    rtaTopInitCalibration();
  }
}

// ---------------------------------------------------------------------------
// The chain's analytic response.
//
// |H| of the chain is available to the self-test, which measures it against
// the implementation - the model's answer and the code's answer are printed
// side by side there, which is a stronger check than a table anyone has to
// compare by eye.
// ---------------------------------------------------------------------------


// The chain is FIXED.
//
// The DC blocker and the input LPF are always in the signal path, and the
// calibration is built from the chain as it actually runs. There is no runtime
// A/B for them any more: a switch would leave the calibration describing a
// filter that is no longer in the path, which is exactly the failure the old
// "rebuild the profile on every switch" dance was working around - and the
// self-test ('t') now measures the chain directly, which is a better answer
// than a toggle ever was.

// ---------------------------------------------------------------------------
// The 31-band RTA view.
//
// This is the comparison instrument the whole exercise is for: a hardware
// analyser's bars are the third-octave series, so showing the same series
// from the same audio, in dB, is what makes "bar for bar" possible.
//
// The values are the INTEGRATED band powers - the RTA's own unit, where white
// noise rises 3 dB/octave across the bars - and they are PRE-AGC: an AGC
// offset is a display artefact and would destroy the comparison.
//
// They are also CALIBRATED: the chain's response is divided out, and the
// reference offset and the per-band trim are added, so the numbers printed
// here are the numbers the info page shows - the ones to be compared with an
// external analyser, band for band.
//
// The window is the display's window. In auto-range mode it spans 48 dB
// below the loudest band, because that is a shape comparison. In fixed mode it
// IS the window the display is using, so the plot and the bars are the same
// picture with the same grid.
// ---------------------------------------------------------------------------
void audioReactiveRtaPlot() {
  if (!bandLayoutReady) initBandLayout();
  const int n = lay31.count;
  const int rows = 16;

  // The values are read ONCE, into a local snapshot.
  //
  // The FFT task rewrites the RTA values every 10.67 ms, and this plot prints
  // 16 rows of 31 bars - about 500 serial writes, which takes longer than a
  // frame. Reading them in the inner loop therefore drew the bars from one
  // frame, the next row from the next, and the numbers at the bottom from a
  // later one again: the plot and the numbers underneath it did not agree, and
  // a band could appear loud in one row and 40 dB down in the next.
  //
  // The snapshot comes from the PUBLISHED copy, and it is taken under the
  // publish lock, so it is one whole frame and not a mixture of two.
  float snap[AR_MAX_BANDS];
  float maxDb = -120.0f;
  {
    ArPublishGuard guard;
    memcpy(snap, arRtaDbPub, sizeof(snap));
  }
  for (int k = 0; k < n; k++) if (snap[k] > maxDb) maxDb = snap[k];

  USER_PRINTLN();
  if (maxDb <= -119.0f) {
    USER_PRINTLN("RTA: no signal (the noise gate is shut, or the input is silent)");
    return;
  }

  // The window: the fixed one if the display is in fixed mode, and otherwise
  // the shared span below the peak.
  float winLo, winHi;
  if (arAutoRange) {
    winHi = maxDb;
    winLo = maxDb - arWindowRangeDb;
  } else {
    winLo = arWindowFloorDb;
    winHi = arWindowTopDb;
  }
  const float span = winHi - winLo;
  if (span < 1.0f) {
    USER_PRINTLN("RTA: the window is empty");
    return;
  }

  if (arAutoRange) {
    USER_PRINTF("RTA 31-band, 1/3-octave ISO 266, integrated power, calibrated, peak %.1f dB (auto-range, %.0f dB)\n",
                (double)maxDb, (double)span);
  } else {
    USER_PRINTF("RTA 31-band, 1/3-octave ISO 266, integrated power, calibrated, peak %.1f dB (fixed window %.1f to %.1f dB)\n",
                (double)maxDb, (double)winLo, (double)winHi);
  }

  // The bar cell is THREE characters wide, because the frequency axis below it
  // is three characters per band ("%3d" / "%.0fk"). Two-wide cells put the
  // axis one character out of step per band, and by the 31st band the label
  // was 31 characters to the left of the bar it belongs to.
  for (int row = rows; row > 0; row--) {
    USER_PRINTF("%2d ", row);
    for (int k = 0; k < n; k++) {
      const float db = snap[k];                       // absolute, calibrated
      int h = (int)lroundf((float)rows * (db - winLo) / span);
      if (h < 0) h = 0;
      if (h > rows) h = rows;
      USER_PRINT(h >= row ? "███" : "   ");
    }
    USER_PRINTLN();
  }

  // The band centres, so a bar can be matched with an external analyser's.
  USER_PRINT("   ");
  for (int k = 0; k < n; k++) {
    char b[8];
    const float f = lay31.centre[k];
    if (f < 1000.0f) snprintf(b, sizeof(b), "%3d", (int)(f + 0.5f));
    else             snprintf(b, sizeof(b), "%.0fk", (double)(f / 1000.0f));
    USER_PRINT(b);
  }
  USER_PRINTLN();

  // The values themselves, in dB on the analyser's scale.
  for (int part = 0; part < 2; part++) {
    USER_PRINT(part ? "   " : "   ");
    for (int k = part * 16; k < n && k < (part + 1) * 16; k++) {
      USER_PRINTF("%4.0f", (double)snap[k]);
    }
    USER_PRINTLN();
  }
}

// ---------------------------------------------------------------------------
// Self-test: measure the chain that actually runs.
//
// The question is narrow and it has been open for a while: the input chain's
// MEASURED response disagreed with its analytic MODEL by +2.4 dB, and no
// amount of reading the source explained it. A discrepancy like that is
// usually not a source-reading problem - it is a measurement problem, and the
// way out is to measure properly.
//
// Three numbers per probe frequency, from three independent methods:
//
//   model  |H| from the analytic model: dcBlockerMagAt() x biquadMagAt(),
//          evaluated on the same coefficient array the filter runs on.
//   impl   |H| from the chain's IMPULSE RESPONSE: a unit impulse through a
//          fresh chain, the response DTFT'd at each probe as it comes out.
//          This is the chain as implemented, whatever it turns out to be.
//   sine   |H| from the chain's response to a SINE: prime the chain with the
//          tone until its transient has died, then compare the output's
//          peak-to-peak with the input's.
//
// impl and sine are independent of each other, which is the whole point. If
// they agree with each other, the measurement is sound and any disagreement
// with model belongs to the implementation. If they disagree with each other,
// the measurement is what is broken. The old raw/peak diagnostic could not
// tell those apart - it used the same peak-ratio idea on a live signal, where
// the INT16 artefact-rejection threshold could bias the answer and the tone's
// phase decided where the peak landed.
//
// The LPF is also measured on its own (a short impulse response, no DC blocker
// in front of it), so that a failure can be localised to one stage instead of
// "somewhere in the chain".
// ---------------------------------------------------------------------------

#define SELFTEST_NPROBE 15
static const float selfTestProbeHz[SELFTEST_NPROBE] = {
  20.0f, 22.0f, 43.0f, 75.0f, 161.0f, 323.0f, 635.0f, 1281.0f,
  2562.0f, 3198.0f, 3704.0f, 5000.0f, 7000.0f, 9211.0f, 11025.0f
};

// How close the implementation has to be to the model, in dB.
constexpr float SELFTEST_TOL = 0.10f;

// The result of the last self-test, kept so that it can be published.
//
// The self-test prints to serial, but serial is not always where you expect it:
// on this build the app's console is UART0 while the USB port carries only the
// IDF's own log, so a self-test that exists only as serial output cannot be
// read. The verdict and the whole table are kept here and published on the
// info page as well, so the instrument is always readable.
struct SelfTestResult {
  bool  valid  = false;
  int   probes = 0;
  int   fails  = 0;
  float freq[SELFTEST_NPROBE];
  float modelDb[SELFTEST_NPROBE];
  float implDb[SELFTEST_NPROBE];
  float sineDb[SELFTEST_NPROBE];
  float lpfModelDb[SELFTEST_NPROBE];
  float lpfImplDb[SELFTEST_NPROBE];
  bool  ok[SELFTEST_NPROBE];
  float worstImpl = 0.0f, worstImplF = 0.0f;   // impl vs model
  float worstSine = 0.0f, worstSineF = 0.0f;   // sine vs impl
  float worstLpf  = 0.0f, worstLpfF  = 0.0f;   // LPF alone vs model
};
static SelfTestResult selfTestResult;

// Probe frequencies, clamped below Nyquist: a sine at exactly fs/2 is a
// degenerate signal (sin(pi*n) is 0, -0, 0...), so it cannot be measured.
static float selfTestFreq(int i) {
  const float fmax = 0.45f * (float)SAMPLE_RATE;
  const float f = selfTestProbeHz[i];
  return (f > fmax) ? fmax : f;
}

// Length of the impulse response the DTFT needs.
//
// The DC blocker's impulse response decays geometrically at rate R, so a
// response captured over N samples is missing a tail of roughly (1+R)*R^N.
// That tail contributes (1+R)*R^N / |1 - R e^{jw}| to the DTFT at frequency f,
// and that denominator is smallest at the LOWEST probe - which is where the
// error is largest. So N is solved for the lowest probe.
//
// At 22050 Hz with the current 20 Hz corner this is 3493 samples; after the
// rate goes to 48 kHz and the corner to 8 Hz it is 20076.
static int selfTestIrLength() {
  const float R = DC_BLOCKER_R;
  const float wMin = 2.0f * (float)M_PI * selfTestFreq(0) / (float)SAMPLE_RATE;
  const float denMin = sqrtf((1.0f - R) * (1.0f - R) + R * R * wMin * wMin);
  // the tail has to fall below 1e-6 of the response there, and the response
  // is at least 0.5 near the blocker's corner
  const float target = 1.0e-6f * denMin * 0.5f / (1.0f + R);
  int n = (int)ceilf(logf(target) / logf(R));
  if (n < 512)   n = 512;
  if (n > 32768) n = 32768;
  return n;
}

// How long the chain has to be primed with a tone before its transient has
// died. The transient decays at the filter's own pole, so R^N is the measure
// of what is left; 16 in the exponent leaves it at 1e-7.
static int selfTestPrimeLength() {
  const float R = DC_BLOCKER_R;
  int n = (int)ceilf(16.0f / -logf(R));
  if (n < 512)   n = 512;
  if (n > 65536) n = 65536;
  return n;
}

// Measure the chain's impulse response and return |H| at each of the given
// frequencies, in dB.
//
// The response is never stored: each frequency keeps its own rotation
// accumulator, so the whole measurement costs one chunk buffer and a handful of
// accumulator pairs.
//
// This is the measurement the calibration is built from, and it is the same
// measurement the chain self-test compares against the analytic model - so the
// calibration and the self-test cannot disagree about what the chain does.
static void measureChainDb(float *outDb, const float *freqs, int nf, int irLen, bool dcOn, bool lpfOn) {
  constexpr int CHUNK = 256;
  if (nf > AR_EST_SLOTS) nf = AR_EST_SLOTS;
  InputChain c;
  chainInit(&c, dcOn, lpfOn);

  // The rotation and the accumulation are in single precision.
  //
  // The P4 has no double-precision FPU: every double operation is a
  // soft-float library call, and this loop runs 47 frequencies over 20326
  // samples - nearly a million sample-frequency pairs - at every boot, on the
  // path that delays the first published frame.
  //
  // The precision is not the issue. The summation is 20326 terms whose
  // magnitudes decay, and the result is a few percent of the largest term, so
  // the accumulated rounding is of order sqrt(20326) * 1e-7 of the terms -
  // about 1e-5 relative, which is 0.00005 dB. The calibration this feeds has a
  // 0.01 dB budget, and the chain self-test measures THIS SAME routine against
  // the analytic model and prints the difference, so the accuracy is on the
  // log rather than assumed.
  float accRe[AR_EST_SLOTS], accIm[AR_EST_SLOTS];
  float zRe[AR_EST_SLOTS], zIm[AR_EST_SLOTS];
  float cw[AR_EST_SLOTS], sw[AR_EST_SLOTS];
  for (int p = 0; p < nf; p++) {
    const double w = 2.0 * M_PI * (double)freqs[p] / (double)SAMPLE_RATE;
    cw[p] = (float)cos(w);
    sw[p] = (float)sin(w);
    zRe[p] = 1.0f; zIm[p] = 0.0f;
    accRe[p] = 0.0f; accIm[p] = 0.0f;
  }

  float *buf     = (float*)heap_caps_calloc_prefer(sizeof(float), CHUNK, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
  float *scratch = (float*)heap_caps_calloc_prefer(sizeof(float), CHUNK, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
  if ((buf == nullptr) || (scratch == nullptr)) {
    if (buf)     free(buf);
    if (scratch) free(scratch);
    for (int p = 0; p < nf; p++) outDb[p] = 0.0f;
    return;
  }

  int done = 0;
  bool impulseSent = false;
  while (done < irLen) {
    int n = irLen - done; if (n > CHUNK) n = CHUNK;
    for (int i = 0; i < n; i++) buf[i] = 0.0f;
    if (!impulseSent) { buf[0] = 1.0f; impulseSent = true; }
    chainProcess(&c, buf, n, scratch);
    for (int i = 0; i < n; i++) {
      const float v = buf[i];
      for (int p = 0; p < nf; p++) {
        accRe[p] += v * zRe[p];
        accIm[p] += v * zIm[p];
        const float nr = zRe[p] * cw[p] + zIm[p] * sw[p];
        const float ni = zIm[p] * cw[p] - zRe[p] * sw[p];
        zRe[p] = nr; zIm[p] = ni;
        // Renormalised every 64 samples, for the same reason the tone
        // generator's rotation is: an un-normalised rotation drifts in
        // magnitude, and 20326 samples of float drift is not small.
        if ((i & 63) == 63) {
          const float m = sqrtf(zRe[p] * zRe[p] + zIm[p] * zIm[p]);
          zRe[p] /= m; zIm[p] /= m;
        }
      }
    }
    done += n;
  }

  for (int p = 0; p < nf; p++) {
    const float m = sqrtf(accRe[p] * accRe[p] + accIm[p] * accIm[p]);
    outDb[p] = (m > 1.0e-30f) ? (float)(20.0 * log10((double)m)) : -300.0f;
  }
  free(buf);
  free(scratch);
}

// The chain self-test's own measurement, at its probe frequencies.
static void selfTestImpulse(float *outDb, int irLen, bool dcOn, bool lpfOn) {
  float freqs[SELFTEST_NPROBE];
  for (int p = 0; p < SELFTEST_NPROBE; p++) freqs[p] = selfTestFreq(p);
  measureChainDb(outDb, freqs, SELFTEST_NPROBE, irLen, dcOn, lpfOn);
}

// Build the calibration: the chain's measured response at every band centre,
// turned into the dB correction that divides it out.
//
// This runs whenever the chain changes - at init, and whenever the 'b'/'w'
// A/B switches rebuild the chain's coefficients - so the correction can never
// describe a chain that is not the one running.
static void estInitCalibration() {
  if (!bandLayoutReady) return;
  const int total = NUM_GEQ_CHANNELS + lay31.count;

  float freqs[AR_EST_SLOTS];
  float chainDb[AR_EST_SLOTS];
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) freqs[k] = lay16.centre[k];
  for (int k = 0; k < lay31.count; k++) freqs[NUM_GEQ_CHANNELS + k] = lay31.centre[k];

  measureChainDb(chainDb, freqs, total, selfTestIrLength(),
                 dcBlockerInChain(), inputLpfInChain());

  int clamps = 0;
  for (int k = 0; k < total; k++) {
    calChainDb[k] = chainDb[k];
    // -20*log10(|H|), plus the target curve.
    float c = -chainDb[k];
    if (calTargetPink) {
      // The pink target is +10*log10(f/1000), NOT minus.
      //
      // A pink input has a PSD that falls 3 dB per octave, so its band value
      // already carries a -10*log10(f) slope; the target has to cancel that
      // slope for a pink input to read flat, and cancelling a slope means
      // adding its opposite. With the sign the other way round the two slopes
      // add up: pink reads 6 dB per octave DOWN and white 3 dB per octave
      // UP, which is the opposite of what the target is for.
      c += 10.0f * log10f(freqs[k] / CAL_PINK_REF_HZ);
    }
    if (c > CAL_CLAMP_DB) {
      c = CAL_CLAMP_DB;
      clamps++;
    } else if (c < -CAL_CLAMP_DB) {
      c = -CAL_CLAMP_DB;
      clamps++;
    }
    calDb[k] = c;
  }
  calClamps = clamps;
  calReady = true;
}

// Measure the chain's response to a sine of known amplitude: the ratio of the
// output's peak-to-peak to the input's, in dB.
//
// The chain is primed with the tone first. A filter fed a sine that starts at
// zero answers with the steady-state response PLUS a transient that decays at
// the filter's own pole; feeding the tone long enough (until that transient is
// 1e-7 of the tone) leaves a pure sinusoid to measure.
//
// This is a peak ratio, like the old raw/peak diagnostic - but of a synthetic
// signal with a known amplitude, so the INT16 artefact-rejection threshold
// cannot bias it and the tone's phase does not decide where the peak lands
// (the peak-to-peak of a sinusoid is the same wherever the grid samples it,
// to within the grid's own curvature).
static void selfTestSine(float *outDb) {
  constexpr int CHUNK = 256;
  constexpr int MEAS  = 8192;   // at least two cycles at the lowest probe
  const float TWO_PI_F = (float)(2.0 * M_PI);
  const int primeLen = selfTestPrimeLength();

  float *buf     = (float*)heap_caps_calloc_prefer(sizeof(float), CHUNK, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
  float *scratch = (float*)heap_caps_calloc_prefer(sizeof(float), CHUNK, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
  if ((buf == nullptr) || (scratch == nullptr)) {
    if (buf)     free(buf);
    if (scratch) free(scratch);
    for (int p = 0; p < SELFTEST_NPROBE; p++) outDb[p] = 0.0f;
    return;
  }

  for (int p = 0; p < SELFTEST_NPROBE; p++) {
    InputChain c;
    chainInit(&c, dcBlockerInChain(), inputLpfInChain());

    const float f = selfTestFreq(p);
    const float dph = 2.0f * (float)M_PI * f / (float)SAMPLE_RATE;
    const float amp = 0.5f;
    float ph = 0.0f;

    // prime
    int done = 0;
    while (done < primeLen) {
      int n = primeLen - done; if (n > CHUNK) n = CHUNK;
      for (int i = 0; i < n; i++) {
        buf[i] = amp * sinf(ph);
        ph += dph; if (ph > TWO_PI_F) ph -= TWO_PI_F;
      }
      chainProcess(&c, buf, n, scratch);
      done += n;
    }

    // measure
    float inMin = 1.0e30f, inMax = -1.0e30f;
    float outMin = 1.0e30f, outMax = -1.0e30f;
    done = 0;
    while (done < MEAS) {
      int n = MEAS - done; if (n > CHUNK) n = CHUNK;
      for (int i = 0; i < n; i++) {
        const float v = amp * sinf(ph);
        ph += dph; if (ph > TWO_PI_F) ph -= TWO_PI_F;
        buf[i] = v;
        if (v < inMin) inMin = v;
        if (v > inMax) inMax = v;
      }
      chainProcess(&c, buf, n, scratch);
      for (int i = 0; i < n; i++) {
        const float v = buf[i];
        if (v < outMin) outMin = v;
        if (v > outMax) outMax = v;
      }
      done += n;
    }

    const float pin  = inMax - inMin;
    const float pout = outMax - outMin;
    outDb[p] = ((pin > 0.0f) && (pout > 0.0f)) ? (20.0f * log10f(pout / pin)) : -300.0f;
  }
  free(buf);
  free(scratch);
}

// Measure the chain against its model and print the verdict.
//
// Runs on its own chain instances and on synthetic signals only, so it cannot
// disturb the live path. Called once at boot and on demand from serial 't'.
void audioReactiveSelfTest() {
  // The calibration is already built by the time the boot self-test runs: the
  // init builds it, because the first frame needs it. Re-measuring it here
  // would repeat the whole chain measurement - 47 frequencies over 20326
  // samples - for an answer that cannot differ, because the chain is fixed.
  // It is built here only if it is not there, which is the case when this runs
  // from serial 't' before anything else has built it.
  if (!calReady) initChainCalibration(true);   // also lays out the bands if they are not laid out yet

  const int irLen = selfTestIrLength();
  float implDb[SELFTEST_NPROBE], sineDb[SELFTEST_NPROBE], lpfImpDb[SELFTEST_NPROBE];

  const uint64_t tImp0 = esp_timer_get_time();
  selfTestImpulse(implDb, irLen, dcBlockerInChain(), inputLpfInChain());
  const uint64_t tImp1 = esp_timer_get_time();
  selfTestImpulse(lpfImpDb, 512, false, inputLpfInChain());   // the LPF on its own
  selfTestSine(sineDb);
  const uint64_t tSin1 = esp_timer_get_time();

  USER_PRINTF("\n=== AudioReactive self-test =========================================\n");
  USER_PRINTF("  fs %d Hz   DC blocker %s %.1f Hz (R %.6f)   input LPF %s %.1f Hz (Q %.3f)\n",
              (int)SAMPLE_RATE,
              dcBlockerInChain() ? "ON " : "OFF", (double)DC_BLOCKER_HZ, (double)DC_BLOCKER_R,
              inputLpfInChain()   ? "ON " : "OFF", (double)inputLpfHz, (double)INPUT_FILTER_Q);
  USER_PRINTF("  LPF coeffs: b0 %.6f  b1 %.6f  b2 %.6f  a1 %.6f  a2 %.6f\n",
              (double)coeffs_lpf[0], (double)coeffs_lpf[1], (double)coeffs_lpf[2],
              (double)coeffs_lpf[3], (double)coeffs_lpf[4]);
  USER_PRINTF("  impulse response %d samples, sine primed %d samples, tolerance +/-%.2f dB\n",
              irLen, selfTestPrimeLength(), (double)SELFTEST_TOL);
  USER_PRINTF("    Hz   model dB   impl dB   sine dB   LPF model  LPF impl    d(impl)   d(sine)  verdict\n");

  int fails = 0;
  float worstImpl = 0.0f, worstSine = 0.0f, worstLpf = 0.0f;
  float worstImplF = 0.0f, worstSineF = 0.0f, worstLpfF = 0.0f;

  for (int p = 0; p < SELFTEST_NPROBE; p++) {
    const float f = selfTestFreq(p);
    const float mDc  = dcBlockerInChain() ? dcBlockerMagAt(f, DC_BLOCKER_R) : 1.0f;
    const float mLpf = inputLpfInChain()   ? biquadMagAt(coeffs_lpf, f)  : 1.0f;
    const float modelDb = 20.0f * log10f(mDc * mLpf);
    const float lpfDb   = 20.0f * log10f(mLpf);
    const float dImpl = implDb[p] - modelDb;
    const float dSine = sineDb[p] - implDb[p];
    const float dLpf  = lpfImpDb[p] - lpfDb;
    const bool  ok = (fabsf(dImpl) <= SELFTEST_TOL) && (fabsf(dSine) <= SELFTEST_TOL);
    if (!ok) fails++;

    if (fabsf(dImpl) > fabsf(worstImpl)) { worstImpl = dImpl; worstImplF = f; }
    if (fabsf(dSine) > fabsf(worstSine)) { worstSine = dSine; worstSineF = f; }
    if (fabsf(dLpf)  > fabsf(worstLpf))  { worstLpf  = dLpf;  worstLpfF  = f; }

    // keep the table for the info page
    selfTestResult.freq[p]       = f;
    selfTestResult.modelDb[p]    = modelDb;
    selfTestResult.implDb[p]     = implDb[p];
    selfTestResult.sineDb[p]     = sineDb[p];
    selfTestResult.lpfModelDb[p] = lpfDb;
    selfTestResult.lpfImplDb[p]  = lpfImpDb[p];
    selfTestResult.ok[p]         = ok;

    USER_PRINTF("%6.0f   %+8.3f  %+8.3f  %+8.3f   %+8.3f  %+8.3f   %+8.3f  %+8.3f   %s\n",
                (double)f, (double)modelDb, (double)implDb[p], (double)sineDb[p],
                (double)lpfDb, (double)lpfImpDb[p],
                (double)dImpl, (double)dSine, ok ? "PASS" : "FAIL");
  }

  selfTestResult.probes    = SELFTEST_NPROBE;
  selfTestResult.fails     = fails;
  selfTestResult.worstImpl = worstImpl; selfTestResult.worstImplF = worstImplF;
  selfTestResult.worstSine = worstSine; selfTestResult.worstSineF = worstSineF;
  selfTestResult.worstLpf  = worstLpf;  selfTestResult.worstLpfF  = worstLpfF;
  selfTestResult.valid     = true;

  USER_PRINTF("  %d of %d probes %s", SELFTEST_NPROBE - fails, SELFTEST_NPROBE, fails ? "FAIL\n" : "PASS\n");
  USER_PRINTF("  worst |impl-model| %.3f dB at %.0f Hz   worst |sine-impl| %.3f dB at %.0f Hz\n",
              (double)worstImpl, (double)worstImplF, (double)worstSine, (double)worstSineF);
  USER_PRINTF("  input LPF alone: worst |impl-model| %.3f dB at %.0f Hz\n",
              (double)worstLpf, (double)worstLpfF);
  USER_PRINTF("  self-test time: impulse %.1f ms, LPF alone + sine %.1f ms\n",
              (double)(tImp1 - tImp0) / 1000.0,
              (double)(tSin1 - tImp1) / 1000.0);
  USER_PRINTF("=== end of self-test ==============================================\n");
  USER_FLUSH();
}

// ---------------------------------------------------------------------------
// The bank's self-test: the REAL filters on a REAL tone.
//
// The contract the bank is built on: a tone of power P at a band's centre must
// read P - in the shaped power the bank reports, once the chain and the
// anti-alias filter have been divided out. And the neighbours must not see it.
//
// The tone goes through the CHAIN first - the same code the live path runs,
// on its own state - because the calibration divides the chain out, and a test
// that skipped the chain would not be testing the calibration at all.
//
// The reading is averaged over the last 0.4 s of the tone, because the
// detector's own output carries a ripple at TWICE the tone's frequency (the
// squaring), and at 20 Hz that ripple is 25 ms long: averaging is what makes
// the reading a power rather than an instant of one.
//
// The neighbours are checked on the RAW readings, not the calibrated ones: the
// leakage is the band filter's own skirt at the tone's frequency, and the
// calibration is not part of that.
// ---------------------------------------------------------------------------
static volatile bool  bankTestDone = false;
static int   bankTestFails = 0;      // bands that did not read their own power
static float bankTestWorst = 0.0f;    // worst |error| over the bands, dB
static float bankTestWorstF = 0.0f;   // and the band it happened at
static int   bankTestNbrFails = 0;    // neighbours that saw too much of the tone
static int   bankTestNbrChecks = 0;   // neighbour checks that were decidable
static float bankTestWorstNbr = 0.0f; // the worst neighbour's leakage, dB
static float bankTestTimeMs = 0.0f;   // how long the test took

static void bankSelfTest() {
  if (bankTestDone) return;
  bankTestDone = true;
  if (!bandLayoutReady) initBandLayout();
  if (!bankReady) {
    USER_PRINTLN("AR: bank self-test skipped - the bank is not built");
    return;
  }

  const uint64_t t0 = esp_timer_get_time();

  // The tone's amplitude, and the power it carries: A^2/2.
  const float amp   = 0.5f;
  const float toneDb = 10.0f * log10f(amp * amp * 0.5f);   // -9.03 dB

  constexpr int N = 512;                              // the chunk the chain runs in
  static float buf[N];
  static float scratch[N];
  // The last samplesFFT samples of the chain's output, for the transform.
  //
  // The transform and the bank are asked the same question about the same
  // signal, so the transform has to see the same samples the bank sees: the
  // chain's output, at the full rate. A ring of one frame's worth is enough -
  // the transform only ever looks at the last frame.
  PSRAMScratch ring(samplesFFT);
  PSRAMScratch frame(samplesFFT);
  InputChain chain;
  BankPath bp;

  int fails = 0, nbrFails = 0, nbrChecks = 0;
  float worst = 0.0f, worstF = 0.0f, worstNbr = 0.0f;
  int   ringPos = 0;

  // The chain's response at the eight band centres, measured with the SAME
  // flags the chain below runs with.
  //
  // The global calibration (calDb[]) is built with dcBlockerInChain(), which is
  // not quite liveChainDcOn(): the PDM path runs the input LPF but not the DC
  // blocker, and dcBlockerInChain() assumes the blocker is in for any
  // non-zero setting. Using the global here would compare the transform
  // against a chain that is not the one that ran - so the correction is
  // measured here, for this chain.
  float chainDb[AR_BANK_BANDS];
  {
    float freqs[AR_BANK_BANDS];
    for (int k = 0; k < AR_BANK_BANDS; k++) freqs[k] = lay31.centre[k];
    measureChainDb(chainDb, freqs, AR_BANK_BANDS, selfTestIrLength(),
                   liveChainDcOn(), liveChainLpfOn());
  }

  USER_PRINTLN("\n=== AudioReactive bank self-test =================================");
  USER_PRINTF("  %d bands, decimate %d -> %.0f Hz, AA %.0f Hz (%d-pole), detector %.0f ms\n",
              AR_BANK_BANDS, AR_BANK_DEC, (double)AR_BANK_FS, (double)AR_BANK_AA_HZ,
              2 * AR_BANK_AA_SECS, (double)bankTauMs);

  for (int k = 0; k < AR_BANK_BANDS; k++) {
    const float fc = lay31.centre[k];
    chainInit(&chain, liveChainDcOn(), liveChainLpfOn());
    memset(&bp, 0, sizeof(bp));

    float acc[AR_BANK_BANDS] = {0.0f};
    int   accN = 0;
    float phase = 0.0f;
    const float dw = (float)(2.0 * M_PI * (double)fc / (double)SAMPLE_RATE);

    // Time to settle the filters and the detector, then time to average.
    //
    // The settle has to cover the DETECTOR, not just the filters: a one-pole
    // of time constant tau is within 1% of its final value after 4.6 tau, so
    // a fixed settle would leave a long detector short of its answer and the
    // test would read low. The filters themselves need about 100 ms at the
    // bottom band, which 1.2 s covers comfortably.
    const float settleSecs = fmaxf(1.2f, 4.6f * bankTauMs * 0.001f + 0.2f);
    const int settleChunks = (int)(settleSecs * (float)SAMPLE_RATE / (float)N);
    const int avgChunks    = (int)(0.4f * (float)SAMPLE_RATE / (float)N);

    for (int c = 0; c < settleChunks + avgChunks; c++) {
      for (int i = 0; i < N; i++) {
        buf[i] = amp * sinf(phase);
        phase += dw;
        if (phase > 6.2831853f) phase -= 6.2831853f;
      }
      chainProcess(&chain, buf, N, scratch);
      bankProcess(&bp, buf, N);
      // Keep the last frame of the chain's output for the transform.
      for (int i = 0; i < N; i++) {
        ring[ringPos] = buf[i];
        ringPos++;
        if (ringPos >= samplesFFT) ringPos = 0;
      }
      if (c >= settleChunks) {
        // One sample of every band per chunk: the counter is per CHUNK, not
        // per band. Counting it inside the band loop divides every band's
        // sum by AR_BANK_BANDS as well - which is a uniform factor of 8 on
        // the power, and so it reads as a uniform -9.03 dB error on every
        // band while the neighbours, whose ratio is scale-free, still look
        // perfect. It hid for exactly as long as it took to read the
        // neighbour numbers as proof that the filters were right.
        for (int j = 0; j < AR_BANK_BANDS; j++) acc[j] += bp.det[j];
        accN++;
      }
    }

    float raw[AR_BANK_BANDS];
    for (int j = 0; j < AR_BANK_BANDS; j++) raw[j] = acc[j] / (float)accN;

    // The band itself: the calibrated reading must be the tone's own power.
    const float gotDb = 10.0f * log10f(raw[k]) + bankCalDb[k];
    const float errDb = gotDb - toneDb;
    const bool  ok    = (errDb > -0.3f) && (errDb < 0.3f);
    if (!ok) fails++;
    if (fabsf(errDb) > fabsf(worst)) { worst = errDb; worstF = fc; }

    // The neighbours: the tone must not light them.
    float nbrDb = -99.0f;
    for (int s = 0; s < 2; s++) {
      const int j = (s == 0) ? k - 1 : k + 1;
      if (j < 0 || j >= AR_BANK_BANDS) continue;
      nbrChecks++;
      const float d = 10.0f * log10f(raw[j] / raw[k]);
      if (d > -11.0f) nbrFails++;
      if (d > nbrDb) nbrDb = d;
    }
    if (nbrDb > worstNbr) worstNbr = nbrDb;

    // THE HEAD-TO-HEAD: the transform, asked the same question about the same
    // signal.
    //
    // The ring holds the last frame of the CHAIN'S OUTPUT, which is exactly
    // what the live path hands the transform - the live frame is the chain's
    // output too. The transform is then calibrated with the chain's own
    // correction, so both sides are referred to the input and the only thing
    // left between them is the measurement kernel.
    //
    // The band here is the bottom bank, whose chain also carries the
    // anti-alias filter. That filter's response is inside bankCalDb[], and the
    // transform never sees it - so the two are not corrected the same way
    // unless it is put back. The anti-alias filter is flat to 0.00 dB across
    // these eight centres (it is a 400 Hz corner), so the difference is
    // negligible - but it is worth knowing it is there.
    for (int i = 0; i < samplesFFT; i++) frame[i] = ring[(ringPos + i) % samplesFFT];
    // The transform's side is calibrated exactly as calDb[] would calibrate
    // it, but for the chain that actually ran.
    float tfCorr = -chainDb[k];
    if (calTargetPink) tfCorr += 10.0f * log10f(lay31.centre[k] / CAL_PINK_REF_HZ);
    const float tfDb   = xfTransformBandDb(frame.get(), k) + tfCorr;
    const float bankDb = (raw[k] > 0.0f) ? (10.0f * log10f(raw[k]) + bankCalDb[k]) : -120.0f;
    xfRecord(k, tfDb, bankDb, toneDb);

    USER_PRINTF("  band %d  %8.2f Hz   %+8.3f dB %s   neighbour %+7.2f dB"
                "   | transform %+7.3f dB %s   bank %+7.3f dB %s\n",
                k, (double)fc, (double)errDb, ok ? "PASS" : "FAIL", (double)nbrDb,
                (double)(tfDb - toneDb), (fabsf(tfDb - toneDb) <= 1.0f) ? "  " : "!!",
                (double)(bankDb - toneDb), (fabsf(bankDb - toneDb) <= 0.35f) ? "  " : "!!");
  }

  bankTestFails     = fails;
  bankTestWorst     = worst;
  bankTestWorstF    = worstF;
  bankTestNbrFails  = nbrFails;
  bankTestNbrChecks = nbrChecks;
  bankTestWorstNbr  = worstNbr;
  bankTestTimeMs    = (float)((double)(esp_timer_get_time() - t0) / 1000.0);

  USER_PRINTF("  %d of %d bands %s   worst %+.3f dB at %.2f Hz\n",
              AR_BANK_BANDS - fails, AR_BANK_BANDS, fails ? "FAIL" : "PASS",
              (double)worst, (double)worstF);
  USER_PRINTF("  neighbours: %d of %d checks FAIL, worst leakage %+.2f dB\n",
              nbrFails, nbrChecks, (double)worstNbr);
  USER_PRINTF("  self-test time: %.0f ms\n", (double)bankTestTimeMs);
  USER_PRINTLN("=== end of bank self-test ===========================================");
  USER_FLUSH();
}

// The old psychoacoustic band combiner is GONE.
//
// computeBandsPsychoacoustic() used to fill the 16 band values from the FFT
// bins: a bin sum for the bands wider than a bin, the direct-DFT integral for
// the ones narrower, and a scale factor to make the two paths agree. It had no
// callers left once the estimator took over - the band values now come from
// arPublishBands(), which maps the calibrated band levels in dB onto the bar.
//
// Keeping it would have been worse than keeping it dead: it reads the same
// vReal[] the estimator uses and writes into the same fftCalc[] the effects
// read, so a stray call would have put a different unit on the bars.

#if defined(CONFIG_IDF_TARGET_ESP32C3)
constexpr bool skipSecondFFT = true;
#else
constexpr bool skipSecondFFT = false;
#endif

// allocate FFT sample buffers from heap
static bool alocateFFTBuffers(void) {
  #ifdef SR_DEBUG
    USER_PRINT(F("\nFree heap ")); USER_PRINTLN(ESP.getFreeHeap());
  #endif

  if (vReal) heap_caps_free(vReal); // should not happen
  if (vImag) heap_caps_free(vImag); // should not happen
  if (fftBuf) heap_caps_free(fftBuf); // should not happen
  #ifdef ESP32
  if ((vReal = (float*)heap_caps_calloc_prefer(sizeof(float), samplesFFT, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL)) == nullptr) return false; // calloc or die
  if ((vImag = (float*)heap_caps_calloc_prefer(sizeof(float), samplesFFT, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL)) == nullptr) return false;
  // The transform's workspace: samplesFFT COMPLEX points, interleaved. See the
  // comment on fftBuf for why the frame is transformed as a complex sequence.
  //
  // It has to be 2*samplesFFT FLOATS, not samplesFFT: dsps_fft4r_fc32(data, N)
  // takes N COMPLEX points and reads and writes all 2N of them. Allocating
  // samplesFFT floats instead is a 16 KB heap overflow - and it is a quiet one,
  // because everything that reads the spectrum only looks at the FIRST half of
  // the bins. The overflow landed on whatever the allocator had put next, and
  // for a long time that was the pinkFactors[] table. Delete that table and the
  // overflow starts eating the FFT window instead: the window is overwritten
  // with FFT output every frame, the PSD normalisation goes with it, and the
  // estimator's self-test reports every centre as wrong. The chain self-test
  // still passes, because the chain does not use the window.
  //
  // heap_caps_calloc_prefer(n, size, num, caps...) is n CHUNKS of size BYTES,
  // with num = the number of capability arguments that follow - not an
  // alignment.
  //
  // This one buffer is asked for in internal RAM rather than PSRAM: it is the
  // single hottest region in the whole system, and the transform's PSRAM
  // traffic is bursty enough to starve the display. See arAllocHotBuf().
  if ((fftBuf = (float*)arAllocHotBuf(sizeof(float) * samplesFFT * 2, "fftBuf", arFftBufInternal)) == nullptr) return false;
  #else
  if ((vReal = (float*) calloc(sizeof(float), samplesFFT)) == nullptr) return false; // calloc or die
  if ((vImag = (float*) calloc(sizeof(float), samplesFFT)) == nullptr) return false;
  if ((fftBuf = (float*) calloc(sizeof(float), samplesFFT * 2)) == nullptr) return false;
  #endif

  #ifdef SR_DEBUG
    USER_PRINTLN("\nalocateFFTBuffers() completed successfully.");
    USER_PRINT(F("Free heap: ")); USER_PRINTLN(ESP.getFreeHeap());
    USER_PRINT("FFTtask free stack: "); USER_PRINTLN(uxTaskGetStackHighWaterMark(NULL));
    USER_FLUSH();
  #endif
  return(true); // success
}

// High-Pass "DC blocker" filter
// see https://www.dsprelated.com/freebooks/filters/DC_Blocker.html
//
// The filter itself now lives in chainProcess(), alongside the input LPF, so
// that the live chain and the self-test's chain are the same code. See the
// InputChain struct above.

//
// FFT main task
//
void FFTcode(void * parameter)
{
  #ifdef SR_DEBUG
    USER_FLUSH();
    USER_PRINT("AR: "); USER_PRINT(pcTaskGetTaskName(NULL));
    USER_PRINT(" task started on core "); USER_PRINT(xPortGetCoreID()); // causes trouble on -S2
    USER_PRINT(" [prio="); USER_PRINT(uxTaskPriorityGet(NULL));
    USER_PRINT(", min free stack="); USER_PRINT(uxTaskGetStackHighWaterMark(NULL));
    USER_PRINTLN("]"); USER_FLUSH();
  #endif

  // see https://www.freertos.org/vtaskdelayuntil.html
  const TickType_t xFrequency = FFT_TASK_PERIOD_TICKS;
  const TickType_t xFrequencyDouble = FFT_TASK_PERIOD_TICKS * 2;
  static bool isFirstRun = false;

  static float* oldSamples = nullptr; // previous 50% of samples
  static bool haveOldSamples = false; // for sliding window FFT
  bool usingOldSamples = false;
  if (!oldSamples) oldSamples = (float*) calloc(sizeof(float), retainSamples); // allocate on first run
  if (!oldSamples) { disableSoundProcessing = true; return; }                 // no memory -> die

  bool success = true;
  // The init's phases are timed, and the times are printed at the end of the
  // init. Everything here happens BEFORE the first frame is published, so
  // this is the time the display spends showing nothing - and until it was
  // measured, "the display is slow at boot" had four candidate explanations
  // and no way to tell them apart.
  const uint64_t tInit0 = esp_timer_get_time();
  if ((vReal == nullptr) || (vImag == nullptr) || (fftBuf == nullptr)) success = alocateFFTBuffers(); // allocate sample buffers on first run
  if (success == false) { disableSoundProcessing = true; return; }             // no memory -> die

  // NB: no blanket suppression of bin 0 - band 0 now spans bins 0+1, so
  // real 0-43 Hz content is wanted there. Residual DC is handled in the FFT
  // task by clamping bin 0 to the level of bin 1.

  // ESP-DSP's radix-4 twiddle table: four times max_fft_size floats.
  //
  // Left to itself the library allocates this in PSRAM, where the transform
  // would read about 190 KB of it every frame. It is asked for with a buffer
  // of our own instead, so that it lands in internal RAM, for the reason
  // arAllocHotBuf() gives: the transform's PSRAM traffic is bursty enough to
  // starve the display when the display is sharing the same PSRAM.
  //
  // dsps_fft4r_init_fc32() fills the caller's buffer and points the library's
  // global at it, so this is the same table the library would have allocated -
  // only in different memory. The estimator's self-test is the check that it
  // was filled correctly: a twiddle table that was not filled shows up
  // immediately as every band centre being wrong.
  if (!fftTwiddle) {
    fftTwiddle = (float*)arAllocHotBuf(sizeof(float) * 4 * (samplesFFT >> 1), "fft twiddle", arTwiddleInternal);
    if (!fftTwiddle) { USER_PRINTLN("AR: no memory for the FFT twiddle table"); disableSoundProcessing = true; return; }
  }
  esp_err_t myerr = dsps_fft4r_init_fc32(fftTwiddle, samplesFFT >> 1);
  if (myerr != ESP_OK) {
    // Either the library had already been initialised - in which case its own
    // table is in use, not ours - or something is genuinely wrong. Give our
    // buffer back either way: if the library's own table is the one in use the
    // transform still works, it just does its reading out of PSRAM.
    heap_caps_free(fftTwiddle);
    fftTwiddle = nullptr;
    if (!dsps_fft4r_initialized) {
      USER_PRINTF("Not possible to initialize FFT. Error = %i", myerr);
      return;
    }
  }
  // Belt and braces: if the library did not take our buffer, it is running on a
  // table we do not own, and ours is dead weight.
  if (fftTwiddle != nullptr && dsps_fft4r_w_table_fc32 != fftTwiddle) {
    heap_caps_free(fftTwiddle);
    fftTwiddle = nullptr;
  }
  // The Blackman-Harris window lives in PSRAM, not on the stack: at N=4096 it
  // is 16 KB, and the stack is the wrong place to spend 16 KB on a constant.
  // 16-byte aligned because the transform reads it with SIMD stores.
  if (!windowBuf) {
    #ifdef ESP32
      windowBuf = (float*)heap_caps_aligned_alloc(16, sizeof(float) * samplesFFT, MALLOC_CAP_SPIRAM);
      if (!windowBuf) windowBuf = (float*)heap_caps_aligned_alloc(16, sizeof(float) * samplesFFT, MALLOC_CAP_INTERNAL);
    #else
      windowBuf = (float*)calloc(sizeof(float), samplesFFT);
    #endif
    if (!windowBuf) { USER_PRINTLN("AR: no memory for the FFT window"); disableSoundProcessing = true; return; }
  }
  dsps_wind_blackman_harris_f32(windowBuf, samplesFFT);
  // The PSD unit hangs off the window's energy, so the normalisation constant
  // is measured from the window that is actually applied.
  estSetNorm(windowBuf, samplesFFT);

  // These two are generated rather than hand-pasted so they track SAMPLE_RATE.
  // At 22050 Hz / Q 0.734 it reproduces the old earlevel.com block equations
  // exactly, so the tuning is unchanged. This is a noise filter, not a
  // correction - the "Auto" pink profile inverts its response exactly, so no
  // accuracy is lost.

  // lowpass, inputLpfHz, INPUT_FILTER_Q - file scope so the calibration can
  // measure the very same filter (see initChainCalibration)
  const uint64_t tCal0 = esp_timer_get_time();
  initChainCalibration(true);
  const uint64_t tCal1 = esp_timer_get_time();

  // The live chain starts from a clean state. Its flags are refreshed from the
  // live settings on every frame, so the 'b'/'w' A/B switches still work.
  chainInit(&liveChain, false, false);

  // One-shot: measure the input chain against its analytic model before the
  // first frame is published, so that a mismatch is on the log from the start
  // instead of being discovered two days later. It runs on its own chain
  // instance and touches nothing the live path depends on.
  //
  // It is 236 ms of boot time, and it is a diagnostic, so it does not run
  // unless arBootSelfTests says so. The serial key 't' runs it on demand.
  static bool chainSelfTestDone = false;
  const uint64_t tChain0 = esp_timer_get_time();
  if (arBootSelfTests && !chainSelfTestDone) {
    chainSelfTestDone = true;
    audioReactiveSelfTest();
  }
  const uint64_t tChain1 = esp_timer_get_time();

  // The estimator's self-test runs here as well: it needs the transform, and
  // the transform is not reentrant. In the FFT task, before the first frame,
  // the peripheral is ours. Another 103 ms, also off by default.
  if (arBootSelfTests) estimatorSelfTest();
  const uint64_t tEst1 = esp_timer_get_time();

  // The bank's self-test: the bottom eight bands are measured by their own
  // filters, and those filters get the same treatment as everything else -
  // the real code, on a synthetic tone, asserted against the contract they
  // were built on.
  if (arBootSelfTests) bankSelfTest();

  // The RTA's top bank's self-test: the twenty-three bands above the bottom
  // eight are measured by their own filters now, and they get the same
  // treatment - the real code, on a synthetic tone and on synthetic noise,
  // asserted against the contract they were built on.
  if (arBootSelfTests) rtaTopSelfTest();

  // The banks' health, from their own self-tests.
  //
  // This is what makes the fallback real. A bank that failed its test is not
  // a measurement - its numbers do not mean what the contract says - so the
  // flag goes false and the transform takes the frame back where the
  // transform can do the job. The RTA's bottom bank is the exception, and
  // the reason is in arTransformRuns(): the transform cannot separate those
  // bands at all.
  //
  // There is no display bank any more, so there is no display bank's health:
  // the sixteen display bands are sixteen of the thirty-one, and they are as
  // healthy as the RTA band they came from.
  //
  // With the self-tests off, no test has failed and the banks are trusted -
  // which is the intended state: the banks are the measurement, and the
  // transform is the fallback.
  arBankHealthy   = (bankTestFails  == 0);
  arRtaTopHealthy = (rtaTestFails   == 0);

  if (!arBankHealthy) {
    USER_PRINTLN("AR: the RTA's bottom bank FAILED its self-test - it keeps its eight bands (the FFT cannot separate them) and the failure is reported");
  }
  if (!arRtaTopHealthy) {
    USER_PRINTLN("AR: the RTA's top bank FAILED its self-test - the transform takes the twenty-three top bands back");
  }

  // The init's bill. The buffers' time is what is left of the init before
  // the calibration started.
  arInitBuffersMs = (float)((double)(tCal0 - tInit0) / 1000.0);
  arInitCalMs     = (float)((double)(tCal1 - tCal0) / 1000.0);
  arInitChainMs   = (float)((double)(tChain1 - tChain0) / 1000.0);
  arInitEstMs     = (float)((double)(tEst1 - tChain1) / 1000.0);
  arInitTotalMs   = (float)((double)(tEst1 - tInit0) / 1000.0);
  USER_PRINTF("AR init: buffers %.1f ms, calibration %.1f ms, chain self-test %.1f ms, "
              "estimator self-test %.1f ms, total %.1f ms\n",
              (double)(tCal0 - tInit0) / 1000.0,
              (double)(tCal1 - tCal0) / 1000.0,
              (double)(tChain1 - tChain0) / 1000.0,
              (double)(tEst1 - tChain1) / 1000.0,
              (double)(tEst1 - tInit0) / 1000.0);
  if (!arBootSelfTests) {
    USER_PRINTLN("AR: the boot-time self-tests are off (chain, estimator, both banks). "
                 "Run the chain's with 't'; the banks' and the estimator's run at the next boot with them on.");
  }
  // Which measurement the frame will use. The banks are the measurement and
  // the transform is the reference they are checked against, so the normal
  // answer is "banks only" - and the frame says so, because "is the FFT
  // running?" is the first question anyone asks of this code.
  //
  // The forced case is printed differently, because it is not the same
  // thing: the transform is running because a bank could not be trusted,
  // not because anyone asked for it.
  const bool tfAtBoot = arTransformRuns();
  USER_PRINTF("AR measurement: %s\n",
              tfAtBoot
                ? (arUseTransform
                    ? "the FFT transform (the banks are the comparison)"
                    : "the FFT transform - FORCED, because a bank could not be trusted")
                : "the filter banks alone (no FFT in the frame)");
  // The key that switches the transform on, so the switch is findable without
  // a web page. It is a runtime diagnostic: it is off at every boot. Not
  // printed when the transform is already running - forced or asked for - the
  // line above says why it is running, and this would contradict it.
  if (!tfAtBoot) {
    USER_PRINTLN("AR: press 'a' on the serial console to switch the transform on for the comparison");
  }
  USER_FLUSH();

  // The clock check, once, at boot. The codec never reports its own clock and
  // the I2S driver never reports the clock it programmed, so this is the only
  // place the number can be verified - and if it is wrong, every band is on
  // the wrong frequency while everything else keeps looking normal.
  {
    uint32_t gotMclk = 0, gotFs = 0;
    if (audioSource) {
      gotMclk = audioSource->getMclkHz();
      gotFs   = audioSource->getClockSampleRateHz();
    }
    if (gotMclk && gotFs) {
      long ppm = (long)((((long long)gotFs - (long long)SAMPLE_RATE) * 1000000LL) / (long long)SAMPLE_RATE);
      USER_PRINTF("AR clock: want MCLK %lu Hz (256 x %lu Hz) | measured MCLK %lu Hz -> %lu Hz (%+ld ppm)\n",
                  (unsigned long)ES8311_MCLK_HZ, (unsigned long)SAMPLE_RATE,
                  (unsigned long)gotMclk, (unsigned long)gotFs, ppm);
    } else {
      USER_PRINTF("AR clock: want MCLK %lu Hz (256 x %lu Hz) | not readable on this target\n",
                  (unsigned long)ES8311_MCLK_HZ, (unsigned long)SAMPLE_RATE);
    }
    USER_FLUSH();
  }

  // A +24 dB peaking EQ used to sit here at 4659 Hz, on by default. It was a
  // display curve living in the signal path: it dominated everything above
  // ~300 Hz (15.85x, +24.0 dB at its centre) and is the reason the hand-tuned
  // pink profiles no longer match the input chain - they ascend where the
  // chain requires a 24 dB fall. Dropped, because a "Auto" profile that
  // inverts the chain cannot also preserve an arbitrary tilt. Any wanted
  // overall curve belongs in the display layer, where it is visible and
  // tunable, not baked in here where it silently fights the profile.

  TickType_t xLastWakeTime = xTaskGetTickCount();
  for(;;) {
    // There used to be an unconditional delay(1) here, with a note saying it
    // must not be deleted because the IDLE task needed the CPU and the
    // watchdog needed feeding.
    //
    // It was costing a full TICK of wall clock on every frame, and that is
    // not the same thing as costing the CPU a millisecond. The task consumes
    // exactly one hop (hopSamples) per cycle, so a cycle LONGER than the
    // frame period means the task takes samples out of the DMA ring SLOWER
    // than the codec puts them in. The ring grows, the DMA starts overwriting
    // samples it has not handed over yet, and the analysis ends up with a
    // hole in the middle of every 4096-sample window - broadband splatter,
    // right where the fresh hop is spliced onto the carried-over one.
    //
    // With the delay(1) in place the cycle measured 11.68 ms against a
    // 10.667 ms frame period: the frame itself was correctly pinned at
    // 10.667 ms by the deadline wait and the blocking read (see the pacing
    // at the bottom of the loop), and the delay(1) was a whole extra
    // millisecond on top of it.
    //
    // Why a full tick and not half: the wait at the bottom of the loop ends
    // ON a tick boundary, so this delay(1) always began just after a tick
    // had fired and had a full tick to wait for. It was the tick that was
    // being paid, not the delay.
    //
    // The idle task and the watchdog are still served. The task blocks for
    // most of every frame - in the deadline wait below, or in the blocking
    // read - and it is pinned to core 1, so core 0's idle task is untouched
    // by it either way. The task is not subscribed to the task watchdog, so
    // there is nothing here for it to feed.
    const uint64_t tLoopEnterUs = esp_timer_get_time();

    // Don't run FFT computing code if we're in Receive mode or in realtime mode
    if (disableSoundProcessing || (audioSyncEnabled == AUDIOSYNC_REC)) {
      isFirstRun = false;
      haveOldSamples = false;
      vTaskDelayUntil( &xLastWakeTime, xFrequency);        // release CPU, and let I2S fill its buffers
      continue;
    }

    // timing
    uint64_t start = esp_timer_get_time();
    // The raw microsecond count of everything this frame spends, added up as
    // it goes. The cycle is what it should equal; the difference is the
    // residual, and it is computed per frame rather than by comparing two
    // differently-smoothed averages (see tParts).
    uint64_t partsRawUs = 0;
    {
      // The gap between the loop turning over and the frame's own timestamp.
      // It is a handful of instructions, but it is the one term in the cycle
      // that no other timer covers, and the cycle's parts have to add up to
      // the cycle - so it gets measured rather than assumed.
      const uint64_t tLoopUs = start - tLoopEnterUs;
      tLoop = ((float)(tLoopUs / 10ULL) * 3 + tLoop * 7) / 10.0;
      partsRawUs += tLoopUs;
    }
    // The frame's deadline: FRAME_PERIOD_MS after the frame started. One hop
    // of audio is that long, and the audio is the clock this task runs on.
    // See the pacing at the bottom of the loop, which is where it is used.
    const uint64_t frameDeadlineUs = start + (uint64_t)(FRAME_PERIOD_MS * 1000.0f);
    // The work's own clock. The frame's compute starts when the audio has
    // been read, because the read's wait is not compute - it is the task
    // waiting for the next hop to arrive. Set just below, once the read is
    // done.
    uint64_t workStartUs = 0;
    bool haveDoneFFT = false; // indicates if second measurement (FFT time) is valid
    static uint64_t lastCycleStart = 0;
    static uint64_t lastLastTime = 0;
    static uint64_t lastPartsRawUs = 0;   // the previous frame's raw parts, in us
    static float   lastLastParts = 0.0f;   // the frame before that, for the two-tap average
    static float   lastLastResidual = 0.0f;
    if ((lastCycleStart > 0) && (lastCycleStart < start)) { // filter out overflows
      const uint64_t cycleRawUs = start - lastCycleStart;
      uint64_t taskTimeInMillis = (cycleRawUs +5ULL) / 10ULL; // "+5" to ensure proper rounding
      fftTaskCycle = arCycleSmooth(fftTaskCycle, (float)taskTimeInMillis, (float)lastLastTime); // smart smooth
      lastLastTime = taskTimeInMillis;

      // The parts of the frame that just ended, and what is left of the
      // cycle once they are taken out.
      //
      // Both go through the SAME filter as the cycle above. That is the whole
      // point: the cycle used to be compared against the sum of the smoothed
      // parts, and the two are filtered differently - a 0.4 EMA over a
      // two-sample average against a 0.3 EMA each - so the difference
      // between them was an artefact of the filtering, not time that went
      // missing. Filtered identically, cycle = parts + residual exactly, and
      // the residual says something about the frame.
      if (lastPartsRawUs > 0) {
        const float partsUnits = (float)lastPartsRawUs / 10.0f;
        tParts = arCycleSmooth(tParts, partsUnits, lastLastParts);
        lastLastParts = partsUnits;

        const float residualUnits = ((float)cycleRawUs - (float)lastPartsRawUs) / 10.0f;
        tResidual = arCycleSmooth(tResidual, residualUnits, lastLastResidual);
        lastLastResidual = residualUnits;
      }

      // The worst (shortest) cycle ever seen - see fftTaskMinCycle.
      if ((fftTaskMinCycle == 0.0f) || (taskTimeInMillis < fftTaskMinCycle)) fftTaskMinCycle = taskTimeInMillis;
      if (taskTimeInMillis > fftTaskMaxCycle) fftTaskMaxCycle = taskTimeInMillis;
      arFramesTotal++;
    }
    lastCycleStart = start;

    // get a fresh batch of samples from I2S
    //
    // There is no "start clean" memset here, because there is nothing to
    // clean. The frame is filled completely before anything reads it: the
    // carried-over head is memcpy'd in below, and the do-while that follows
    // fills the rest hopSamples at a time until the frame is full. getSamples()
    // zeroes its own destination before filling it - whether or not the source
    // came up - so a short I2S read leaves a defined buffer rather than stale
    // samples. The memset that used to be here was 16 KB of PSRAM writes per
    // frame, 93 frames a second, to write zeros over bytes that were about to
    // be overwritten anyway.

    uint16_t readOffset;
    if (haveOldSamples && (doSlidingFFT > 0)) {
      memcpy(vReal, oldSamples, sizeof(float) * retainSamples);                    // carry the overlap forward
      usingOldSamples = true;
      readOffset = retainSamples;
    } else {
      usingOldSamples = false;
      readOffset = 0;
    }
    // read fresh samples until the frame is full
    do {
      // this looks a bit cumbersome, but it onlyworks this way - any second instance of the getSamples() call delivers junk data.
      if (audioSource) audioSource->getSamples(vReal+readOffset, hopSamples);
      readOffset += hopSamples;
    } while (readOffset < samplesFFT);

    // TEMPORARY DIAGNOSTIC: peak of the fresh samples BEFORE the input filters
    // run. maxSample (below) is measured after them, so the pair separates
    // "the signal never arrived at the codec" from "our own filter removed it".
    // The region and the artefact rejection are identical to maxSample's, so
    // rawPeak and peak differ by exactly the filters and nothing else.
    float rawPeak = 0.0f;
    {
      const int from = usingOldSamples ? (samplesFFT - hopSamples) : 0;
      for (int i = from; i < samplesFFT; i++) {
        if ((vReal[i] <= (INT16_MAX - 1024)) && (vReal[i] >= (INT16_MIN + 1024))) {
          const float a = fabsf(vReal[i]);
          if (a > rawPeak) rawPeak = a;
        }
      }
    }

    // debug info in case that stack usage changes
    static unsigned int minStackFree = UINT32_MAX;
    unsigned int stackFree = uxTaskGetStackHighWaterMark(NULL);
    if (minStackFree > stackFree) {
      minStackFree = stackFree;
      DEBUGSR_PRINTF("|| %-9s min free stack %d\n", pcTaskGetTaskName(NULL), minStackFree); //WLEDMM
    }
    // timing
    //
    // This measures the sample READ, and on a correctly paced board the read is
    // mostly the task sitting Blocked on the DMA waiting for the next 256
    // samples (11.6 ms of audio) to be delivered. It is wait time, not compute,
    // and the task burns no CPU in it. It reads as work only when the task has
    // fallen behind: then the ring is full, the read never blocks, and this
    // number DROPS - which is exactly backwards if you take it for a cost.
    // It is excluded from the CPU budget for that reason; see arHealth().
    if (start < esp_timer_get_time()) { // filter out overflows
      const uint64_t rReadUs = esp_timer_get_time() - start;
      uint64_t sampleTimeInMillis = (rReadUs +5ULL) / 10ULL; // "+5" to ensure proper rounding
      sampleTime = (sampleTimeInMillis*3 + sampleTime*7)/10.0; // smooth
      partsRawUs += rReadUs;
    }
    start = esp_timer_get_time(); // start measuring filter time
    workStartUs = start;           // the frame's COMPUTE starts here

    xLastWakeTime = xTaskGetTickCount();       // update "last unblocked time" for vTaskDelay
    isFirstRun = !isFirstRun; //  toggle throttle

#ifdef MIC_LOGGER
    float datMin = 0.0f;
    float datMax = 0.0f;
    double datAvg = 0.0f;
    for (int i=0; i < samplesFFT; i++) {
      if (i==0) {
        datMin = datMax = vReal[i];
      } else {
        if (datMin >  vReal[i]) datMin =  vReal[i];
        if (datMax <  vReal[i]) datMax =  vReal[i];
      }
      datAvg +=  vReal[i];
    }
#endif

    // experimental - be nice to LED update task (trying to avoid flickering) - dual core only
    // if (strip.isServicing()) delay(1);


    // normal mode: filter everything
    float *samplesStart = vReal;
    uint16_t sampleCount = samplesFFT;
    if (usingOldSamples) {
      // sliding window mode: only the newest hopSamples need filtering
      samplesStart = vReal + (samplesFFT - hopSamples);
      sampleCount = hopSamples;
    }
    // band pass filter - can reduce noise floor by a factor of 50
    // downside: frequencies below 100Hz will be ignored
   if ((useInputFilter > 0) && (useInputFilter < 99)) {
      // Filter only the NEW samples. In sliding-window mode the first
      // (samplesFFT - hopSamples) entries were already filtered on a previous
      // frame and are carried forward verbatim; running the filter over them
      // again applies it once per frame of retention. With hopSamples=256 that
      // is ~7 applications, and because the DC blocker is a highpass the cost
      // lands almost entirely on the bottom of the spectrum: measured, band 0
      // (41.9 Hz) loses 6.13 dB cumulatively against 0.63 dB at band 3
      // (123 Hz) - a 5.5 dB tilt that no amount of profile correction can undo.
      //
      // This is why band 0 read dead under the Auto-derived profile while the
      // "Generic Line-In" hand table appeared to fix it: that table's +7.4 dB
      // at band 0 was masking this, not compensating for a real response.
      //
      // The filter's state lives in liveChain, so filtering only the new tail
      // keeps the filters continuous across frames rather than restarting them.
      switch(useInputFilter) {
        case 1: runMicFilter(sampleCount, samplesStart); break;           // PDM microphone bandpass
        default: break;
      }

      // The input chain proper: DC blocker, then input LPF. Both run on the
      // NEW samples only, and BEFORE the oldSamples capture below. Both
      // details are load-bearing:
      //
      //   - New samples only. The retained head was filtered on a previous frame
      //     and is carried forward; filtering it again stacks one pass per frame
      //     of retention (~7-8 at hopSamples=256), and band 15's centre came out
      //     -24 dB down instead of -3 dB.
      //   - Before the capture. oldSamples is saved below from vReal, so anything
      //     filtered after that point is discarded and the unfiltered data is
      //     what gets carried into the next frame. Filtering here, alongside the
      //     DC blocker and ahead of the save, is what makes the carried data
      //     filtered at all.
      //
      // vImag is scratch because the biquad has no in-place mode. The LPF's
      // delay line lives in liveChain, so filtering only the tail keeps the
      // filter continuous across frames.
      liveChain.dcOn  = liveChainDcOn();
      liveChain.lpfOn = liveChainLpfOn();
      chainProcess(&liveChain, samplesStart, sampleCount, vImag);
    }

    // The RTA filter bank: the bottom eight bands.
    //
    // It runs on the SAME fresh samples the chain just filtered, and it READS
    // them - the filters are transposed direct form II, so they need no
    // output buffer and cannot disturb the FFT's input.
    //
    // Its filters never stop. Every frame adds the new samples to them and
    // every band is recomputed, which is the whole difference from the narrow
    // path: its transform only ran once every 682.7 ms, so the bottom bars
    // were frozen for two-thirds of a second at a time.
    //
    // The cost is counted in the frame's own budget (it is part of the
    // filtering region below) and reported separately, because it is the
    // number that says whether the bank is affordable.
    if (bankReady) {
      const uint64_t tBank0 = esp_timer_get_time();
      bankProcess(&liveBank, samplesStart, sampleCount);
      const uint64_t rBankUs = esp_timer_get_time() - tBank0;
      const float bankUnits = (float)((rBankUs + 5ULL) / 10ULL);   // TIMING_UNIT_US
      bankTime = (bankUnits * 3.0f + bankTime * 7.0f) / 10.0f;
    }


    // The RTA's top twenty-three, on their own filters.
    //
    // Same reason, same fresh hop, same "always runs" rule as the display
    // bank: the bank's reading and the transform's reading have to be two
    // measurements of the SAME audio, or the comparison between them means
    // nothing.
    //
    // It is the more expensive of the two - twenty-three bands of two sections
    // each over the fresh hop - but it is also the last thing standing between
    // the RTA and the transform's 42.7 ms window.
    if (rtaTopReady) {
      const uint64_t tRta0 = esp_timer_get_time();
      rtaTopProcess(&liveRtaTop, samplesStart, sampleCount);
      const uint64_t rRtaUs = esp_timer_get_time() - tRta0;
      const float rtaUnits = (float)((rRtaUs + 5ULL) / 10ULL);   // TIMING_UNIT_US
      rtaTime = (rtaUnits * 3.0f + rtaTime * 7.0f) / 10.0f;
    }

    // The broadband level of the FRESH HOP, and with it the noise floor and
    // the gate.
    //
    // It is measured here, on the newest hopSamples of the frame, AFTER the
    // chain and BEFORE the window: after the chain, because the level has to
    // describe what comes out of the chain (the chain's own response is
    // corrected for in the bands, not here); before the window, because the
    // window is a property of the ANALYSIS, not of the signal.
    //
    // The fresh hop is the frame's tail: in sliding mode the rest of the
    // frame was carried forward from the previous frame and is not new
    // information.
    {
      const int from = samplesFFT - hopSamples;
      arLevelDb = arHopLevelDb(vReal + from, hopSamples);
      arTrackLevel(arLevelDb);
    }

    // timing measurement
    if (start < esp_timer_get_time()) { // filter out overflows
      const uint64_t rFilterUs = esp_timer_get_time() - start;
      uint64_t filterTimeInMillis = (rFilterUs +5ULL) / 10ULL; // "+5" to ensure proper rounding
      filterTime = (filterTimeInMillis*3 + filterTime*7)/10.0; // smooth
      partsRawUs += rFilterUs;
    }
    start = esp_timer_get_time(); // start measuring FFT time

    // Sub-timers for the fftTime region. fftTime alone is a single number
    // covering five unrelated things, and the per-stage breakdown exists
    // because that number was not enough to act on: at the time these were
    // added it read 24.73 ms, and the only question that mattered was WHICH of
    // the five was responsible. It turned out to be captureSubBinBands() -
    // 23.10 ms of the 24.73, all of it the double-precision rotation recurrence
    // in dftPowerAt() running emulated on a single-precision FPU. That is now
    // fixed (see dftTwiddle), and the probes stay because the next question
    // will be the same shape: filtering time was 2.18 ms and, once the DFT
    // stopped dominating the frame, becomes the largest thing left in it.
    //
    // The five tile the whole region - the profile below summed to 24.727 ms
    // against a reported fftTime of 24.73 ms, so nothing is unaccounted for and
    // nothing is double-counted. Keep it that way if a probe moves.
    //
    // Each probe records the gap since the previous one and moves the mark, so
    // the stages tile the region without any of them knowing what comes next.
    // Values are in TIMING_UNIT_US, matching fftTime.
    uint64_t tMark = start;
    float dWindow = 0.0f, dPrep = 0.0f, dFft = 0.0f, dPost = 0.0f, dFinal = 0.0f;
    float dBitrev = 0.0f, dEst = 0.0f;
    auto probe = [&tMark](float &out) {
      const uint64_t now = esp_timer_get_time();
      out = (float)((now - tMark + 5ULL) / 10ULL); // "+5" to ensure proper rounding
      tMark = now;
    };

    // vImag is the chain's scratch buffer - the biquad has no in-place mode -
    // and it is dead from here until the next frame's filter pass. Nothing
    // reads it: the transform's imaginary parts are written explicitly
    // (fftBuf[2*i+1] = 0.0f) further down, not taken from here. So it is not
    // cleared, and 16 KB of PSRAM is not written every frame for nothing.

    // Keep the newest retainSamples for the next frame - i.e. the tail of this
    // frame's new data, which begins hopSamples in.
    memcpy(oldSamples, vReal+(samplesFFT - retainSamples), sizeof(float) * retainSamples);
    haveOldSamples = true;

    // find highest sample in the batch, and count zero crossings
    float maxSample = 0.0f;                         // max sample from FFT batch
    uint_fast16_t newZeroCrossingCount = 0;
    for (int i=0; i < samplesFFT; i++) {
	    // pick our  our current mic sample - we take the max value from all samples that go into FFT
	    if ((vReal[i] <= (INT16_MAX - 1024)) && (vReal[i] >= (INT16_MIN + 1024))) { //skip extreme values - normally these are artefacts
        if (usingOldSamples) {
          if ((i >= (samplesFFT - hopSamples)) && (fabsf(vReal[i]) > maxSample)) maxSample = fabsf(vReal[i]);  // only look at newest hopSamples
        } else if (fabsf((float)vReal[i]) > maxSample) maxSample = fabsf((float)vReal[i]);
      }
      // WLED-MM/TroyHacks: Calculate zero crossings
      //
      if (i < (samplesFFT-1)) {
        if (__builtin_signbit(vReal[i]) != __builtin_signbit(vReal[i+1]))  // test sign bit: sign changed -> zero crossing
            newZeroCrossingCount++;
      }
    }
    newZeroCrossingCount = (newZeroCrossingCount*2)/3; // reduce value so it typically stays below 256
    zeroCrossingCount = newZeroCrossingCount; // update only once, to avoid that effects pick up an intermediate value

    // Whether the transform runs this frame. Decided ONCE, here, so that
    // every stage below - the windowing, the transform itself, the magnitude
    // scaling and the beat detector - agrees about it. They must: a frame
    // that windowed but did not transform would leave the spectrum reading
    // the windowed frame as if it were magnitudes.
    const bool transformRuns = arTransformRuns();

    // The windowing, on its own pass.
    //
    // It is part of the ANALYSIS, not of the frame: the frame has to keep its
    // time-domain content for the carry-forward and for the level, and with
    // the transform off there is nothing to window for. The peak and the
    // zero-crossing count above are taken on the UNWINDOWED samples, which is
    // what they were before this was split out, and what they have to be.
    if (transformRuns) {
      for (int i = 0; i < samplesFFT; i++) vReal[i] *= windowBuf[i]; // FFT windowing for ESP-DSP
    }
    probe(dWindow);

    // The frame's peak, released early for the volume-reactive effects. The
    // level the effects use is the broadband level of the fresh hop, measured
    // above - this peak is only used to detect beats.
    //
    // The level telemetry used to be dumped here. It is on the info page now -
    // "Level (dB)", "Noise floor (dB)", "Gate" and "Display AGC (dB)" - which
    // is where it belongs: it is read far more often than it is printed, and
    // the info page carries it without a serial line rate to overrun.
#ifdef MIC_LOGGER
    micReal_min = datMin;
    micReal_max = datMax;
    micReal_avg = datAvg / samplesFFT;
#if 0
    // compute mix/max again after filering - usefull for filter debugging
    for (int i=0; i < samplesFFT; i++) {
      if (i==0) {
        datMin = datMax = vReal[i];
      } else {
        if (datMin >  vReal[i]) datMin =  vReal[i];
        if (datMax <  vReal[i]) datMax =  vReal[i];
      }
    }
    micReal_min2 = datMin;
    micReal_max2 = datMax;
#endif
#endif

    float wc = 1.0; // FFT window correction factor, relative to Blackman_Harris

    // The FFT always runs.
    //
    // The gate used to skip it entirely, and that is why the whole pipeline
    // went silent when the gate shut - and why the level, the floor and the
    // AGC, all of which need the spectrum, froze with it. Now the gate only
    // decides what is PUBLISHED.
    {
      if ((skipSecondFFT == false) || (isFirstRun == true)) {
        // The old "3-5ms on ESP32 / ~30ms on -C3" timings that used to sit here
        // measured a SOFTWARE FFT and no longer describe this call. On ESP32-P4
        // dsps_fft4r_fc32 dispatches to the on-chip hardware FFT, and a
        // 2048-point one now measures 260 us - against a 24.73 ms frame that was
        // 23.10 ms of emulated double arithmetic in the sub-bin DFT beside it.
        // Read the live figure from the AudioReactive info page, not from here.
        wc = 1.0f; // use Blackman_Harris value from ESP-DSP code
        // NB: the DC bin is clamped after the FFT, once magnitudes are real.

        float last_majorpeak = FFT_MajorPeak;
        float last_magnitude = FFT_Magnitude;

          // The input LPF runs earlier, with the DC blocker and ahead of the
          // oldSamples capture - see the comment on that block. It has to be
          // before the capture or the carried-forward data is unfiltered.

          // The bands narrower than one FFT bin used to be measured here, with
          // a direct DFT, while vReal still held the time-domain signal. The
          // estimator does not need that: it interpolates the spectrum
          // BETWEEN the bins, so a band narrower than a bin is measured from
          // the bins around it. The direct DFT is gone - see the cleanup.
          probe(dPrep);

          // THE TRANSFORM, OR NOT.
          //
          // With arUseTransform off, none of this runs: no windowing, no
          // transform, no bit reversal, no magnitude fold. The frame keeps its
          // TIME-DOMAIN content, because that is what the next frame's
          // carry-forward and the level and the zero-crossing count need -
          // and the banks, which ran earlier in the frame, are the whole
          // measurement.
          //
          // The banks are not optional here: they update every frame from
          // the fresh hop, and estFinalise() - which runs either way - is
          // what turns their readings into the published bands.
          if (transformRuns) {
          // The frame is transformed as a COMPLEX sequence of samplesFFT
          // points - the windowed frame in the real slots, zeros in the
          // imaginary ones - rather than with ESP-DSP's real-FFT helper.
          // See the comment on fftBuf for why: the real FFT needs a transform
          // length that is a power of FOUR (samplesFFT/2 = 2048 is not one,
          // and neither the bit-reversal nor the P4's radix-4 kernel can do
          // it), and its unpack reads the twiddle at twice the angle the
          // unpack needs.
          //
          // The transform's output order is bit-reversed, so the bit reversal
          // follows it - the same order ESP-DSP's own real FFT uses.
          for (int i = 0; i < samplesFFT; i++) {
            fftBuf[2*i]     = vReal[i];   // the windowed frame
            fftBuf[2*i + 1] = 0.0f;       // ... as a real sequence
          }
          dsps_fft4r_fc32(fftBuf, samplesFFT);
          probe(dFft);
          dsps_bit_rev4r_fc32(fftBuf, samplesFFT);
          probe(dBitrev);

          // The estimator runs here, while the spectrum is still complex. The
          // fold below turns the interleaved (re, im) pairs into magnitudes
          // and throws the imaginary parts away, and the interpolation needs
          // them: it reconstructs the spectrum BETWEEN the bins.
          //
          // (The calibration's rebuild is above, outside the transform - it
          // belongs to the measurement, and the measurement is the banks'.)
          if (bandLayoutReady) estComputeTransform(fftBuf);
          else                 estClearTransform();
          probe(dEst);
          arTransformValid = bandLayoutReady;

          // The transform's output is INTERLEAVED real/imaginary pairs:
          //   fftBuf[2k]   = Re(bin k)
          //   fftBuf[2k+1] = Im(bin k)
          // so fftBuf[1] is Im(DC) - mathematically 0 - and reading the array
          // as if it held magnitudes makes band 0 dead. Fold each pair into a
          // true magnitude (forward is safe: we write index i while reading
          // 2i and 2i+1, both >= i).
          //
          // Only the first half of the bins is kept: bin k is at k * fs/N, so
          // bins past samplesFFT/2 are the mirror image of the negative
          // frequencies and carry nothing the first half does not.
          const int halfN = samplesFFT >> 1;
          for (int i = 0; i < halfN; i++) {
            float re = fftBuf[2*i];
            float im = fftBuf[2*i+1];
            vReal[i] = sqrtf(re*re + im*im);
          }
          memset(vReal + halfN, 0, sizeof(float) * (samplesFFT - halfN));
          // Band 0 now reads bin 0, which carries both 0-43 Hz content and any
          // residual DC offset. Zeroing it would throw away the sub-bass, so
          // clamp it to bin 1's level instead: real LF survives, a DC spike
          // can't dominate the band. (runDCBlocker already removes most DC at
          // the source for the default useInputFilter == 2 path.)
          if (vReal[0] > vReal[1]) vReal[0] = vReal[1];

          FFT_MajorPeak = 0;
          FFT_Magnitude = 0;

          for (int i = 0; i < halfN; i++) {
            if (vReal[i] > FFT_Magnitude) {
              FFT_Magnitude = vReal[i];
              // binWidth, not SAMPLE_RATE / samplesFFT: SRate_t is uint32_t, so
              // the latter is integer division (22050/2048 = 10) and reported
              // every peak 7.5% low - binWidth is 10.767 Hz.
              FFT_MajorPeak = i * binWidth;
            }
          }

        // The legacy "pink noise" scaling of the spectrum is GONE. It used to
        // multiply every bin by a per-bin factor and then divide every bin by
        // the same factor again, so the two cancelled and the spectrum that
        // reaches the estimator was never pink-corrected - only the PEAK
        // magnitude was, by the one factor below. The band values were never
        // touched by it either way.
        //
        // What is left is that one factor, computed in place of the table:
        // the peak magnitude is lifted for low frequencies by
        // pinkcenter / sqrt(bin centre frequency), floored at 1.

        FFT_Magnitude *= wc;  // apply correction factor

        if (FFT_MajorPeak < binWidth) {FFT_MajorPeak = 1.0f; FFT_Magnitude = 0;}                                  // too low - use zero
        if (FFT_MajorPeak > (0.42f * SAMPLE_RATE)) {FFT_MajorPeak = last_majorpeak; FFT_Magnitude = last_magnitude;} // too high - keep last peak

        //fix peak magnitude
        if ((FFT_MajorPeak > (binWidth/1.25f)) && (FFT_MajorPeak < (SAMPLE_RATE/2.2f)) && (FFT_Magnitude > 4.0f)) {
          unsigned peakBin = constrain((int)((FFT_MajorPeak + binWidth/2.0f) / binWidth), 0, samplesFFT -1);
          float peakFreq = peakBin * binWidth + binWidth/2.0f;
          const float fold = SAMPLE_RATE * 0.42f;
          if (peakFreq > fold) peakFreq = fold - 0.25f * (peakFreq - fold); // suppress noise and aliasing
          if (peakFreq > 0.0f) FFT_Magnitude *= fmaxf(pinkcenter / sqrtf(peakFreq), 1.0f);
        }
        probe(dPost);

        FFT_MajorPeak = constrain(FFT_MajorPeak, 1.0f, NYQUIST_HZ);   // restrict value to range expected by effects
        FFT_MajPeakSmth = FFT_MajPeakSmth + 0.42 * (FFT_MajorPeak - FFT_MajPeakSmth);   // I like this "swooping peak" look

      } // end of the transform
        else { // the transform is not running: the banks are the measurement
          // The transform's own numbers are stale, so they are marked as stale
          // rather than left to be read as live. The banks' numbers are NOT
          // touched: they are current, and they are the measurement.
          estClearTransform();
        }

      } else { // skip second run --> clear fft results, keep peaks
        memset(vReal, 0, sizeof(float) * samplesFFT);
        // The estimator had no spectrum to measure. The published band
        // values are NOT cleared: they are the last good frame's, and the
        // display holds them until the next spectrum arrives.
        estClearTransform();
      }

      haveDoneFFT = true;

    }

    // The publish runs every frame when the transform is NOT running.
    //
    // The transform's gate is the FFT's: on the ESP32-C3 the second FFT of
    // each pair is skipped, and the spectrum is a frame old. But the
    // published bands are the BANKS' readings, and the banks update every
    // frame whatever the FFT is doing - so holding the publish for a second
    // frame would publish the banks at half their rate, and the AGC and the
    // smoothing that ride on it with them.
    //
    // Nothing in this block needs a spectrum when the transform is off, so
    // the gate does not apply.
    if ((skipSecondFFT == false) || (isFirstRun == true) || (!transformRuns)) {
      // The magnitudes in vReal[] are scaled here, for the beat detector,
      // which reads one of them by bin number.
      //
      // With the transform off there are no magnitudes in vReal[] - it holds
      // the time-domain frame, which the next frame's carry-forward and the
      // level both need - so the scaling is skipped, and the beat detector
      // reads the bank's magnitude at the same bin instead.
      if (transformRuns) {
        for (int i = 0; i < samplesFFT; i++) {
          float t = fabsf(vReal[i]);                      // just to be sure - values in fft bins should be positive any way
          vReal[i] = t * FFT_BIN_SCALE;                   // Reduce magnitude. Want end result to be scaled linear and ~4096 max.
        } // for()
      }

      // A target-curve change at runtime rebuilds the calibration HERE, in
      // the FFT task. The rebuild measures an impulse response through the
      // chain and writes the calibration's arrays, and this is the only
      // thread that may do either - the JSON handler cannot.
      //
      // It runs OUTSIDE the transform: the calibration belongs to the
      // measurement, and the measurement is the banks' when the transform is
      // off.
      //
      // It is a CALIBRATION rebuild, not a cold start: the filters do not
      // change, so the banks keep their state and the bars do not drop to
      // the floor for a frame.
      if (arCalDirty) {
        arCalDirty = false;
        initChainCalibration(false);
      }

      // THE BANKS' READINGS BECOME THE PUBLISHED VALUES.
      //
      // This runs whether or not the transform ran. It is the whole
      // measurement, and the transform was only ever the alternative to it:
      // with the transform on, the A/B switches say which of the two drives;
      // with it off, there is only the bank.
      estFinalise(transformRuns);

      // The display AGC, on this frame's calibrated band values. It is the
      // last thing that touches them.
      arUpdateAgc(arFrameDtMs);

      // The dominant frequency and the peak magnitude.
      //
      // With the transform on these are the transform's, exactly as they have
      // always been - the effects do not change. With it off they are the
      // bank's, and the bank's are computed either way so the two can be
      // compared on the same audio.
      arBankPeak();

      // The 16 display bands.
      //
      // This is where the spectrum becomes the bars: the RTA band's
      // calibrated level - the integrated power over that third-octave band -
      // the AGC's dB offset, and the display's dB span. Nothing else - no
      // per-band gain, no input profile, no histogram normalisation, no floor
      // subtraction. Every one of those MUTATED the band value, and a mutated
      // band value is not a measurement.
      //
      // The old path summed the FFT bins over hand-tuned band ranges, scaled
      // the sum by a profile and by a gain, and mapped the result to a byte.
      // It is gone; the RTA band's calibrated level is the band value.
      arPublishBands(arFrameDtMs);

      // Nothing else touches the band values here. The RTA band's calibrated
      // level is the band value; the AGC's dB offset and the display's dB span
      // are applied inside arPublishBands().

      // The RTA set, on its own scale and its own auto-range. These are the
      // thirty-one the sixteen come from, in the same window and with the
      // same offset - and they are published in their own slot for the
      // effects that draw an analyser.
      arPublishRta(arFrameDtMs);

      // The published frame: this frame's numbers, copied under the publish
      // lock so the JSON endpoint cannot read a mix of two frames. It is
      // LAST, because everything above is what it copies.
      arPublishFrameSnapshot();

    } else {
      // No new spectrum (the second FFT was skipped): the published values
      // are held, exactly as they were. Nothing to restore - fftCalc IS the
      // published value now.
    }

    probe(dFinal);

    // timing
    static uint64_t lastLastFFT = 0;
    const uint64_t tFftRegionEndUs = esp_timer_get_time();
    if (haveDoneFFT && (start < esp_timer_get_time())) { // filter out overflows
      uint64_t fftTimeInMillis = ((tFftRegionEndUs - start) +5ULL) / 10ULL; // "+5" to ensure proper rounding
      fftTime  = (((fftTimeInMillis + lastLastFFT)/2) *3 + fftTime*7)/10.0; // smart smooth
      lastLastFFT = fftTimeInMillis;
      partsRawUs += (tFftRegionEndUs - start);
      // Stage split, same smoothing as fftTime. A frame that skipped the FFT
      // (noise gate closed, or skipSecondFFT) leaves the un-probed stages at
      // zero, which is honest - that work genuinely did not happen - and the
      // smoothing keeps a few skipped frames from dominating the average.
      tWindow = (dWindow *3 + tWindow *7)/10.0;
      tPrep= (dPrep*3 + tPrep*7)/10.0;
      tFft    = (dFft    *3 + tFft    *7)/10.0;
      tPost   = (dPost   *3 + tPost   *7)/10.0;
      tFinal  = (dFinal  *3 + tFinal  *7)/10.0;
      tBitrev = (dBitrev *3 + tBitrev *7)/10.0;
      tEst    = (dEst    *3 + tEst    *7)/10.0;
    }

    // run peak detection
    autoResetPeak();
    detectSamplePeak();

    haveNewFFTResult = true;

    // The tail: everything between the end of the FFT region and the pacing.
    //
    // It used to be inside no timer at all, which is how a whole millisecond
    // of the cycle could hide from the accounting. It is small - the peak
    // detection and the publish - but "small" is a thing to measure, not to
    // assume.
    {
      const uint64_t tTailUs = esp_timer_get_time() - tFftRegionEndUs;
      tTail = ((float)(tTailUs / 10ULL) * 3 + tTail * 7) / 10.0;
      partsRawUs += tTailUs;
    }

    // The pacing.
    //
    // In sliding mode the task consumes exactly ONE hop (hopSamples) per
    // cycle, and getSamples() BLOCKS until the DMA has delivered that many
    // samples. That block IS the pacer: the task then runs at exactly the
    // audio's rate, the DMA ring stays empty, and no audio is dropped.
    //
    // The old code put a timer on top of that - vTaskDelayUntil() for
    // FFT_TASK_PERIOD_TICKS, anchored to the moment the work started. The
    // work takes ~3.4 ms, so the timer only ever had ~7.6 ms left to wait,
    // and the cycle came out at 12.7 ms instead of the frame's 10.67 ms.
    // The task then consumed 40.2 k samples/s out of the 48 k the codec
    // delivered, the DMA ring filled, and the driver began discarding
    // samples: one gap of ~100 samples in the middle of every 4096-sample
    // window, right where the new hop is spliced onto the carried-over one.
    // A discontinuity inside the window is broadband splatter, and splatter
    // is exactly what makes a measured spectrum disagree with the truth.
    //
    // So: no timer in the sliding case. One tick is left as a floor, so that
    // a source which returns immediately instead of blocking cannot spin the
    // task.
    const uint64_t tFloorStartUs = esp_timer_get_time();
    if (audioSource != nullptr)  // yield to the DMA ring so it can fill
    {
      if (!usingOldSamples) {
        vTaskDelayUntil( &xLastWakeTime, xFrequencyDouble); // we need a double wait when no old data was used
      } else {
        vTaskDelayUntil( &xLastWakeTime, 1);                 // the blocking read is the pacer
      }
    }
    {
      const uint64_t tFloorUs = esp_timer_get_time() - tFloorStartUs;
      tFloor = ((float)(tFloorUs / 10ULL) * 3 + tFloor * 7) / 10.0;
      partsRawUs += tFloorUs;
    }

    // The frame's deadline.
    //
    // The blocking read is the pacer - but only while it blocks. When the
    // DMA ring already has samples ready - which is what happens after a
    // long init, and again after any stall - the read returns at once, and
    // the task is left with nothing between frames but the one-tick floor
    // above. That floor is shorter than the frame's own work (about 3 ms
    // against a 1 ms tick), so it does not bound anything: the task goes
    // round as fast as it can, and it consumes the ring faster than the
    // codec delivers it.
    //
    // Which way round the failure runs is worth stating plainly, because the
    // old comment here had it backwards and that is how the delay(1) at the
    // top of the loop survived:
    //
    //   - A cycle SHORTER than the frame period means the task takes samples
    //     out of the ring FASTER than the codec puts them in. The ring
    //     empties, the read starts returning whatever is left, and the
    //     analysis runs on audio that has holes in it.
    //   - A cycle LONGER than the frame period means the task takes samples
    //     out SLOWER than the codec puts them in. The ring grows until it is
    //     full, and then the DMA overwrites the samples it has not handed
    //     over yet - a gap in the middle of every window, which is broadband
    //     splatter, which is exactly what makes a measured spectrum disagree
    //     with the truth. The display also shows audio that is behind the
    //     room.
    //
    // Both are bad, and the deadline is the thing that holds the cycle at
    // the frame period from above while the blocking read holds it there
    // from below.
    //
    // So a frame ends no earlier than FRAME_PERIOD_MS after it started,
    // measured from the same instant the audio is paced against. In steady
    // state the read has already blocked away the missing time and this
    // never fires; it fires exactly when the read came back early, which is
    // precisely when the task would otherwise run away from the audio.
    //
    // The wait is in whole ticks only. The tick is 1 ms and the frame is
    // 10.67 ms, so the remainder in steady state is a few microseconds -
    // and sleeping a whole tick for it would add a millisecond to every
    // frame, which would drag the consumption rate below the codec's and
    // make the ring grow. Below a tick, there is nothing to wait for. The
    // slack is TRUNCATED, not rounded up, for the same reason: a wait that
    // overshoots the deadline makes the cycle longer than the frame period,
    // and the read is the accurate pacer - it blocks for exactly as long as
    // the audio needs.
    //
    // This is outside the audioSource test above on purpose. With no source
    // there is no read to block, and the deadline is then the only thing
    // standing between this task and a 100% spin on core 1.
    const uint64_t now = esp_timer_get_time();
    if (now < frameDeadlineUs) {
      const uint32_t slackMs = (uint32_t)((frameDeadlineUs - now) / 1000ULL);
      if (slackMs > 0) vTaskDelay(pdMS_TO_TICKS(slackMs));
    } else {
      // The frame is at or past its deadline.
      //
      // That is NOT the same as being late, and the difference matters: in
      // the healthy case the READ has already blocked away the whole frame
      // period - the task was waiting for the audio, which is the pacer - so
      // every frame finishes at or past its deadline while its WORK took a
      // quarter of it. A frame is only LATE when the work alone ran longer
      // than the frame period, and that is what the counter counts. Counting
      // the elapsed time instead reported every healthy frame as an overrun,
      // which is a number that cannot be read.
      //
      // When the frame really is late, no wait can save it: the task is
      // behind the audio and the right answer is to catch up - the next
      // frame's read will return sooner because the ring is fuller, and the
      // difference comes out of the ring's surplus.
      //
      // This used to be vTaskDelay(1), and that was a full TICK of dead
      // time - up to a millisecond during which no audio was consumed at
      // all, in a frame that was already late. It is a taskYIELD() now: the
      // task gives up the CPU for anybody else at the same priority on the
      // same core, and returns immediately if there is nobody. The idle
      // task does not need a tick handed to it here - the task blocks in
      // the next frame's read, which is where it spends most of its time.
      if ((workStartUs > 0) &&
          ((now - workStartUs) > (uint64_t)(FRAME_PERIOD_MS * 1000.0f))) {
        arFramesOverran++;
      }
      taskYIELD();
    }
    {
      const uint64_t tWaitUs = esp_timer_get_time() - now;
      tWait = ((float)(tWaitUs / 10ULL) * 3 + tWait * 7) / 10.0;
      partsRawUs += tWaitUs;
    }

    // The frame is done. Its parts are now known, and the next frame's
    // timestamp is what closes the cycle - the comparison between the two is
    // made at the top of the next frame, where both are in hand.
    lastPartsRawUs = partsRawUs;
  } // for(;;)ever
} // FFTcode() task end


///////////////////////////
// Pre / Postprocessing  //
///////////////////////////

static void runMicFilter(uint16_t numSamples, float *sampleBuffer)          // pre-filtering of raw samples (band-pass)
{
  // low frequency cutoff parameter - see https://dsp.stackexchange.com/questions/40462/exponential-moving-average-cut-off-frequency
  //constexpr float alpha = 0.04f;   // 150Hz
  //constexpr float alpha = 0.03f;   // 110Hz
  constexpr float alpha = 0.0225f; // 80hz
  //constexpr float alpha = 0.01693f;// 60hz
  // high frequency cutoff  parameter
  //constexpr float beta1 = 0.75f;   // 11Khz
  //constexpr float beta1 = 0.82f;   // 15Khz
  //constexpr float beta1 = 0.8285f; // 18Khz
  constexpr float beta1 = 0.85f;  // 20Khz

  constexpr float beta2 = (1.0f - beta1) / 2.0;
  static float last_vals[2] = { 0.0f }; // FIR high freq cutoff filter
  static float lowfilt = 0.0f;          // IIR low frequency cutoff filter

  for (int i=0; i < numSamples; i++) {
        // FIR lowpass, to remove high frequency noise
        float highFilteredSample;
        if (i < (numSamples-1)) highFilteredSample = beta1*sampleBuffer[i] + beta2*last_vals[0] + beta2*sampleBuffer[i+1];  // smooth out spikes
        else highFilteredSample = beta1*sampleBuffer[i] + beta2*last_vals[0]  + beta2*last_vals[1];                  // special handling for last sample in array
        last_vals[1] = last_vals[0];
        last_vals[0] = sampleBuffer[i];
        sampleBuffer[i] = highFilteredSample;
        // IIR highpass, to remove low frequency noise
        lowfilt += alpha * (sampleBuffer[i] - lowfilt);
        sampleBuffer[i] = sampleBuffer[i] - lowfilt;
  }
}

// The old postProcessFFTResults() is gone.
//
// It did four things to the band values, and all four of them made the band
// value something other than a measurement:
//   - multiplied by a per-band profile that inverted the input chain,
//   - multiplied by a gain,
//   - ran its own smoothing and its own limiter,
//   - and mapped the result to a byte with a second, different dB curve.
//
// The band value now comes from the RTA band's calibrated level, gets the
// AGC's single dB offset, and is mapped once - in arPublishBands().
////////////////////
// Peak detection //
////////////////////

// peak detection is called from FFT task when vReal[] contains valid FFT results
static void detectSamplePeak(void) {
  bool havePeak = false;
#if 1
  // softhack007: this code continuously triggers while volume in the selected bin is above a certain threshold. So it does not detect peaks - it detects volume in a frequency bin.
  // Poor man's beat detection by seeing if sample > Average + some value.
  // This goes through ALL of the 255 bins - but ignores stupid settings
  // Then we got a peak, else we don't. The peak has to time out on its own in order to support UDP sound sync.
  //
  // The magnitude in the selected bin: the FFT's own with the transform on,
  // and the bank's at the same frequency with it off. vReal[] holds the
  // TIME-DOMAIN frame when the transform is off, so reading it here would
  // compare a sample against a threshold and call it a beat.
  const float binMag = arTransformValid ? vReal[binNum] : arBankBinMag((float)binNum * binWidth);
  if (arGateOpen && (maxVol > 0) && (binNum > 4) && (binMag > maxVol) && ((millis() - timeOfPeak) > 100)) {
    havePeak = true;
  }
#endif

#if 0
  // alternate detection, based on FFT_MajorPeak and FFT_Magnitude. Not much better...
  if ((binNum > 1)  && (maxVol > 8) && (binNum < 10) && (sampleAgc > 127) && 
      (FFT_MajorPeak > 50) && (FFT_MajorPeak < 250) && (FFT_Magnitude > (16.0f * (maxVol+42.0)) /*my_magnitude > 136.0f*16.0f*/) && 
      (millis() - timeOfPeak > 80)) {
    havePeak = true;
  }
#endif

  if (havePeak) {
    samplePeak    = true;
    timeOfPeak    = millis();
    udpSamplePeak = true;
  }
}

static void autoResetPeak(void) {
  uint16_t MinShowDelay = MAX(50, strip.getMinShowDelay());  // Fixes private class variable compiler error. Unsure if this is the correct way of fixing the root problem. -THATDONFC
  if (millis() - timeOfPeak > MinShowDelay) {          // Auto-reset of samplePeak after a complete frame has passed.
    samplePeak = false;
    if (audioSyncEnabled == AUDIOSYNC_NONE) udpSamplePeak = false;  // this is normally reset by transmitAudioData
  }
}

////////////////////
// usermod class  //
////////////////////

//class name. Use something descriptive and leave the ": public Usermod" part :)
// The pipeline's health, as a handful of words.
//
// These judgements were once HTML verdict strings, one per timing row in the
// info page. They are the same judgements now - only the format changed - so
// that a script can watch them over time, and so that the thresholds live in
// ONE place. The info page could not tell you that a red row and an orange row
// meant different things.
//
// Three limits, and conflating them is what made the old hand-set task period
// useless as a warning for years:
//   - I2S_RING_MS is the only one that costs audio. Overrun it and the DMA ring
//     refills underneath the task, dropping samples.
//   - FRAME_PERIOD_MS is the CPU budget: the wall-clock time one frame's audio
//     takes to arrive. Overrun it and the task falls behind the samples it is
//     handed, so the update rate slips. The ring absorbs the drift, so this is a
//     real problem but not a destructive one.
//   - half of that, as an early warning. The failure mode here is a slow slide
//     toward saturation rather than a step, and this tier is what makes the
//     slide visible before it costs anything.
//
// The CPU work is fftTime + filterTime. It deliberately excludes sampleTime.
// That number looks like a stage but is mostly the task sitting blocked on the
// DMA waiting for the next hop to arrive - wait time, not compute, and time in
// which it burns no CPU at all.
//
// Getting this wrong is not academic. With sampleTime counted, a correctly
// paced task reads 6.75 ms of an 11.61 ms frame and trips the half-budget
// warning, while the task list shows it at 34% CPU, not 58%. The two disagreed
// because one of them was counting sleep. A warning that fires on a healthy
// system is how you learn to ignore warnings.
//
// The cycle and the sample read measure PACING rather than work, so their
// limits are different:
//   - a correctly paced task's cycle does not come in under FRAME_PERIOD_MS - it
//     sits AT it, because the task spends the remainder of the frame asleep and
//     only wakes when the next audio is there. Testing it with
//     `>= FRAME_PERIOD_MS` would report perfect pacing as overdue. So the only
//     question is whether the cycle has run well PAST one frame, which means
//     frames are being missed and audio is being consumed faster than the DMA
//     delivers it. 1.5x leaves room for jitter without being reachable by a
//     healthy task.
//   - a read that returns at once means the DMA ring already had the samples
//     ready - the task is BEHIND the audio, and the ring has a surplus that will
//     only grow. A read that blocks is the healthy case: it is the pacer, and
//     it means the task is keeping exactly up with the codec. This is the
//     opposite sense to the cycle, which is why it is a separate judgement.
//   - the surplus is the most direct measure of the ring's health: the read
//     blocks until the hop has arrived, so the wait time and the surplus are
//     two readings of the same quantity (surplus = frame period MINUS the
//     wait). The equilibrium is a surplus worth exactly the task's own work
//     time - the frame waits for the audio, does its work, and the next frame's
//     wait has to cover that work - so the ring carries the task's working time
//     as its buffer, and no more. Below that the task is consuming the audio
//     faster than the codec delivers it and the DMA starts overwriting samples
//     it has not handed over; above it the task is behind the audio and the ring
//     is filling, which does not cost samples but does add latency.
//   - the FASTEST cycle the task has run is the opposite problem again: a cycle
//     that is too SHORT means the task went round ahead of the audio, doing
//     work on samples it had already had. A healthy task's shortest cycle is
//     the frame period minus about a tick (the deadline wait is truncated
//     rather than rounded up), so it is ~9.5 ms, not 10.67 ms. Only a cycle
//     well below that means the task really did run ahead.
//
// The times arrive in TIMING_UNIT_US (10 us per unit); they are converted to
// milliseconds here, so the thresholds below are read in their own units.
struct ArHealth {
  const char* budget;         // the CPU work against the frame's budget
  const char* pacing;         // the task against the audio
  const char* fastest;        // the shortest cycle the task has run
  const char* surplus;        // the DMA ring
  bool        readBlocking;   // the sample read is waiting for audio (the healthy case)
};

static ArHealth arHealth() {
  ArHealth h;
  const double workMs    = (double)(fftTime + filterTime) * (double)TIMING_UNIT_US / 1000.0;
  const double cycleMs   = (double)fftTaskCycle   * (double)TIMING_UNIT_US / 1000.0;
  const double fastestMs = (double)fftTaskMinCycle * (double)TIMING_UNIT_US / 1000.0;
  const double readMs    = (double)sampleTime     * (double)TIMING_UNIT_US / 1000.0;
  const double surplusMs = (double)FRAME_PERIOD_MS - readMs;

  if (workMs >= (double)I2S_RING_MS)                  h.budget = "over_dma_ring";
  else if (workMs >= (double)FRAME_PERIOD_MS)         h.budget = "over_frame";
  else if (workMs >= (double)FRAME_PERIOD_MS * 0.5)   h.budget = "half_frame";
  else                                                 h.budget = "ok";

  if (cycleMs >= (double)I2S_RING_MS)                  h.pacing = "over_dma_ring";
  else if (cycleMs >= (double)FRAME_PERIOD_MS * 1.5)   h.pacing = "frames_missed";
  else if (cycleMs >= (double)FRAME_PERIOD_MS * 1.05)  h.pacing = "ring_growing";
  else                                                 h.pacing = "ok";

  if (fastestMs <= 0.0)                               h.fastest = "n/a";
  else if (fastestMs < (double)FRAME_PERIOD_MS * 0.5)  h.fastest = "ran_ahead";
  else if (fastestMs < (double)FRAME_PERIOD_MS * 0.85) h.fastest = "within_a_tick";
  else                                                 h.fastest = "ok";

  if (surplusMs < 1.0)                                h.surplus = "ring_empty";
  else if (surplusMs > (double)I2S_RING_MS * 0.5)      h.surplus = "ring_filling";
  else                                                 h.surplus = "ok";

  h.readBlocking = !(readMs < (double)FRAME_PERIOD_MS * 0.2);
  return h;
}


// A number that arrives as a STRING has to be checked before it is read.
//
// ArduinoJson's as<T>() does parse a numeric string - the vendored 6.21.5
// routes strings through parseNumber() - but it does NOT complain about a
// string that is not a number: parseNumber() fails, the variant stays null,
// and as<T>() hands back 0 with nothing said. So a setting that arrives in
// the wrong type is not a wrong value that gets reported - it is a wrong
// value that is accepted, and the setting then reads 0.
//
// It can arrive that way. The settings form posts a hidden twin that carries
// the field's TYPE, and the value comes in a second field with the same
// name. If the twin and the value arrive in the wrong order, the twin is
// read as the type and the value is written as a string - and that string is
// the type marker ("number"), not the number. A client posting the settings
// by hand can send a string too.
//
// So the AR's own numbers are read with the type checked. A string that is
// not a number is treated as "not there": the setting keeps the value it
// has, and the config is marked incomplete, so it is rewritten with the
// right value instead of with a silent zero.
//
// A string that IS a number is accepted and parsed by ArduinoJson as it
// always was. A hand-edited config file with "rtaFloor": "-50" is a
// legitimate thing to write and must not be thrown away.
//
// The test below is deliberately stricter than ArduinoJson's parser: it
// admits only what parseNumber() reads back as the same number, so a string
// that passes it can never turn into a silent zero. "nan", "inf" and "0x10"
// are refused - parseNumber() would make a float out of the first two, and
// stop at the 'x' in the third, and neither is a setting.
static bool arIsNumericText(const char *s) {
  if (!s || !*s) return false;
  bool digit = false;
  for (const char *p = s; *p; p++) {
    if (*p >= '0' && *p <= '9') { digit = true; continue; }
    if (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E') continue;
    return false;                 // letters, spaces, hex - none of them a setting
  }
  if (!digit) return false;
  char *end = nullptr;
  double d = strtod(s, &end);
  if (end == s || *end) return false;                       // trailing junk
  if (!(d == d) || d > 1e30 || d < -1e30) return false;     // NaN, infinity
  return true;
}

template<typename T>
static bool arGetNumber(const JsonVariant& v, T& dst) {
  if (v.is<const char*>()) {      // true for every string variant
    if (!arIsNumericText(v.as<const char*>())) return false;
  }
  return getJsonValue(v, dst);
}

// The same for a boolean. as<bool>() on a string returns TRUE - it is not a
// boolean, but ArduinoJson's asBoolean() has no "not a boolean" case and
// falls through to true. The two spellings a config file may use are spelled
// out here; anything else is refused rather than read as "true".
template<typename T>
static bool arGetBool(const JsonVariant& v, T& dst) {
  if (v.is<const char*>()) {
    const char *s = v.as<const char*>();
    if (!s) return false;
    if (!strcmp(s, "true")  || !strcmp(s, "1")) { dst = static_cast<T>(true);  return true; }
    if (!strcmp(s, "false") || !strcmp(s, "0")) { dst = static_cast<T>(false); return true; }
    return false;
  }
  return getJsonValue(v, dst);
}


class AudioReactive : public Usermod {

  private:
#ifdef ARDUINO_ARCH_ESP32

// HUB75 workaround - audio receive only
#ifdef WLED_ENABLE_HUB75MATRIX
#undef SR_DMTYPE
#define SR_DMTYPE 254  // "network receive only"
#endif
    #ifndef AUDIOPIN
    int8_t audioPin = -1;
    #else
    int8_t audioPin = AUDIOPIN;
    #endif
    #ifndef SR_DMTYPE // I2S mic type
    uint8_t dmType = 1; // 0=none/disabled/analog; 1=generic I2S
    #define SR_DMTYPE 1 // default type = I2S
    #else
    uint8_t dmType = SR_DMTYPE;
    #endif
    #ifndef I2S_SDPIN // aka DOUT
    int8_t i2ssdPin = 32;
    #else
    int8_t i2ssdPin = I2S_SDPIN;
    #endif
    #ifndef I2S_WSPIN // aka LRCL
    int8_t i2swsPin = 15;
    #else
    int8_t i2swsPin = I2S_WSPIN;
    #endif
    #ifndef I2S_CKPIN // aka BCLK
    int8_t i2sckPin = 14; /*PDM: set to I2S_PIN_NO_CHANGE*/
    #else
    int8_t i2sckPin = I2S_CKPIN;
    #endif
    #ifndef ES7243_SDAPIN
    int8_t sdaPin = -1;
    #else
    int8_t sdaPin = ES7243_SDAPIN;
    #endif
    #ifndef ES7243_SCLPIN
    int8_t sclPin = -1;
    #else
    int8_t sclPin = ES7243_SCLPIN;
    #endif
    #ifndef MCLK_PIN
    int8_t mclkPin = I2S_PIN_NO_CHANGE;  /* ESP32: only -1, 0, 1, 3 allowed*/
    #else
    int8_t mclkPin = MCLK_PIN;
    #endif
    // Runtime-configurable I2S parameters (replace legacy compile-time macros).
    #ifndef SR_I2S_BITS_PER_SAMPLE
    uint8_t i2sBitsPerSample = 32;       // 16, 24, or 32 — matches legacy 32-bit default
    #else
    uint8_t i2sBitsPerSample = SR_I2S_BITS_PER_SAMPLE;
    #endif
    #ifndef SR_I2S_USE_RIGHT_SLOT
    bool i2sUseRightSlot = false;        // false = LEFT slot, true = RIGHT slot
    #else
    bool i2sUseRightSlot = (SR_I2S_USE_RIGHT_SLOT != 0);
    #endif
    #ifndef SR_I2S_MASTER
    bool i2sMaster = true;               // false = I2S SLAVE role
    #else
    bool i2sMaster = (SR_I2S_MASTER != 0);
    #endif
#endif
    // new "V2" audiosync struct - 44 Bytes
    struct __attribute__ ((packed)) audioSyncPacket {  // WLEDMM "packed" ensures that there are no additional gaps
      char    header[6];      //  06 Bytes  offset 0 - "00002" for protocol version 2 ( includes \0 for c-style string termination) 
      uint8_t pressure[2];    //  02 Bytes, offset 6  - sound pressure as fixed point (8bit integer,  8bit fraction) 
      float   sampleRaw;      //  04 Bytes  offset 8  - either "sampleRaw" or "rawSampleAgc" depending on soundAgc setting
      float   sampleSmth;     //  04 Bytes  offset 12 - either "sampleAvg" or "sampleAgc" depending on soundAgc setting
      uint8_t samplePeak;     //  01 Bytes  offset 16 - 0 no peak; >=1 peak detected. In future, this will also provide peak Magnitude
      uint8_t frameCounter;   //  01 Bytes  offset 17 - rolling counter to track duplicate/out of order packets
      uint8_t fftResult[16];  //  16 Bytes  offset 18 - 16 GEQ channels, each channel has one byte (uint8_t)
      uint16_t zeroCrossingCount; // 02 Bytes, offset 34 - number of zero crossings seen in 23ms
      float  FFT_Magnitude;   //  04 Bytes  offset 36 - largest FFT result from a single run (raw value, can go up to 4096)
      float  FFT_MajorPeak;   //  04 Bytes  offset 40 - frequency (Hz) of largest FFT result
    };

    // old "V1" audiosync struct - 83 Bytes payload, 88 bytes total - for backwards compatibility
    struct audioSyncPacket_v1 {
      char header[6];         //  06 Bytes
      uint8_t myVals[32];     //  32 Bytes
      int32_t sampleAgc;          //  04 Bytes
      int32_t sampleRaw;          //  04 Bytes
      float sampleAvg;        //  04 Bytes
      bool samplePeak;        //  01 Bytes
      uint8_t fftResult[16];  //  16 Bytes
      double FFT_Magnitude;   //  08 Bytes
      double FFT_MajorPeak;   //  08 Bytes
    };

    #define UDPSOUND_MAX_PACKET 96 // max packet size for audiosync, with a bit of "headroom"

    // set your config variables to their boot default value (this can also be done in readFromConfig() or a constructor if you prefer)
  #if defined(SR_ENABLE_DEFAULT) || defined(UM_AUDIOREACTIVE_ENABLE)
    bool     enabled = true;        // WLEDMM
  #else
    bool     enabled = false;
  #endif
    bool     initDone = false;

    // variables  for UDP sound sync
    AsyncUDP fftUdp;               // UDP object for sound sync (from WiFi UDP, not Async UDP!)

    // Hardcore Buffering (Shared between Callback and Loop)
    volatile bool hasNewData = false;             // Flag to tell loop we have data
    uint8_t incomingBuffer[UDPSOUND_MAX_PACKET];  // Shared storage
    size_t incomingPacketSize = 0;                // Size of data in storage

    // const uint16_t delayMs = 10;
    uint16_t audioSyncPort = 11988;

    // bool updateIsRunning = false;

    unsigned long lastTime = 0;   // last time of running UDP Microphone Sync
#if defined(WLEDMM_FASTPATH)
    const uint16_t delayMs = 5;   // I don't want to sample too often and overload WLED
#else
    const uint16_t delayMs = 10;  // I don't want to sample too often and overload WLED
#endif
    // uint16_t audioSyncPort= 11988;// default port for UDP sound sync

    bool updateIsRunning = false; // true during OTA.

#ifdef ARDUINO_ARCH_ESP32
    // The level tracker's state lives in the file-scope statics near the
    // estimator (arLevelDb, arFloorDb, arGateOpen, arAgcDb): they are read by
    // the FFT task as well as by this one, and keeping them in one place is
    // what makes the "who writes what" question answerable.
#endif

    // variables used in effects
    int16_t  volumeRaw = 0;       // the published level, unsmoothed (0..255)
    float syncVolumeSmth = 0;
    float my_magnitude =0.0f;     // FFT_Magnitude
    float soundPressure = 0;      // Sound Pressure estimation, based on microphone raw readings. 0 ->5db, 255 ->105db

    // used to feed "Info" Page
    unsigned long last_UDPTime = 0;    // time of last valid UDP sound sync datapacket
    int receivedFormat = 0;            // last received UDP sound sync format - 0=none, 1=v1 (0.13.x), 2=v2 (0.14.x)

    // The transmit counters.
    //
    // The send is the one part of the sound sync that fails silently: the
    // framework's writeTo() returns 0 and says nothing about why, and a
    // multicast group is a destination lwip has no route for. These make the
    // failure visible instead of leaving the board looking connected while
    // nothing goes out.
    uint32_t txCalls = 0;             // send attempts
    uint32_t txOkIf = 0;              // sent, naming the interface
    uint32_t txOkPlain = 0;           // sent, no interface named
    uint32_t txFail = 0;              // sent nowhere
    uint32_t txBytes = 0;             // bytes handed to the stack
    uint32_t txLastMs = 0;            // when the last attempt was made
    int      txLastErr = 0;           // the stack's error code on the last failure
    int      txIf = -1;               // the adapter the send named (-1 = none)
    float maxSample5sec = 0.0f;        // max sample (after AGC) in last 5 seconds 
    unsigned long sampleMaxTimer = 0;  // last time maxSample5sec was reset
    #define CYCLE_SAMPLEMAX 3500       // time window for merasuring

    // strings to reduce flash memory usage (used more than twice)
    static const char _name[];
    static const char _enabled[];
    static const char _inputLvl[];
#if defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)
    static const char _analogmic[];
#endif
    static const char _digitalmic[];
    static const char UDP_SYNC_HEADER[];
    static const char UDP_SYNC_HEADER_v1[];

    // private methods

    ////////////////////
    // Debug support  //
    ////////////////////
    void logAudio()
    {
      if (disableSoundProcessing && (!udpSyncConnected || ((audioSyncEnabled & AUDIOSYNC_REC) == 0))) return;   // no audio available
    #ifdef MIC_LOGGER
      // Debugging functions for audio input and sound processing. Comment out the values you want to see
      PLOT_PRINT("volumeSmth:");  PLOT_PRINT(volumeSmth + 256.0f);  PLOT_PRINT("\t");  // +256 to move above other lines
      //PLOT_PRINT("volumeRaw:");   PLOT_PRINT(volumeRaw + 256.0f); PLOT_PRINT("\t");  // +256 to move above other lines
      //PLOT_PRINT("samplePeak:");  PLOT_PRINT((samplePeak!=0) ? 128:0);   PLOT_PRINT("\t");
    #ifdef ARDUINO_ARCH_ESP32
      PLOT_PRINT("micMin:");     PLOT_PRINT(0.5f * micReal_min);    PLOT_PRINT("\t");  // scaled down to 50%, for better readability
      PLOT_PRINT("micMax:");     PLOT_PRINT(0.5f * micReal_max);    PLOT_PRINT("\t");  // scaled down to 50%
      //PLOT_PRINT("micAvg:");     PLOT_PRINT(0.5f * micReal_avg);  PLOT_PRINT("\t");  // scaled down to 50%
      //PLOT_PRINT("micDC:");      PLOT_PRINT(0.5f * (micReal_min + micReal_max)/2.0f);PLOT_PRINT("\t");  // scaled down to 50%
      PLOT_PRINT("level_dB:");   PLOT_PRINT(arLevelDb);   PLOT_PRINT("\t");
      PLOT_PRINT("floor_dB:");   PLOT_PRINT(arFloorDb);   PLOT_PRINT("\t");
      PLOT_PRINT("range_dB:");     PLOT_PRINT(arAgcDb);     PLOT_PRINT("\t");
      PLOT_PRINT("gate:");       PLOT_PRINT(arGateOpen ? 1 : 0); PLOT_PRINT("\t");
      //PLOT_PRINT("sampleAgc:");   PLOT_PRINT(sampleAgc);   PLOT_PRINT("\t");
      //PLOT_PRINT("sampleAvg:");   PLOT_PRINT(sampleAvg);   PLOT_PRINT("\t");
    #endif
      PLOT_PRINTLN();
      PLOT_FLUSH();
    #endif

    #ifdef FFT_SAMPLING_LOG
      #if 0
        for(int i=0; i<NUM_GEQ_CHANNELS; i++) {
          PLOT_PRINT(fftResult[i]);
          PLOT_PRINT("\t");
        }
        PLOT_PRINTLN();
    #endif

      // OPTIONS are in the following format: Description \n Option
      //
      // Set true if wanting to see all the bands in their own vertical space on the Serial Plotter, false if wanting to see values in Serial Monitor
      const bool mapValuesToPlotterSpace = false;
      // Set true to apply an auto-gain like setting to to the data (this hasn't been tested recently)
      const bool scaleValuesFromCurrentMaxVal = false;
      // prints the max value seen in the current data
      const bool printMaxVal = false;
      // prints the min value seen in the current data
      const bool printMinVal = false;
      // if !scaleValuesFromCurrentMaxVal, we scale values from [0..defaultScalingFromHighValue] to [0..scalingToHighValue], lower this if you want to see smaller values easier
      const int defaultScalingFromHighValue = 256;
      // Print values to terminal in range of [0..scalingToHighValue] if !mapValuesToPlotterSpace, or [(i)*scalingToHighValue..(i+1)*scalingToHighValue] if mapValuesToPlotterSpace
      const int scalingToHighValue = 256;
      // set higher if using scaleValuesFromCurrentMaxVal and you want a small value that's also the current maxVal to look small on the plotter (can't be 0 to avoid divide by zero error)
      const int minimumMaxVal = 1;

      int maxVal = minimumMaxVal;
      int minVal = 0;
      for(int i = 0; i < NUM_GEQ_CHANNELS; i++) {
        if(fftResult[i] > maxVal) maxVal = fftResult[i];
        if(fftResult[i] < minVal) minVal = fftResult[i];
      }
      for(int i = 0; i < NUM_GEQ_CHANNELS; i++) {
        PLOT_PRINT(i); PLOT_PRINT(":");
        PLOT_PRINTF("%04ld ", map(fftResult[i], 0, (scaleValuesFromCurrentMaxVal ? maxVal : defaultScalingFromHighValue), (mapValuesToPlotterSpace*i*scalingToHighValue)+0, (mapValuesToPlotterSpace*i*scalingToHighValue)+scalingToHighValue-1));
      }
      if(printMaxVal) {
        PLOT_PRINTF("maxVal:%04d ", maxVal + (mapValuesToPlotterSpace ? 16*256 : 0));
      }
      if(printMinVal) {
        PLOT_PRINTF("%04d:minVal ", minVal);  // printed with value first, then label, so negative values can be seen in Serial Monitor but don't throw off y axis in Serial Plotter
      }
      if(mapValuesToPlotterSpace) {
        PLOT_PRINTF("max:%04d ", (printMaxVal ? 17 : 16)*256); // print line above the maximum value we expect to see on the plotter to avoid autoscaling y axis
      } else {
        PLOT_PRINTF("max:%04d ", 256);
      }
      PLOT_PRINTLN();
    #endif // FFT_SAMPLING_LOG
    } // logAudio()

#ifdef ARDUINO_ARCH_ESP32

    //////////////////////
    // Audio Processing //
    //////////////////////

    // The level, published for the volume-reactive effects.
    //
    // The level itself is measured in the FFT task - the RMS of the fresh hop,
    // in dB re full scale - because that is where the samples are. Here it is
    // only mapped into the 0..255 range the effects read, and it is gated:
    // with the gate shut there is no signal to show.
    //
    // There is no PI controller and no squelch on this value any more. Both
    // belonged to the old path, where the level was a peak-minus-floor
    // difference: the controller amplified a quantity that the floor itself
    // had already zeroed, which is the deadlock the new floor tracker removes
    // by construction.
    void publishLevel() {
      const float lvl = arGateOpen ? mapf(arLevelDb, AR_LEVEL_DB_LO, AR_LEVEL_DB_HI, 0.0f, 255.0f) : 0.0f;
      sampleAvg = lvl;
      sampleAgc = lvl;
      volumeSmth = lvl;
      volumeRaw  = (int16_t)lvl;
    }

    // getSample() is gone.
    //
    // It was the level tracker: a "peak minus floor" difference, smoothed,
    // squelched, and amplified by a PI controller. Every one of those steps
    // made the level a derived quantity, and the floor it subtracted was the
    // same number the gate compared against - so the gate read a value the
    // floor had already zeroed, and the floor could not rise until the signal
    // was already at the floor. That is the deadlock.
    //
    // The level is now measured directly (the RMS of the fresh hop, in dB) and
    // the floor is a separate minimum statistic, so nothing reads its own
    // output.


    // The current sensitivity: how far the input is below full scale.
    //
    // The old version returned the PI controller's gain factor - a number the
    // controller invented, not a property of the signal. The level is measured
    // directly now, so the sensitivity is the headroom: 0 at full scale and
    // 255 at 60 dB below it. The effects read it as "255 - sensitivity" to get
    // the input level.
    float getSensitivity()
    {
      const float s = mapf(arLevelDb, 0.0f, -60.0f, 0.0f, 255.0f);
      return fminf(fmaxf(s, 0.0f), 255.0f);
    }

    // estimate sound pressure, based on some assumptions :
    //   * full scale (0 dBFS) -> Acoustic overload point --> 105db ==> 255
    //   * 100 dB below      -> just above hearing level -->   5db ==> 0
    // see https://en.wikipedia.org/wiki/Sound_pressure#Examples_of_sound_pressure
    // use with I2S digital microphones. Expect stupid values for analog in, and with Line-In !!
    float estimatePressure() {
      // The level is measured as an RMS in dB re full scale, so the same
      // 5..105 dB range is a straight dB mapping - no logarithm needed. The
      // input-type corrections are the ones the old code applied to the sample
      // magnitude, now expressed in dB.
      float offsetDb = 0.0f;
      if (dmType == 0) offsetDb += 6.02f;   // correction for ADC analog (x2)
      if (dmType == 5) offsetDb += 6.02f;   // correction for PDM (x2)
      if (dmType == 4) offsetDb -= 20.0f;  // line-in is not a microphone: it runs hot
      const float dbSpl = 105.0f + arLevelDb + offsetDb;
      return fminf(fmaxf((dbSpl - 5.0f) * 2.55f, 0.0f), 255.0f);   // 5..105 dB -> 0..255
    }
#endif


    /* Limits the dynamics of volumeSmth (= sampleAvg or sampleAgc). 
     * does not affect FFTResult[] or volumeRaw ( = sample or rawSampleAgc) 
    */
    // effects: Gravimeter, Gravcenter, Gravcentric, Noisefire, Plasmoid, Freqpixels, Freqwave, Gravfreq, (2D Swirl, 2D Waverly)
    void limitSampleDynamics(void) {
      const float bigChange = 196;                  // just a representative number - a large, expected sample value
      static unsigned long last_time = 0;
      static float last_volumeSmth = 0.0f;

      if (limiterOn == false) return;

      long delta_time = millis() - last_time;
      delta_time = constrain(delta_time , 1, 1000); // below 1ms -> 1ms; above 1sec -> silly lil hick-up
      float deltaSample = volumeSmth - last_volumeSmth;

      if (attackTime > 0) {                         // user has defined attack time > 0
        float maxAttack =   bigChange * float(delta_time) / float(attackTime);
        if (deltaSample > maxAttack) deltaSample = maxAttack;
      }
      if (decayTime > 0) {                          // user has defined decay time > 0
        float maxDecay  = - bigChange * float(delta_time) / float(decayTime);
        if (deltaSample < maxDecay) deltaSample = maxDecay;
      }

      volumeSmth = last_volumeSmth + deltaSample; 

      last_volumeSmth = volumeSmth;
      last_time = millis();
    }

    // MM experimental: limiter to smooth GEQ samples (only for UDP sound receiver mode)
    //   target value (if gotNewSample) : fftCalc
    //   last filtered value: fftAvg
    void limitGEQDynamics(bool gotNewSample) {
      constexpr float bigChange = 202;                  // just a representative number - a large, expected sample value
      constexpr float smooth = 0.8f;                    // a bit of filtering
      static unsigned long last_time = 0;

      if (limiterOn == false) return;

      // The published bytes are copied out and put back at the end, through
      // the double buffer, so the effects never see a half-written frame.
      uint8_t bands[NUM_GEQ_CHANNELS];
      memcpy(bands, fftResult, sizeof(bands));

      if (gotNewSample) { // take new FFT samples as target values
        for(unsigned i=0; i < NUM_GEQ_CHANNELS; i++) {
          fftCalc[i] = bands[i];
          bands[i] = (uint8_t)fftAvg[i];
        }
      }

      long delta_time = millis() - last_time;
      delta_time = constrain(delta_time , 1, 1000); // below 1ms -> 1ms; above 1sec -> silly lil hick-up
      float maxAttack = (attackTime <= 0) ?  255.0f : (bigChange * float(delta_time) / float(attackTime));
      float maxDecay  = (decayTime <= 0)  ? -255.0f : (-bigChange * float(delta_time) / float(decayTime));

      for(unsigned i=0; i < NUM_GEQ_CHANNELS; i++) {
        float deltaSample = fftCalc[i] - fftAvg[i];
        if (deltaSample > maxAttack) deltaSample = maxAttack;
        if (deltaSample < maxDecay) deltaSample = maxDecay;
        deltaSample = deltaSample * smooth;
        fftAvg[i] = fmaxf(0.0f, fminf(255.0f, fftAvg[i] + deltaSample));
        bands[i] = (uint8_t)fftAvg[i];
      }
      arPublishRaw(bands);
      last_time = millis();
    }

    //////////////////////
    // UDP Sound Sync   //
    //////////////////////

    // try to establish UDP sound sync connection

    void connectUDPSoundSync(void) {
      static unsigned long last_connection_attempt = 0;

      if ((audioSyncPort <= 0) || (audioSyncEnabled == AUDIOSYNC_NONE)) return;
      if (!(apActive || WLED_CONNECTED || interfacesInited)) {
        if (udpSyncConnected) {
          udpSyncConnected = false;
          fftUdp.close(); // AsyncUDP uses close(), not stop()
          receivedFormat = 0;
        }
        return;
      }
      if (udpSyncConnected) return;
      if (millis() - last_connection_attempt < 5000) return; // Retry every 5s
      if (updateIsRunning) return;

      last_connection_attempt = millis();
      connected();
    }

    void connected() {
      if (udpSyncConnected) {   // clean-up: if open, close old UDP sync connection
        udpSyncConnected = false;
        fftUdp.close();         // <--- FIXED: changed from stop() to close()
        receivedFormat = 0;
        DEBUGSR_PRINTLN(F("AR connected(): old UDP connection closed."));
      }

      if ((audioSyncPort > 0) && (audioSyncEnabled > AUDIOSYNC_NONE)) {
        IPAddress multicastGroup(239, 0, 0, 1);

        // <--- FIXED: changed from beginMulticast() to listenMulticast()
        if (fftUdp.listenMulticast(multicastGroup, audioSyncPort)) {
          udpSyncConnected = true;

          // Register the Async Callback
          fftUdp.onPacket([this](AsyncUDPPacket packet) {
            if (packet.length() > UDPSOUND_MAX_PACKET) return;
            memcpy(this->incomingBuffer, packet.data(), packet.length());
            this->incomingPacketSize = packet.length();
            this->hasNewData = true;
            this->last_UDPTime = millis();
            });

          if (apActive && !(WLED_CONNECTED)) {
            DEBUGSR_PRINTLN(F("AR connected(): UDP: connected using AP."));
          } else {
            DEBUGSR_PRINTLN(F("AR connected(): UDP: connected to WIFI."));
          }
        } else {
          DEBUGSR_PRINTLN(F("AR connected(): UDP: FAILED to connect."));
        }
      }
    }

    // Which adapter the sound sync leaves by.
    //
    // A multicast group is not a destination lwip has a route to, so the
    // plain writeTo() - which asks for one - can come back empty where a
    // send that names the adapter goes straight out of the interface the
    // board is on. The framework's writeTo() falls back to the routeless
    // send when the adapter it is given does not exist, so naming the wrong
    // one costs nothing.
    //
    // The adapter is the one with the board's address, and the default
    // netif wins when more than one has one.
    static tcpip_adapter_if_t arSyncAdapterIf() {
#ifdef ARDUINO_ARCH_ESP32
      static const char* keys[TCPIP_ADAPTER_IF_MAX] = {
        "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF", "PPP_DEF"
      };
      static tcpip_adapter_if_t cached = TCPIP_ADAPTER_IF_MAX;
      static uint32_t cachedAt = 0;

      // The answer only changes when the board's address does, and that
      // takes seconds. Re-ask now and then, and straight away when the
      // last answer was "none".
      const uint32_t now = millis();
      if (cached != TCPIP_ADAPTER_IF_MAX && (now - cachedAt) < 2000) return cached;

      esp_netif_t* def = esp_netif_get_default_netif();
      tcpip_adapter_if_t found = TCPIP_ADAPTER_IF_MAX;
      for (int pass = 0; (pass < 2) && (found == TCPIP_ADAPTER_IF_MAX); pass++) {
        for (int i = 0; i < TCPIP_ADAPTER_IF_MAX; i++) {
          esp_netif_t* h = esp_netif_get_handle_from_ifkey(keys[i]);
          if (h == nullptr) continue;
          if (pass == 0 && h != def) continue;   // the default netif first
          if (pass == 1 && h == def) continue;
          esp_netif_ip_info_t info;
          if (esp_netif_get_ip_info(h, &info) != ESP_OK) continue;
          if (info.ip.addr == 0) continue;        // not up
          found = (tcpip_adapter_if_t)i;
          break;
        }
      }
      cached = found;
      cachedAt = now;
      return found;
#else
      return TCPIP_ADAPTER_IF_MAX;
#endif
    }

    void transmitAudioData() {
      if (!udpSyncConnected) return; // We reuse the listener socket for sending
      static uint8_t frameCounter = 0;

      audioSyncPacket transmitData;
      memset(reinterpret_cast<void*>(&transmitData), 0, sizeof(transmitData));

      strncpy_P(transmitData.header, PSTR(UDP_SYNC_HEADER), 6);
      transmitData.sampleRaw = volumeRaw;
      transmitData.sampleSmth = volumeSmth;
      transmitData.samplePeak = udpSamplePeak ? 1 : 0;
      udpSamplePeak = false;
      transmitData.frameCounter = frameCounter;
      transmitData.zeroCrossingCount = zeroCrossingCount;

      // Copy the published bytes out in one go: the pointer is swapped by the
      // publisher, so reading it band by band could straddle a swap.
      {
        uint8_t bands[NUM_GEQ_CHANNELS];
        memcpy(bands, fftResult, sizeof(bands));
        for (int i = 0; i < NUM_GEQ_CHANNELS; i++) {
          transmitData.fftResult[i] = bands[i];
        }
      }

      uint32_t pressure16bit = max(0.0f, soundPressure) * 256.0f;
      uint16_t pressInt = pressure16bit / 256;
      uint16_t pressFract = pressure16bit % 256;
      if (pressInt > 255) pressInt = 255;
      transmitData.pressure[0] = (uint8_t)pressInt;
      transmitData.pressure[1] = (uint8_t)pressFract;

      transmitData.FFT_Magnitude = my_magnitude;
      transmitData.FFT_MajorPeak = FFT_MajorPeak;

      // AsyncUDP Send
      //
      // The packet is the v2 format and stays the v2 format: 16 bands, 44
      // bytes, byte-identical to what every other WLED instance expects. The
      // 31 RTA bands are NOT in here - they go out board-locally to the
      // effects, and adding them to this packet is what broke the other
      // instances.
      IPAddress multicastGroup(239, 0, 0, 1);
      const uint8_t* pbuf = reinterpret_cast<uint8_t*>(&transmitData);
      const size_t   plen = sizeof(transmitData);

      txCalls++;
      txLastMs = millis();
      size_t sent = 0;

      // Name the adapter: a multicast group has no route, and the routeless
      // send is the one that fails silently.
      const tcpip_adapter_if_t ifc = arSyncAdapterIf();
      txIf = (int)ifc;
      if (ifc < TCPIP_ADAPTER_IF_MAX) {
        sent = fftUdp.writeTo(pbuf, plen, multicastGroup, audioSyncPort, ifc);
        if (sent) txOkIf++;
      }
      if (!sent) {
        // Fall back to the routeless send, and remember how that went.
        sent = fftUdp.writeTo(pbuf, plen, multicastGroup, audioSyncPort);
        if (sent) txOkPlain++;
      }
      if (sent) {
        txBytes += sent;
      } else {
        txFail++;
        txLastErr = (int)fftUdp.lastErr();
        if (txFail == 1) {
          DEBUGSR_PRINTLN(F("AR: UDP sound sync send FAILED."));
          DEBUG_PRINTF("AR: UDP sound sync send error %d on adapter %d\n", txLastErr, txIf);
        }
      }

      frameCounter++;
    }

    static bool isValidUdpSyncVersion(const char *header) {
      return strncmp_P(header, UDP_SYNC_HEADER, 6) == 0;
    }
    static bool isValidUdpSyncVersion_v1(const char *header) {
      return strncmp_P(header, UDP_SYNC_HEADER_v1, 6) == 0;
    }

    bool decodeAudioData(int packetSize, uint8_t *fftBuff) {
      if((0 == packetSize) || (nullptr == fftBuff)) return false; // sanity check
      //audioSyncPacket *receivedPacket = reinterpret_cast<audioSyncPacket*>(fftBuff);
      audioSyncPacket receivedPacket;
      memset(&receivedPacket, 0, sizeof(receivedPacket));                                  // start clean
      memcpy(&receivedPacket, fftBuff, min((unsigned)packetSize, (unsigned)sizeof(receivedPacket))); // don't violate alignment - thanks @willmmiles

      // validate sequence, discard out-of-sequence packets
      static uint8_t lastFrameCounter = 0;
      // add info for UI
      if ((receivedPacket.frameCounter > 0) && (lastFrameCounter > 0)) receivedFormat = 3; // v2+
      else receivedFormat = 2; // v2
      // check sequence
      bool sequenceOK = false;
      if(receivedPacket.frameCounter > lastFrameCounter) sequenceOK = true;                  // sequence OK
      if((lastFrameCounter < 12) && (receivedPacket.frameCounter > 248)) sequenceOK = false; // prevent sequence "roll-back" due to late packets (1->254)
      if((lastFrameCounter > 248) && (receivedPacket.frameCounter < 12)) sequenceOK = true;  // handle roll-over (255 -> 0)
      if(audioSyncSequence == false) sequenceOK = true;                                       // sequence checking disabled by user
      if((sequenceOK == false) && (receivedPacket.frameCounter != 0)) {                      // always accept "0" - its the legacy value
        DEBUGSR_PRINTF("Skipping audio frame out of order or duplicated - %u vs %u\n", lastFrameCounter, receivedPacket.frameCounter);
        return false;   // reject out-of sequence frame
      }
      else {
        lastFrameCounter = receivedPacket.frameCounter;
      }

      // update samples for effects
      volumeSmth   = fmaxf(receivedPacket.sampleSmth, 0.0f);
      volumeRaw    = fmaxf(receivedPacket.sampleRaw, 0.0f);
#ifdef ARDUINO_ARCH_ESP32
      // update internal samples
      sampleAvg    = volumeSmth;
      sampleAgc    = volumeSmth;
#endif
      // Only change samplePeak IF it's currently false.
      // If it's true already, then the animation still needs to respond.
      autoResetPeak();
      if (!samplePeak) {
            samplePeak = receivedPacket.samplePeak >0 ? true:false;
            if (samplePeak) timeOfPeak = millis();
            //userVar1 = samplePeak;
      }
      //These values are only computed by ESP32
      // They are published through the double buffer, so the effects never see
      // a half-written frame.
      arPublishRaw(receivedPacket.fftResult);
      my_magnitude  = fmaxf(receivedPacket.FFT_Magnitude, 0.0f);
      FFT_Magnitude = my_magnitude;
      FFT_MajorPeak = constrain(receivedPacket.FFT_MajorPeak, 1.0f, NYQUIST_HZ);  // restrict value to range expected by effects
#ifdef ARDUINO_ARCH_ESP32
      FFT_MajPeakSmth = FFT_MajPeakSmth + 0.42f * (FFT_MajorPeak - FFT_MajPeakSmth); // simulate smooth value
#endif
      agcSensitivity = 128.0f; // substitute - V2 format does not include this value
      zeroCrossingCount = receivedPacket.zeroCrossingCount;

      // WLEDMM extract soundPressure
      if ((receivedPacket.pressure[0] != 0) || (receivedPacket.pressure[1] != 0)) {
        // found something in gap "reserved2"
        soundPressure  = float(receivedPacket.pressure[1]) / 256.0f; // fractional part
        soundPressure += float(receivedPacket.pressure[0]);          // integer part
      } else {
        soundPressure = volumeSmth; // fallback
      }

      return true;
    }

    void decodeAudioData_v1(int packetSize, uint8_t *fftBuff) {
      audioSyncPacket_v1 *receivedPacket = reinterpret_cast<audioSyncPacket_v1*>(fftBuff);
      // update samples for effects
      volumeSmth   = fmaxf(receivedPacket->sampleAgc, 0.0f);
      volumeRaw    = volumeSmth;   // V1 format does not have "raw" AGC sample
#ifdef ARDUINO_ARCH_ESP32
      // update internal samples
      sampleAvg    = fmaxf(receivedPacket->sampleAvg, 0.0f);;
      sampleAgc    = volumeSmth;
#endif
      // Only change samplePeak IF it's currently false.
      // If it's true already, then the animation still needs to respond.
      autoResetPeak();
      if (!samplePeak) {
            samplePeak = receivedPacket->samplePeak >0 ? true:false;
            if (samplePeak) timeOfPeak = millis();
            //userVar1 = samplePeak;
      }
      //These values are only available on the ESP32
      // Published through the double buffer - see decodeAudioData().
      arPublishRaw(receivedPacket->fftResult);
      my_magnitude  = fmaxf(receivedPacket->FFT_Magnitude, 0.0);
      FFT_Magnitude = my_magnitude;
      FFT_MajorPeak = constrain(receivedPacket->FFT_MajorPeak, 1.0, NYQUIST_HZ);  // restrict value to range expected by effects
      soundPressure = volumeSmth; // substitute - V1 format does not include this value
      agcSensitivity = 128.0f; // substitute - V1 format does not include this value
    }

    // bool receiveAudioData() {
    //   if (!udpSyncConnected) return false;
    //   bool haveFreshData = false;
    //   size_t packetSize = 0;
    //   static uint8_t fftUdpBuffer[UDPSOUND_MAX_PACKET + 1] = {0};
    //   size_t lastValidPacketSize = 0;

    //   // Loop to read all available packets
    //   while (true) {
    //     #if __cpp_exceptions
    //     try {
    //       packetSize = fftUdp.parsePacket();
    //     } catch (...) {
    //       packetSize = 0;
    //       #ifdef ARDUINO_ARCH_ESP32
    //       fftUdp.flush();
    //       #endif
    //       DEBUG_PRINTLN(F("receiveAudioData: parsePacket out of memory exception caught!"));
    //       USER_FLUSH();
    //       continue; // Skip to next iteration
    //     }
    //     #else
    //     packetSize = fftUdp.parsePacket();
    //     #endif

    //     #ifdef ARDUINO_ARCH_ESP32
    //     if ((packetSize > 0) && ((packetSize < 5) || (packetSize > UDPSOUND_MAX_PACKET))) {
    //       fftUdp.flush();
    //       continue; // Skip invalid packets
    //     }
    //     #endif

    //     if (packetSize == 0) break; // No more packets available

    //     if ((packetSize > 5) && (packetSize <= UDPSOUND_MAX_PACKET)) {
    //       fftUdp.read(fftUdpBuffer, packetSize);
    //       lastValidPacketSize = packetSize;
    //     }
    //   }

    //   // Process only the last valid packet
    //   if (lastValidPacketSize > 0) {
    //     if (lastValidPacketSize == sizeof(audioSyncPacket) && (isValidUdpSyncVersion((const char *)fftUdpBuffer))) {
    //       receivedFormat = 2;
    //       haveFreshData = decodeAudioData(lastValidPacketSize, fftUdpBuffer);
    //     } else if (lastValidPacketSize == sizeof(audioSyncPacket_v1) && (isValidUdpSyncVersion_v1((const char *)fftUdpBuffer))) {
    //       decodeAudioData_v1(lastValidPacketSize, fftUdpBuffer);
    //       receivedFormat = 1;
    //       haveFreshData = true;
    //     } else {
    //       receivedFormat = 0; // unknown format
    //     }
    //   }
    //   return haveFreshData;
    // }

    // bool receiveAudioData() {
    //   if (!udpSyncConnected) return false;

    //   bool haveFreshData = false;
    //   size_t packetSize = 0;

    //   // Static buffer to avoid memory allocation overhead every loop
    //   static uint8_t fftUdpBuffer[UDPSOUND_MAX_PACKET + 1] = { 0 };
    //   size_t lastValidPacketSize = 0;

    //   // --- HARDCORE ETHERNET FLUSH ---
    //   // We must empty the FIFO buffer to get to the LATEST packet.
    //   // Safety Cap: 25 iterations. 
    //   // If we process >25 packets, we are taking too long; bail out to let LEDs draw.
    //   int safetyBailout = 25;

    //   while (safetyBailout-- > 0) {
    //     // 1. Check next packet in the queue
    //     packetSize = fftUdp.parsePacket();

    //     // 2. If 0, queue is empty! We are finally "real-time".
    //     if (packetSize == 0) break;

    //     // 3. Consume the packet (Required to move queue forward)
    //     // We overwrite the SAME buffer repeatedly. This is a fast RAM copy.
    //     if (packetSize <= UDPSOUND_MAX_PACKET) {
    //       fftUdp.read(fftUdpBuffer, packetSize);
    //       lastValidPacketSize = packetSize;
    //     } else {
    //       // Packet too big? Just dump it to clear the interface.
    //       fftUdp.flush();
    //     }
    //   }

    //   // --- DECODE ONLY THE WINNER ---
    //   // We disregarded all previous packets. We only care about the very last one 
    //   // we saw before the buffer ran dry.
    //   if (lastValidPacketSize > 0) {
    //     // Only run expensive decoding logic ONCE per frame
    //     if (isValidUdpSyncVersion((const char*)fftUdpBuffer)) {
    //       receivedFormat = 2;
    //       haveFreshData = decodeAudioData(lastValidPacketSize, fftUdpBuffer);
    //     } else if (isValidUdpSyncVersion_v1((const char*)fftUdpBuffer)) {
    //       receivedFormat = 1;
    //       decodeAudioData_v1(lastValidPacketSize, fftUdpBuffer);
    //       haveFreshData = true;
    //     } else {
    //       receivedFormat = 0;
    //     }
    //   }

    //   return haveFreshData;
    // }

    // This is no longer "Receive", it is "Process what we received"
    bool processNetworkData() {
      if (!hasNewData) return false;
      
      // Clear flag immediately
      hasNewData = false;
      
      // Process whatever is in the buffer
      bool success = false;
      
      if (isValidUdpSyncVersion((const char *)incomingBuffer)) {
          receivedFormat = 2;
          success = decodeAudioData(incomingPacketSize, incomingBuffer);
      } else if (isValidUdpSyncVersion_v1((const char *)incomingBuffer)) {
          receivedFormat = 1;
          decodeAudioData_v1(incomingPacketSize, incomingBuffer);
          success = true;
      } else {
          receivedFormat = 0;
      }
      
      return success;
    }

    //////////////////////
    // usermod functions//
    //////////////////////

  public:
    //Functions called by WLED or other usermods

    /*
     * setup() is called once at boot. WiFi is not yet connected at this point.
     * You can use it to initialize variables, sensors or similar.
     * It is called *AFTER* readFromConfig()
     */
    void setup()
    {
      disableSoundProcessing = true; // just to be sure
      if (!initDone) {
        // usermod exchangeable data
        // we will assign all usermod exportable data here as pointers to original variables or arrays and allocate memory for pointers
        um_data = new um_data_t;
        // 12 entries, and it stays 12. The structure is carried between WLED
        // instances and serialised with its type tags, so a 13th entry
        // invalidates the packet on every instance that does not have this
        // code. The 31 RTA bands go out board-locally instead - see
        // arRtaBands in fcn_declare.h.
        um_data->u_size = 12;
        um_data->u_type = new um_types_t[um_data->u_size];
        um_data->u_data = new void*[um_data->u_size];
        um_data->u_data[0] = &volumeSmth;      //*used (New)
        um_data->u_type[0] = UMT_FLOAT;
        um_data->u_data[1] = &volumeRaw;       // used (New)
        um_data->u_type[1] = UMT_UINT16;
        um_data->u_data[2] = fftResult;        //*used (Blurz, DJ Light, Noisemove, GEQ_base, 2D Funky Plank, Akemi)
        um_data->u_type[2] = UMT_BYTE_ARR;
        // The FFT task is a free function with no access to this class, so it
        // needs a way to repoint u_data[2] when it swaps the double buffer.
        arUmData = um_data;
        um_data->u_data[3] = &samplePeak;      //*used (Puddlepeak, Ripplepeak, Waterfall)
        um_data->u_type[3] = UMT_BYTE;
        um_data->u_data[4] = &FFT_MajorPeak;   //*used (Ripplepeak, Freqmap, Freqmatrix, Freqpixels, Freqwave, Gravfreq, Rocktaves, Waterfall)
        um_data->u_type[4] = UMT_FLOAT;
        um_data->u_data[5] = &my_magnitude;    // used (New)
        um_data->u_type[5] = UMT_FLOAT;
        um_data->u_data[6] = &maxVol;          // assigned in effect function from UI element!!! (Puddlepeak, Ripplepeak, Waterfall)
        um_data->u_type[6] = UMT_BYTE;
        um_data->u_data[7] = &binNum;          // assigned in effect function from UI element!!! (Puddlepeak, Ripplepeak, Waterfall)
        um_data->u_type[7] = UMT_BYTE;
#ifdef ARDUINO_ARCH_ESP32
        um_data->u_data[8] = &FFT_MajPeakSmth; // new
        um_data->u_type[8] = UMT_FLOAT;
#else
        um_data->u_data[8] = &FFT_MajorPeak;   // substitute for 8266
        um_data->u_type[8] = UMT_FLOAT;
#endif
        um_data->u_data[9]  = &soundPressure;  // used (New)
        um_data->u_type[9]  = UMT_FLOAT;
        um_data->u_data[10] = &agcSensitivity; // used (New) - dummy value on 8266
        um_data->u_type[10] = UMT_FLOAT;
        um_data->u_data[11] = &zeroCrossingCount; // for auto playlist usermod
        um_data->u_type[11] = UMT_UINT16;
      }

#ifdef ARDUINO_ARCH_ESP32

      // Reset I2S peripheral for good measure - not needed in esp-idf v4.4.x and later.
    #if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(4, 4, 0)
      i2s_driver_uninstall(I2S_NUM_0);   // E (696) I2S: i2s_driver_uninstall(2006): I2S port 0 has not installed
      #if !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32P4)
        delay(100);
        periph_module_reset(PERIPH_I2S0_MODULE);   // not possible on -C3
      #endif
    #endif
      delay(100);         // Give that poor microphone some time to setup.

      #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3)
        if ((i2sckPin == I2S_PIN_NO_CHANGE) && (i2ssdPin >= 0) && (i2swsPin >= 0) 
            && ((dmType == 1) || (dmType == 4)) ) dmType = 51;   // dummy user support: SCK == -1 --means--> PDM microphone
      #endif

      useInputFilter = 2; // default: DC blocker
      switch (dmType) {
      #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
        // stub cases for not-yet-supported I2S modes on other ESP32 chips
        #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3)
        case 5:  //PDM Microphone
        case 51: //legacy PDM Microphone
        #endif
      #endif
        case 1:
          DEBUGSR_PRINT(F("AR: Generic I2S Microphone - "));
          DEBUGSR_PRINTLN(F(i2sUseRightSlot ? "right channel" : "left channel"));
          audioSource = new I2SSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          delay(100);
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, I2S_GPIO_UNUSED,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        case 2:
          DEBUGSR_PRINTLN(F("AR: ES7243 Microphone."));
          //useInputFilter = 0; // in case you need to disable low-cut software filtering
          audioSource = new ES7243(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          delay(100);
          // WLEDMM align global pins
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin; // copy usermod prefs into globals (if globals not defined)
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;                        // -1 = use global
          if (i2c_scl >= 0) sclPin = -1;

          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        case 3:
          DEBUGSR_PRINT(F("AR: SPH0645 Microphone - "));
          DEBUGSR_PRINTLN(F(i2sUseRightSlot ? "right channel" : "left channel"));
          audioSource = new SPH0654(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          delay(100);
          audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, I2S_GPIO_UNUSED,
                                  i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        case 4:
          DEBUGSR_PRINT(F("AR: Generic I2S Microphone with Master Clock - "));
          DEBUGSR_PRINTLN(F(i2sUseRightSlot ? "right channel" : "left channel"));
          audioSource = new I2SSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f/24.0f, i2sMaster);
          delay(100);
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        #if  !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3)
        case 5:
          DEBUGSR_PRINT(F("AR: I2S PDM Microphone - "));
          DEBUGSR_PRINTLN(F(i2sUseRightSlot ? "right channel" : "left channel"));
          audioSource = new I2SSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f/4.0f, i2sMaster);
          useInputFilter = 1;  // PDM bandpass filter - this reduces the noise floor on SPM1423 from 5% Vpp (~380) down to 0.05% Vpp (~5)
          delay(100);
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, I2S_GPIO_UNUSED, I2S_GPIO_UNUSED,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        case 51:
          DEBUGSR_PRINT(F("AR: Legacy PDM Microphone - "));
          DEBUGSR_PRINTLN(F(i2sUseRightSlot ? "right channel" : "left channel"));
          audioSource = new I2SSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          useInputFilter = 1;  // PDM bandpass filter
          delay(100);
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, I2S_GPIO_UNUSED, I2S_GPIO_UNUSED,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        #endif
        case 6:
        #ifdef use_es8388_mic
          DEBUGSR_PRINTLN(F("AR: ES8388 Source (Mic)"));
        #else
          DEBUGSR_PRINTLN(F("AR: ES8388 Source (Line-In)"));
        #endif
          audioSource = new ES8388Source(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          //useInputFilter = 0; // to disable low-cut software filtering and restore previous behaviour
          delay(100);
          // WLEDMM align global pins
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin; // copy usermod prefs into globals (if globals not defined)
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;                        // -1 = use global
          if (i2c_scl >= 0) sclPin = -1;

          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        case 7:
        #ifdef use_wm8978_mic
          DEBUGSR_PRINTLN(F("AR: WM8978 Source (Mic)"));
        #else
          DEBUGSR_PRINTLN(F("AR: WM8978 Source (Line-In)"));
        #endif
          audioSource = new WM8978Source(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          //useInputFilter = 0; // to disable low-cut software filtering and restore previous behaviour
          delay(100);
          // WLEDMM align global pins
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin; // copy usermod prefs into globals (if globals not defined)
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;                        // -1 = use global
          if (i2c_scl >= 0) sclPin = -1;

          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        case 8:
          DEBUGSR_PRINTLN(F("AR: AC101 Source (Line-In)"));
          audioSource = new AC101Source(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          //useInputFilter = 0; // to disable low-cut software filtering and restore previous behaviour
          delay(100);
          // WLEDMM align global pins
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin; // copy usermod prefs into globals (if globals not defined)
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;                        // -1 = use global
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        case 9:
          DEBUGSR_PRINTLN(F("AR: ES8311 Source (Mic)"));
          audioSource = new ES8311Source(SAMPLE_RATE, BLOCK_SIZE, 1.0f, i2sMaster);
          //useInputFilter = 0; // to disable low-cut software filtering and restore previous behaviour
          delay(100);
          // WLEDMM align global pins
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin; // copy usermod prefs into globals (if globals not defined)
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;                        // -1 = use global
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin,
                                                    i2sBitsPerSample, i2sUseRightSlot, i2sMaster);
          break;
        #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
        #ifdef CONFIG_CODEC_ES8311_SUPPORT
        case 10:
          DEBUGSR_PRINTLN(F("AR: ES8311 IDF (esp_codec_dev)"));
          audioSource = new ES8311IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif
        #ifdef CONFIG_CODEC_ES8388_SUPPORT
        case 11:
          DEBUGSR_PRINTLN(F("AR: ES8388 IDF (esp_codec_dev)"));
          audioSource = new ES8388IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif
        #ifdef CONFIG_CODEC_ES8374_SUPPORT
        case 12:
          DEBUGSR_PRINTLN(F("AR: ES8374 IDF (esp_codec_dev)"));
          audioSource = new ES8374IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif  // CONFIG_CODEC_ES8374_SUPPORT
        #ifdef CONFIG_CODEC_ZL38063_SUPPORT
        case 13:
          DEBUGSR_PRINTLN(F("AR: ZL38063 IDF (esp_codec_dev)"));
          audioSource = new ZL38063IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif  // CONFIG_CODEC_ZL38063_SUPPORT
        #ifdef CONFIG_CODEC_ES8389_SUPPORT
        case 14:
          DEBUGSR_PRINTLN(F("AR: ES8389 IDF (esp_codec_dev)"));
          audioSource = new ES8389IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif
        #ifdef CONFIG_CODEC_ES7210_SUPPORT
        case 15:
          DEBUGSR_PRINTLN(F("AR: ES7210 IDF (esp_codec_dev, record only)"));
          audioSource = new ES7210IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif
        #ifdef CONFIG_CODEC_ES7243_SUPPORT
        case 16:
          DEBUGSR_PRINTLN(F("AR: ES7243 IDF (esp_codec_dev, record only)"));
          audioSource = new ES7243IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif
        #ifdef CONFIG_CODEC_ES7243E_SUPPORT
        case 17:
          DEBUGSR_PRINTLN(F("AR: ES7243E IDF (esp_codec_dev, record only)"));
          audioSource = new ES7243EIDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif
        #ifdef CONFIG_CODEC_CJC8910_SUPPORT
        case 18:
          DEBUGSR_PRINTLN(F("AR: CJC8910 IDF (esp_codec_dev)"));
          audioSource = new CJC8910IDFSource(SAMPLE_RATE, BLOCK_SIZE, 1.0f);
          delay(100);
          if ((sdaPin >= 0) && (i2c_sda < 0)) i2c_sda = sdaPin;
          if ((sclPin >= 0) && (i2c_scl < 0)) i2c_scl = sclPin;
          if (i2c_sda >= 0) sdaPin = -1;
          if (i2c_scl >= 0) sclPin = -1;
          if (audioSource) audioSource->initialize(i2swsPin, i2ssdPin, i2sckPin, mclkPin);
          break;
        #endif  // CONFIG_CODEC_CJC8910_SUPPORT
        #endif  // ESP_IDF >= 4.4

          case 255: // falls through
          case 254: // dummy "network receive only" driver
            if (audioSource) delete audioSource;
            audioSource = nullptr;
            disableSoundProcessing = true;
            audioSyncEnabled = AUDIOSYNC_REC; // force udp sound receive mode
          break;

        #if  !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ARCH_RISCV)
        // ADC over I2S is only possible on "classic" ESP32. The I2SAdcSource
        // class was removed in the IDF v5 migration; the analog-ADC code path
        // is no longer reachable. The case/default arm is kept as a comment
        // so a future author knows the gap exists.
        //
        // case 0: default:
        //   useInputFilter = 1;
        //   audioSource = new I2SAdcSource(SAMPLE_RATE, BLOCK_SIZE);  // <-- no such class anymore
        //   ...
        #endif
      }
      delay(250); // give microphone enough time to initialise

      if (!audioSource && (dmType < 254)) enabled = false;      // audio failed to initialise
#endif
      if (enabled) onUpdateBegin(false);                        // create FFT task, and initialize network

#ifdef ARDUINO_ARCH_ESP32
      if (audioSource && FFT_Task == nullptr) enabled = false; // FFT task creation failed
      if((!audioSource) || (!audioSource->isInitialized())) {  // audio source failed to initialize. Still stay "enabled", as there might be input arriving via UDP Sound Sync 

        if (dmType < 254) { USER_PRINTLN(F("AR: Failed to initialize sound input driver. Please check input PIN settings."));}
        else  { USER_PRINTLN(F("AR: No sound input driver configured - network receive only."));}
        disableSoundProcessing = true;
      } else {
        USER_PRINTLN(F("AR: sound input driver initialized successfully."));        
      }
#endif
      if (enabled) disableSoundProcessing = false;       // all good - enable audio processing
      // try to start UDP
      last_UDPTime = 0;
      receivedFormat = 0;
      delay(100);
      if (enabled) connectUDPSoundSync();
      initDone = true;
      DEBUGSR_PRINT(F("AR: init done, enabled = "));
      DEBUGSR_PRINTLN(enabled ? F("true.") : F("false."));
      USER_FLUSH();

      // dump audiosync data layout
      #if defined(SR_DEBUG)
      {
        audioSyncPacket data;
        USER_PRINTF("\naudioSyncPacket_v1 size = %d\n", sizeof(audioSyncPacket_v1));                                                         // size 88
        USER_PRINTF("audioSyncPacket size = %d\n", sizeof(audioSyncPacket));                                                               // size 44
        USER_PRINTF("|  char    header[6]     offset = %2d   size = %2d\n", offsetof(audioSyncPacket, header[0]), sizeof(data.header));           // offset  0 size 6
        USER_PRINTF("|  uint8_t pressure[2]   offset = %2d   size = %2d\n", offsetof(audioSyncPacket, pressure[0]), sizeof(data.pressure));       // offset  6 size 2
        USER_PRINTF("|  float   sampleRaw     offset = %2d   size = %2d\n", offsetof(audioSyncPacket, sampleRaw), sizeof(data.sampleRaw));        // offset  8 size 4
        USER_PRINTF("|  float   sampleSmth    offset = %2d   size = %2d\n", offsetof(audioSyncPacket, sampleSmth), sizeof(data.sampleSmth));      // offset 12 size 4
        USER_PRINTF("|  uint8_t samplePeak    offset = %2d   size = %2d\n", offsetof(audioSyncPacket, samplePeak), sizeof(data.samplePeak));      // offset 16 size 1
        USER_PRINTF("|  uint8_t frameCounter  offset = %2d   size = %2d\n", offsetof(audioSyncPacket, frameCounter), sizeof(data.frameCounter));  // offset 17 size 1
        USER_PRINTF("|  uint8_t fftResult[16] offset = %2d   size = %2d\n", offsetof(audioSyncPacket, fftResult[0]), sizeof(data.fftResult));     // offset 18 size 16
        USER_PRINTF("|  uint16_t zeroCrossingCount offset = %2d   size = %2d\n", offsetof(audioSyncPacket, zeroCrossingCount), sizeof(data.zeroCrossingCount)); // offset 34 size 2
        USER_PRINTF("|  float   FFT_Magnitude offset = %2d   size = %2d\n", offsetof(audioSyncPacket, FFT_Magnitude), sizeof(data.FFT_Magnitude));// offset 36 size 4
        USER_PRINTF("|  float   FFT_MajorPeak offset = %2d   size = %2d\n", offsetof(audioSyncPacket, FFT_MajorPeak), sizeof(data.FFT_MajorPeak));// offset 40 size 4
        USER_PRINTLN(); USER_FLUSH();
      }
      #endif

      #if defined(ARDUINO_ARCH_ESP32) && defined(SR_DEBUG)
      DEBUGSR_PRINTF("|| %-9s min free stack %d\n", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL)); //WLEDMM
      #endif
    }

    /*
     * loop() is called continuously. Here you can check for events, read sensors, etc.
     * 
     * Tips:
     * 1. You can use "if (WLED_CONNECTED)" to check for a successful network connection.
     *    Additionally, "if (WLED_MQTT_CONNECTED)" is available to check for a connection to an MQTT broker.
     * 
     * 2. Try to avoid using the delay() function. NEVER use delays longer than 10 milliseconds.
     *    Instead, use a timer check as shown here.
     */
    void loop()
    {
      static unsigned long lastUMRun = millis();

      if (!enabled) {
        disableSoundProcessing = true;   // keep processing suspended (FFT task)
        lastUMRun = millis();            // update time keeping
        return;
      }
      // We cannot wait indefinitely before processing audio data
      if (strip.isServicing() && (millis() - lastUMRun < 2)) return;   // WLEDMM isServicing() is the critical part (be nice, but not too nice)

      // sound sync "receive or local"
      bool useNetworkAudio = false;
      if (audioSyncEnabled > AUDIOSYNC_SEND) {  // we are in "receive" or "receive+local" mode
        if (udpSyncConnected && ((millis() - last_UDPTime) <= AUDIOSYNC_IDLE_MS))
          useNetworkAudio = true;
        else
          useNetworkAudio = false;
        if (audioSyncEnabled == AUDIOSYNC_REC)
          useNetworkAudio = true;  // don't fall back to local audio in standard "receive mode"
      }

      // suspend local sound processing when "real time mode" is active (E131, UDP, ARTNET)
      if (  (realtimeOverride == REALTIME_OVERRIDE_NONE)  // please add other overrides here if needed
          &&( (realtimeMode == REALTIME_MODE_GENERIC)
            ||(realtimeMode == REALTIME_MODE_E131)
            ||(realtimeMode == REALTIME_MODE_UDP)
            ||(realtimeMode == REALTIME_MODE_ARTNET) ) )  // please add other modes here if needed
      {
        #ifdef WLED_DEBUG
        if ((disableSoundProcessing == false) && (audioSyncEnabled < AUDIOSYNC_REC)) {  // we just switched to "disabled"
          DEBUG_PRINTLN("[AR userLoop]  realtime mode active - audio processing suspended.");
          DEBUG_PRINTF( "               RealtimeMode = %d; RealtimeOverride = %d\n", int(realtimeMode), int(realtimeOverride));
        }
        #endif
        disableSoundProcessing = true;
        useNetworkAudio = false;
      } else {
        #if defined(ARDUINO_ARCH_ESP32) && defined(WLED_DEBUG)
        if ((disableSoundProcessing == true) && (audioSyncEnabled < AUDIOSYNC_REC) && audioSource->isInitialized()) {    // we just switched to "enabled"
          DEBUG_PRINTLN("[AR userLoop]  realtime mode ended - audio processing resumed.");
          DEBUG_PRINTF( "               RealtimeMode = %d; RealtimeOverride = %d\n", int(realtimeMode), int(realtimeOverride));
        }
        #endif
        if ((disableSoundProcessing == true) && (audioSyncEnabled != AUDIOSYNC_REC)) lastUMRun = millis();  // just left "realtime mode" - update timekeeping
        disableSoundProcessing = false;
      }

      if (audioSyncEnabled == AUDIOSYNC_REC) disableSoundProcessing = true;   // make sure everything is disabled IF in audio Receive mode
      if (audioSyncEnabled == AUDIOSYNC_SEND) disableSoundProcessing = false;  // keep running audio IF we're in audio Transmit mode
#ifdef ARDUINO_ARCH_ESP32
      if (!audioSource || !audioSource->isInitialized()) {                                                               // no audio source
        disableSoundProcessing = true;
        if (audioSyncEnabled > AUDIOSYNC_SEND) useNetworkAudio = true;
      }
      if ((audioSyncEnabled == AUDIOSYNC_REC_PLUS) && useNetworkAudio) disableSoundProcessing = true;   // UDP sound receiving - disable local audio

      if (disableSoundProcessing) {
        vTaskSuspend(FFT_Task);
      } else {
        vTaskResume(FFT_Task);
      }

      #ifdef SR_DEBUG
      // debug info in case that task stack usage changes
      static unsigned int minLoopStackFree = UINT32_MAX;
      unsigned int stackFree = uxTaskGetStackHighWaterMark(NULL);
      if (minLoopStackFree > stackFree) {
        minLoopStackFree = stackFree;
        DEBUGSR_PRINTF("|| %-9s min free stack %d\n", pcTaskGetTaskName(NULL), minLoopStackFree); //WLEDMM
      }
      #endif

      // Only run the sampling code IF we're not in Receive mode or realtime mode
      if ((audioSyncEnabled != AUDIOSYNC_REC) && !disableSoundProcessing && !useNetworkAudio) {
        // The legacy AGC preset check is gone with the presets: soundAgc is a
        // stored config value that is read and ignored now, and it indexes
        // nothing.

        unsigned long t_now = millis();      // remember current time

        #if defined(SR_DEBUG)
          // complain when audio userloop has been delayed for long time. Currently we need userloop running between 500 and 1500 times per second.
          // softhack007 disabled temporarily - avoid serial console spam with MANY LEDs and low FPS
          //if ((userloopDelay > /*23*/ 65) && !disableSoundProcessing && (audioSyncEnabled == AUDIOSYNC_NONE)) {
            //DEBUG_PRINTF("[AR userLoop] hiccup detected -> was inactive for last %d millis!\n", userloopDelay);
          //}
        #endif

        // The level, published for the volume-reactive effects.
        //
        // There is no filtering loop here any more. The level is measured in
        // the FFT task - where the samples are - and mapped here. The old code
        // ran getSample() and agcAvg() in a catch-up loop to compensate for
        // userloop delays; both are gone, and with them the reason the loop
        // existed.
        publishLevel();
        lastUMRun = t_now;                    // update time keeping

        // update FFTMagnitude, taking into account the level
        my_magnitude = FFT_Magnitude; // / 16.0f, 8.0f, 4.0f done in effects
        if (volumeSmth < 1 ) my_magnitude = 0.001f;  // gate closed - mute

        // get AGC sensitivity and sound pressure
        static unsigned long lastEstimate = 0;
#ifdef WLEDMM_FASTPATH
        if (millis() - lastEstimate > 7) {
#else
        if (millis() - lastEstimate > 12) {
#endif
          lastEstimate = millis();
          agcSensitivity = getSensitivity();
          if (limiterOn)
            soundPressure = soundPressure + 0.38f * (estimatePressure() - soundPressure);  // dynamics limiter on -> some smoothing
          else
            soundPressure = soundPressure + 0.95f * (estimatePressure() - soundPressure);  //  dynamics limiter on -> raw value
        }
        limitSampleDynamics();
      }  // if (!disableSoundProcessing)
#endif

      autoResetPeak();          // auto-reset sample peak after strip minShowDelay
      if (!udpSyncConnected) udpSamplePeak = false;  // reset UDP samplePeak while UDP is unconnected

      connectUDPSoundSync();  // ensure we have a connection - if needed

      // UDP Microphone Sync  - receive mode
      if ((audioSyncEnabled & AUDIOSYNC_REC) && udpSyncConnected) {

        // Check our volatile flag (set by background callback)
        bool have_new_sample = processNetworkData();

        if (have_new_sample) {
          useNetworkAudio = true;
        }

        if (useNetworkAudio) {
          if (have_new_sample) syncVolumeSmth = volumeSmth;
          else volumeSmth = syncVolumeSmth;

          limitSampleDynamics();
          limitGEQDynamics(have_new_sample);
        }
      }

      if (   (audioSyncEnabled & AUDIOSYNC_REC) // receive mode
          && udpSyncConnected          // connected
          && (receivedFormat > 0)      // we actually received something in the past
          && ((millis() - last_UDPTime) > 25000)) {   // close connection after 25sec idle
        udpSyncConnected = false;
        receivedFormat = 0;
        fftUdp.close();
        volumeSmth =0.0f;
        volumeRaw =0;
        my_magnitude = 0.1; FFT_Magnitude = 0.01; FFT_MajorPeak = 2;
        soundPressure = 1.0f;
        agcSensitivity = 64.0f;
        DEBUGSR_PRINTLN(F("AR  loop(): UDP closed due to inactivity."));
      }

      #if defined(MIC_LOGGER) || defined(MIC_SAMPLING_LOG) || defined(FFT_SAMPLING_LOG)
      static unsigned long lastMicLoggerTime = 0;
      if (millis()-lastMicLoggerTime > 20) {
        lastMicLoggerTime = millis();
        logAudio();
      }
      #endif

      // Info Page: keep max sample from last 5 seconds
#ifdef ARDUINO_ARCH_ESP32
      if ((millis() -  sampleMaxTimer) > CYCLE_SAMPLEMAX) {
        sampleMaxTimer = millis();
        maxSample5sec = (0.15 * maxSample5sec) + 0.85 * sampleAvg; // reset, and start with some smoothing
        if (!arGateOpen) maxSample5sec = 0; // gate shut
      } else {
         if (arGateOpen) maxSample5sec = fmaxf(maxSample5sec, volumeRaw); // follow maximum volume
      }
#else  // similar functionality for 8266 receive only - use VolumeSmth instead of raw sample data
      if ((millis() -  sampleMaxTimer) > CYCLE_SAMPLEMAX) {
        sampleMaxTimer = millis();
        maxSample5sec = (0.15 * maxSample5sec) + 0.85 * volumeSmth; // reset, and start with some smoothing
        if (volumeSmth < 1.0f) maxSample5sec = 0; // noise gate
        if (maxSample5sec < 0.0f) maxSample5sec = 0; // avoid negative values
      } else {
         if (volumeSmth >= 1.0f) maxSample5sec = fmaxf(maxSample5sec, volumeRaw); // follow maximum volume
      }
#endif

#ifdef ARDUINO_ARCH_ESP32
      //UDP Microphone Sync  - transmit mode
    #if defined(WLEDMM_FASTPATH)
      if ((audioSyncEnabled & AUDIOSYNC_SEND) && (haveNewFFTResult || (millis() - lastTime > 24))) {  // fastpath: send data once results are ready, or each 25ms as fallback (max sampling time is 23ms)
    #else
      if ((audioSyncEnabled & AUDIOSYNC_SEND) && (millis() - lastTime > 20)) {                        // standard: send data each 20ms
    #endif
        haveNewFFTResult = false; // reset notification
        // Only run the transmit code IF we're in Transmit mode
        transmitAudioData();
        lastTime = millis();
      }
#endif
    }

#if defined(_MoonModules_WLED_) && defined(WLEDMM_FASTPATH)
    void loop2(void) { 
      loop();
    }
#endif

    bool getUMData(um_data_t **data)
    {
      if (!data || !enabled) return false; // no pointer provided by caller or not enabled -> exit
      *data = um_data;
      return true;
    }


    #ifdef ARDUINO_ARCH_ESP32
    void onUpdateBegin(bool init) {
      #if 1 || defined(WLED_DEBUG)
      fftTime = sampleTime = filterTime = 0;
      #endif
      // gracefully suspend FFT task (if running)
      disableSoundProcessing = true;

      // ... (reset sound data lines remain the same) ...
      volumeRaw = 0; volumeSmth = 0;
      sampleAgc = 0; sampleAvg = 0;
      my_magnitude = 0; FFT_Magnitude = 0; FFT_MajorPeak = 1;
      memset(fftCalc, 0, sizeof(fftCalc));
      memset(fftAvg, 0, sizeof(fftAvg));
      // Both halves of the double buffer, and the published pointer put back
      // to the first one - fftResult is a POINTER now, so sizeof(fftResult)
      // would be the size of the pointer and not the size of the array.
      // The publish lock is held, because the FFT task may be publishing.
      {
        ArPublishGuard guard;
        memset(fftResultBuf, 0, sizeof(fftResultBuf));
        fftResultIdx = 0;
        fftResult = fftResultBuf[0];
        // A visible placeholder, so the display shows something before the
        // first frame arrives. It goes into the PUBLISHED buffer, so it has
        // to happen under the publish lock.
        for (int i = (init ? 0 : 1); i < NUM_GEQ_CHANNELS; i += 2) fftResult[i] = 16;
        if (arUmData != nullptr) arUmData->u_data[2] = fftResult;
        // The RTA set, the same way: both halves of the double buffer
        // cleared, the published pointer back to the first one.
        memset(rtaResultBuf, 0, sizeof(rtaResultBuf));
        rtaResultIdx = 0;
        rtaResult = rtaResultBuf[0];
        memset(arRtaBands, 0, sizeof(arRtaBands));
        arRtaBandsIdx = 0;
        arRtaBandsValid = false;
        memset(arRtaSmth, 0, sizeof(arRtaSmth));
        // The display's auto-range is a peak hold, so it starts at the bottom
        // of its travel: the display starts low and the signal earns the top.
        arAgcDb = 0.0f;
      }
      inputLevel = 128;
      autoResetPeak();

      if (init && FFT_Task) {
        delay(25);                 // WLEDMM: give some time for I2S driver to finish sampling
        vTaskSuspend(FFT_Task);    // update is about to begin, disable task to prevent crash
        if (udpSyncConnected) {    // close UDP sync connection (if open)
          udpSyncConnected = false;
          fftUdp.close();          // <--- FIXED: changed from stop() to close()
          DEBUGSR_PRINTLN(F("AR onUpdateBegin(true): UDP connection closed."));
          receivedFormat = 0;
        }
      } else {
        // ... rest of function remains same ...
        if (FFT_Task) {
          vTaskResume(FFT_Task);
          connected(); // resume UDP
        } else {
          // ... task creation logic ...
          if (audioSource)
            xTaskCreateUniversal(
              #if defined(CONFIG_SOC_CPU_CORES_NUM) && CONFIG_SOC_CPU_CORES_NUM > 1
              FFTcode, "AudioTask", FFT_TASK_STACK, NULL, FFTTASK_PRIORITY, &FFT_Task, 1
              #else 
              FFTcode, "AudioTask", 8192, NULL, FFTTASK_PRIORITY, & FFT_Task, -1
              #endif
            );
        }
      }
      if (enabled && audioSource) disableSoundProcessing = false;
      updateIsRunning = init;
    }
    #endif

    /**
     * handleButton() can be used to override default button behaviour. Returning true
     * will prevent button working in a default way.
     */
    bool handleButton(uint8_t b) {
      yield();
      // crude way of determining if audio input is analog
      // better would be for AudioSource to implement getType()
      if (enabled
          && dmType == 0 && audioPin>=0
          && (buttonType[b] == BTN_TYPE_ANALOG || buttonType[b] == BTN_TYPE_ANALOG_INVERTED)
         ) {
        return true;
      }
      return false;
    }

    ////////////////////////////
    // Settings and Info Page //
    ////////////////////////////

    /*
     * addToJsonAr() - the /json/ar endpoint.
     *
     * The info page is for a person: a dozen numbers, read at a glance, each
     * one a short piece of prose. This is the machine-readable twin of it -
     * EVERY number the analysis knows, as a NUMBER, with the unit in the key's
     * name. There are no strings anywhere in here except the few names that
     * have no numeric form (the codec, the window's mode), because a script
     * that has to parse "-23.1 dB" out of a sentence is doing this the hard
     * way.
     *
     * The layout is nested by what the number IS rather than by what it is
     * for: the levels, the display window, the two band sets, the filter bank,
     * the timings, the self-tests, the calibration, the codec and the memory.
     * Everything the info page shows is here too, plus everything it does not
     * have room for.
     */
    void addToJsonAr(JsonObject& obj) override
    {
      JsonObject ar = obj.createNestedObject(FPSTR(_name));
      const int rtaBands = bandLayoutReady ? lay31.count : 0;

      // ONE frame's worth of the analysis numbers.
      //
      // The analysis task rewrites its arrays every 10.67 ms and this runs
      // on the main loop, so reading them in place would give a set of band
      // levels that never existed - half of one frame and half of the next.
      // The published snapshot is the frame's own copy, taken under the same
      // lock, so what is published here is a coherent frame.
      ArFrameSnapshot snap;
      {
        ArPublishGuard guard;
        snap = arFrameSnap;
      }

      // ---- what is running ---------------------------------------------
      ar["enabled"]   = enabled;
      ar["processing"] = (audioSource != nullptr) && !disableSoundProcessing;
      // WHICH source is actually feeding the analysis - not what is set.
      //
      // In "receive + local" mode (REC_PLUS) the local input is used
      // whenever no UDP packets are arriving, so publishing "udp" there
      // would be a lie: the board would be measuring its own microphone while
      // the label said the network. "connected" only means the multicast
      // group is joined - the packets can be dead while it says true.
      //
      // This mirrors the decision in the userloop, so the label and the
      // measurement cannot disagree.
      {
        const bool udpLive  = udpSyncConnected && (last_UDPTime != 0) &&
                              ((millis() - last_UDPTime) <= AUDIOSYNC_IDLE_MS);
        const bool localOk  = (audioSource && audioSource->isInitialized());
        const bool strictRec = (audioSyncEnabled == AUDIOSYNC_REC);
        if (strictRec) {
          // The local input is switched off in this mode, so "udp" is the
          // only answer - even when the feed is dead and the board is
          // measuring silence.
          ar["source"] = "udp";
        } else if ((audioSyncEnabled & AUDIOSYNC_REC) && udpLive) {
          ar["source"] = "udp";
        } else if (localOk) {
          ar["source"] = (dmType == 51) ? "legacy i2s pdm" : "i2s";
        } else {
          ar["source"] = "none";
        }
      }
      ar["codec"]     = arCodecName(dmType);
      ar["input_level"] = (int)inputLevel;
      {
        float post = (float)inputLevel / 128.0f;
        if (post < 1.0f) post = ((post - 1.0f) * 0.8f) + 1.0f;
        ar["input_gain_pct"] = (double)roundf(post * 100.0f);
      }
      ar["peak_pct"]  = (double)(100.0f * maxSample5sec / 255.0f);

      // ---- the level / floor / gate / AGC chain -------------------------
      //
      // Four DIFFERENT quantities. They are separate here because they are
      // separate in the firmware: the level is the fresh hop's RMS, the floor
      // is a minimum statistic, the gate is a comparison between them, and the
      // AGC is a display offset applied last.
      {
        JsonObject l = ar.createNestedObject("level");
        l["level_db"]       = (double)arLevelDb;
        l["noise_floor_db"] = (double)arFloorDb;
        l["above_floor_db"] = (double)(arLevelDb - arFloorDb);
        l["gate_open"]      = arGateOpen;
        l["gate_open_db"]   = (double)soundSquelch;
        l["range_db"]       = (double)arAgcDb;
        // There is ONE offset for the whole spectrum now: the sixteen display
        // bands are sixteen of the thirty-one RTA bands, so both views are
        // drawn in the same window at the same scale. The key is kept because
        // a script may be reading it, and it is the same number.
        l["rta_range_db"]   = (double)arAgcDb;
      }

      // ---- the transform ------------------------------------------------
      //
      // The proof of concept's switch, and the live A/B of the two things the
      // transform owned that the banks had to take over: the dominant
      // frequency and the peak magnitude.
      {
        JsonObject t = ar.createNestedObject("transform");
        // "on" is what is RUNNING, which is not always what was asked for: a
        // bank that failed its self-test forces the transform back on. The
        // request is published next to it, and "forced" says the fallback
        // is what is running it.
        {
          const bool tfNow = arTransformRuns();
          t["on"]        = tfNow;
          t["requested"] = arUseTransform;
          t["forced"]    = (tfNow && !arUseTransform);
        }
        t["valid"]  = arTransformValid;
        // The transform's own answers - what the effects are using.
        t["peak_hz"]    = (double)FFT_MajorPeak;
        t["magnitude"]  = (double)FFT_Magnitude;
        // The bank's answers - computed every frame, so the two are two
        // measurements of the same audio in the same frame.
        t["bank_peak_hz"]   = (double)arBankPeakHz;
        t["bank_magnitude"] = (double)arBankMagnitude;
        // Which RTA band the bank's answer is in, and whether the two agree
        // on the octave - which is all the effects need of it.
        {
          int band = -1;
          if (bandLayoutReady && arBankPeakHz > 0.0f) {
            band = (int)floorf((log10f(arBankPeakHz) - 1.3f) * 10.0f + 0.5f);
            if (band < 0) band = 0;
            if (band > lay31.count - 1) band = lay31.count - 1;
          }
          t["bank_band"] = band;
          t["bank_centre_hz"] = (band >= 0) ? (double)lay31.centre[band] : 0.0;
          // The transform's peak, expressed as the band it falls in.
          int tfBand = -1;
          if (bandLayoutReady && FFT_MajorPeak > 0.0f) {
            tfBand = (int)floorf((log10f(FFT_MajorPeak) - 1.3f) * 10.0f + 0.5f);
            if (tfBand < 0) tfBand = 0;
            if (tfBand > lay31.count - 1) tfBand = lay31.count - 1;
          }
          t["tf_band"] = tfBand;
          t["same_band"] = (band >= 0 && tfBand >= 0) ? (band == tfBand) : false;
        }
      }

      // ---- the display window -------------------------------------------
      {
        JsonObject d = ar.createNestedObject("display");
        d["trim_db"]    = (double)TROYHACKS_DBREF;
        d["span_db"]    = (double)arWindowRangeDb;
        d["input_trim_db"] = (double)arInputTrimDb();
        // The smoothing on the 16 bars: the user's own rise and fall, on both
        // paths, so the switch between them compares the measurement and not
        // the display.
        d["rise_ms"]    = (double)(attackTime  > 0 ? attackTime  : AR_DISP_ATTACK_MS);
        d["fall_ms"]    = (double)(decayTime   > 0 ? decayTime   : AR_DISP_RELEASE_MS);
        // The window is ONE window, for both views: the same mode, the same
        // span, the same fixed grid.
        d["window"]      = arAutoRange ? "auto" : "fixed";
        d["window_floor_db"] = (double)arWindowFloorDb;
        d["window_top_db"]   = (double)arWindowTopDb;
        d["window_span_db"]  = (double)(arAutoRange ? arWindowRangeDb : (arWindowTopDb - arWindowFloorDb));
        // The absolute window the display is currently drawn in. In auto mode
        // this is implied by the auto-range offset, and in fixed mode it is
        // the user's own grid. There is ONE window and ONE offset: the sixteen
        // display bands are sixteen of the thirty-one RTA bands, so the two
        // views are the same picture at two resolutions. The RTA's is the
        // number to compare with an analyser's own scale.
        d["geq_window_lo_db"] = (double)(arAutoRange ? (AR_WINDOW_TARGET_DB - arAgcDb - arWindowRangeDb)
                                                      : arWindowFloorDb);
        d["geq_window_hi_db"] = (double)(arAutoRange ? (AR_WINDOW_TARGET_DB - arAgcDb) : arWindowTopDb);
        d["rta_window_lo_db"] = (double)(arAutoRange ? (AR_WINDOW_TARGET_DB - arAgcDb - arWindowRangeDb)
                                                      : arWindowFloorDb);
        d["rta_window_hi_db"] = (double)(arAutoRange ? (AR_WINDOW_TARGET_DB - arAgcDb) : arWindowTopDb);
        d["rta_ref_db"] = (double)rtaRefDb;
        arJsonFloats(d.createNestedArray("rta_trim_db"), rtaTrimDb, rtaBands);
        // The sixteen trims that belong to the sixteen bands that are shown -
        // the RTA's own trim, read at the sixteen bands the display draws.
        JsonArray gt = d.createNestedArray("geq_trim_db");
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) gt.add((double)rtaTrimDb[arGeqFromRta[k]]);
      }

      // ---- the 16 display bands ----------------------------------------
      //
      // These are sixteen of the thirty-one RTA bands - the nearest
      // third-octave centre to each of the old half-octave ones - so the
      // values here ARE the RTA's values, and the rta_band array says which
      // RTA band each one is.
      {
        JsonObject g = ar.createNestedObject("geq");
        g["count"]     = NUM_GEQ_CHANNELS;
        g["layout"]    = "1/3-octave (subset of the 31)";
        g["f_low_hz"]  = (double)lay31.edgeLo[arGeqFromRta[0]];
        g["f_high_hz"] = (double)lay31.edgeHi[arGeqFromRta[NUM_GEQ_CHANNELS - 1]];
        JsonArray gc = g.createNestedArray("centres_hz");
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) gc.add((double)arGeqCentre(k));
        // Which RTA band each display band is. It is the link between the two
        // views: bar 3 here is bar 8 there, same number, same height.
        JsonArray gb = g.createNestedArray("rta_band");
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) gb.add((int)arGeqFromRta[k]);
        // One copy of the published bytes, taken under the publish lock so a
        // swap cannot straddle it.
        uint8_t bars[NUM_GEQ_CHANNELS];
        {
          ArPublishGuard guard;
          memcpy(bars, fftResult, sizeof(bars));
        }
        JsonArray ba = g.createNestedArray("bars");
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) ba.add((int)bars[k]);
        // The band power RAW - before the calibration divides the input
        // chain's response out of it and before the reference offset and the
        // trim go on. The calibrated number is the band_db array below, and
        // that is what the bars show.
        //
        // It is the RTA band's own raw power, so it comes out of the same
        // published snapshot as the band levels below and it is the same
        // frame's reading.
        JsonArray raw = g.createNestedArray("raw_db");
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) raw.add((double)snap.rawDb[k]);
        JsonArray bd = g.createNestedArray("band_db");
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) bd.add((double)snap.bandDb[k]);
        float bandMax = -1e30f;
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++)
          if (snap.bandDb[k] > bandMax) bandMax = snap.bandDb[k];
        JsonArray sh = g.createNestedArray("shape_db");
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
          const float db = (bandMax > -1e29f && snap.bandDb[k] > -119.0f)
                           ? (snap.bandDb[k] - bandMax) : -99.0f;
          sh.add((double)db);
        }
        // The unit, said out loud: the 16 bars are integrated band powers, in
        // the RTA's unit, and NOT the point densities the old sixteen bands
        // were. This is the number to read to know what a bar means.
        g["unit"] = "integrated band power (dB), 16 of the 31 ISO third-octave bands";
      }

      // ---- the 31 RTA bands ---------------------------------------------
      //
      // These are the numbers to compare with a hardware analyser, one bar
      // for one bar. They are PRE-AGC: an AGC offset is a display artefact
      // and would destroy the comparison.
      {
        JsonObject r = ar.createNestedObject("rta");
        r["count"]     = rtaBands;
        r["layout"]    = "1/3-octave (ISO 266)";
        r["f_low_hz"]  = (double)lay31.fLow;
        r["f_high_hz"] = (double)lay31.fHigh;
        JsonArray c = r.createNestedArray("centres_hz");
        for (int k = 0; k < rtaBands; k++) c.add((double)lay31.centre[k]);
        uint8_t bars[NUM_RTA_CHANNELS];
        {
          ArPublishGuard guard;
          memcpy(bars, rtaResult, sizeof(bars));
        }
        JsonArray ba = r.createNestedArray("bars");
        for (int k = 0; k < rtaBands; k++) ba.add((int)bars[k]);
        JsonArray bp = r.createNestedArray("band_power_db");
        for (int k = 0; k < rtaBands; k++) bp.add((double)snap.rtaPowDb[k]);
        JsonArray bl = r.createNestedArray("band_level_db");
        for (int k = 0; k < rtaBands; k++) bl.add((double)snap.rtaLevelDb[k]);
        JsonArray tr = r.createNestedArray("trim_db");
        arJsonFloats(tr, rtaTrimDb, rtaBands);
      }

      // ---- the filter bank (the bottom 8 RTA bands) ----------------------
      {
        JsonObject b = ar.createNestedObject("bank");
        b["ready"]      = bankReady;
        b["bands"]      = AR_BANK_BANDS;
        b["f_low_hz"]   = bandLayoutReady ? (double)lay31.centre[0] : 0.0;
        b["f_high_hz"]  = bandLayoutReady ? (double)lay31.centre[AR_BANK_BANDS - 1] : 0.0;
        b["rate_hz"]    = (double)AR_BANK_FS;
        b["decimate"]   = AR_BANK_DEC;
        b["aa_hz"]      = (double)AR_BANK_AA_HZ;
        b["tau_ms"]     = (double)bankTauMs;
        b["us_per_frame"] = (double)(bankTime * TIMING_UNIT_US);
        // The filter's ENBW against the nominal band width: how much wider
        // than the band the filter really is.
        JsonArray e = b.createNestedArray("enbw_ratio_db");
        for (int k = 0; k < AR_BANK_BANDS; k++) {
          const float width = lay31.edgeHi[k] - lay31.edgeLo[k];
          e.add((double)(10.0f * log10f(bankEnbw[k] / width)));
        }
        JsonObject st = b.createNestedObject("self_test");
        st["done"]         = bankTestDone;
        st["pass"]         = bankTestDone && (bankTestFails == 0) && (bankTestNbrFails == 0);
        st["fails"]        = bankTestFails;
        st["worst_db"]     = (double)bankTestWorst;
        st["worst_hz"]     = (double)bankTestWorstF;
        st["nbr_checks"]   = bankTestNbrChecks;
        st["nbr_fails"]    = bankTestNbrFails;
        st["worst_nbr_db"] = (double)bankTestWorstNbr;
      }

      // ---- the head-to-head: the transform against the banks --------------
      //
      // Both were asked the same question about the same tone, with the same
      // truth, at boot: "what is the power in RTA band k?". The transform's
      // kernel is a fixed 23.49 Hz wide whatever the band is, so every band
      // narrower than that is a band the transform cannot measure - and the
      // banks' filters are built to the band they measure.
      //
      // "predicted" is what the kernel's width says the transform's error
      // should be: it reads the point PSD at the band centre and multiplies it
      // by the band's width, so a band narrower than the kernel reads low by
      // exactly the ratio.
      {
        JsonObject x = ar.createNestedObject("transform_vs_bank");
        x["done"]            = xfTestDone;
        x["bands"]           = xfTestBands;
        x["kernel_hz"]       = (double)xfKernelEnbw();
        x["tf_fails"]        = xfTestTfFails;
        x["tf_tol_db"]       = 1.00;
        x["tf_worst_db"]     = (double)xfTestTfWorst;
        x["tf_worst_band"]   = xfTestTfWorstBand;
        x["bank_fails"]      = xfTestBankFails;
        x["bank_tol_db"]     = 0.35;
        x["bank_worst_db"]   = (double)xfTestBankWorst;
        x["bank_worst_band"] = xfTestBankWorstBand;
        JsonArray ms = x.createNestedArray("measured");
        JsonArray tf = x.createNestedArray("transform_err_db");
        JsonArray bk = x.createNestedArray("bank_err_db");
        JsonArray pr = x.createNestedArray("predicted_err_db");
        // The band's nominal width, and whether the transform could take the
        // integral from the bins (wide) or had to multiply the point PSD at
        // the centre by the width (narrow).
        //
        // The narrow path is where the kernel's width shows: the point PSD of
        // a tone is its power spread over the kernel, so multiplying it by a
        // width NARROWER than the kernel reads low by exactly the ratio. That
        // is what "predicted" is - and it only applies to the narrow path.
        JsonArray wd = x.createNestedArray("width_hz");
        JsonArray nw = x.createNestedArray("narrow");
        for (int k = 0; k < NUM_RTA_CHANNELS; k++) {
          ms.add(xfTestHas[k]);
          tf.add((double)(xfTestHas[k] ? xfTestTfDb[k] : 0.0));
          bk.add((double)(xfTestHas[k] ? xfTestBankDb[k] : 0.0));
          pr.add((double)(xfTestHas[k] ? xfTestPredDb[k] : 0.0));
          const float width = lay31.edgeHi[k] - lay31.edgeLo[k];
          wd.add((double)width);
          nw.add(width < 3.0f * binWidth);
        }
      }

      // ---- the transform's own reading of the old half-octave layout ------
      //
      // This is the transform's INSTRUMENT, not the display's measurement.
      // The sixteen bars on the screen are sixteen of the thirty-one RTA bands
      // and their values are published in the "geq" object above; what is here
      // is the transform's own reading of lay16 - the old half-octave layout,
      // in the old point-PSD unit - kept because it is what the
      // proof-of-concept path is checked against.
      //
      // The name is "transform_bands" and NOT "transform" because "transform"
      // is taken: it is the proof-of-concept switch and the A/B of the dominant
      // frequency, and it is published further up. Two nested objects with one
      // name do not merge - the second replaces the first - and the switch's
      // state ("on", "requested", "forced", "peak_hz", "same_band") would have
      // vanished from the endpoint without a word of warning.
      {
        JsonObject b = ar.createNestedObject("transform_bands");
        b["bands"]        = AR_DISP_BANDS;
        b["rate_hz"]      = (double)SAMPLE_RATE;
        b["layout"]       = "1/2-octave (the transform's own)";
        b["unit"]         = "point PSD (dB re units^2/Hz)";
        // The transform's answer for the same bands: its window is 4096
        // samples long, so its content cannot be newer than 42.7 ms.
        b["fft_resp_ms"]  = 42.7;
        JsonArray c = b.createNestedArray("centres_hz");
        for (int k = 0; k < AR_DISP_BANDS; k++) c.add((double)lay16.centre[k]);
        // The width the layout asks for.
        JsonArray w = b.createNestedArray("width_hz");
        for (int k = 0; k < AR_DISP_BANDS; k++)
          w.add((double)(lay16.edgeHi[k] - lay16.edgeLo[k]));
        // The transform's reading, and the same without the crest compensation.
        // The difference between these two IS the compensation.
        JsonArray td = b.createNestedArray("transform_db");
        for (int k = 0; k < AR_DISP_BANDS; k++) td.add((double)snap.dispTransformDb[k]);
        JsonArray traw = b.createNestedArray("transform_raw_db");
        for (int k = 0; k < AR_DISP_BANDS; k++) traw.add((double)snap.dispTransformRawDb[k]);
        // The transform's band-AVERAGE reading: the same transform, the same
        // frame, the same unit - but the mean over the band rather than the
        // peak inside it.
        JsonArray tmean = b.createNestedArray("transform_mean_db");
        for (int k = 0; k < AR_DISP_BANDS; k++) tmean.add((double)snap.dispTransformMeanDb[k]);
        // The crest compensation itself, dB.
        JsonArray ecm = b.createNestedArray("est_comp_db");
        for (int k = 0; k < AR_DISP_BANDS; k++) ecm.add((double)estComp[k]);
      }

      // ---- the RTA's top bank (bands 8..30, by their own filters) ---------
      //
      // "active" says whether these bands are coming from the bank or from the
      // transform. Both readings are published either way, so the two can be
      // compared on the same audio without switching.
      //
      // The same question estFinalise() asks, and the same answer - the
      // frame's answer, carried with the frame's numbers.
      {
        JsonObject b = ar.createNestedObject("rtatopbank");
        b["ready"]        = rtaTopReady;
        b["healthy"]      = arRtaTopHealthy;
        b["active"]       = snap.rtaTopBankDrives;
        b["bands"]        = AR_RTA_TOP_BANDS;
        b["first_band"]   = AR_RTA_TOP_FIRST;
        b["rate_hz"]      = (double)SAMPLE_RATE;
        b["tau_ms"]       = (double)rtaTauMs;
        b["us_per_frame"] = (double)(rtaTime * TIMING_UNIT_US);
        // The transform's window is 4096 samples long, so its content cannot
        // be newer than 42.7 ms.
        b["fft_resp_ms"]  = 42.7;
        JsonArray c = b.createNestedArray("centres_hz");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++)
          c.add((double)lay31.centre[AR_RTA_TOP_FIRST + k]);
        // The width the layout asks for, and the width the filter really has.
        JsonArray w = b.createNestedArray("width_hz");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++)
          w.add((double)(lay31.edgeHi[AR_RTA_TOP_FIRST + k] - lay31.edgeLo[AR_RTA_TOP_FIRST + k]));
        JsonArray wm = b.createNestedArray("width_measured_hz");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) wm.add((double)rtaWidthHz[k]);
        JsonArray e = b.createNestedArray("enbw_hz");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) e.add((double)rtaEnbw[k]);
        // The filter's ENBW against the nominal band width: the bottom eight
        // bands are set to 0.85, and so are these.
        JsonArray er = b.createNestedArray("enbw_ratio_db");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
          const float width = lay31.edgeHi[AR_RTA_TOP_FIRST + k] - lay31.edgeLo[AR_RTA_TOP_FIRST + k];
          er.add((double)(10.0f * log10f(rtaEnbw[k] / width)));
        }
        // How old the content on each bar is.
        JsonArray r = b.createNestedArray("resp_ms");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) r.add((double)rtaRespMs[k]);
        // The bank's reading and the transform's, side by side, in dB of power.
        JsonArray bd = b.createNestedArray("band_power_db");
        JsonArray td = b.createNestedArray("transform_power_db");
        JsonArray df = b.createNestedArray("delta_db");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) {
          // Both sides come out of the published snapshot, so the delta is
          // the difference between ONE frame's bank and ONE frame's
          // transform - not between a bank from now and a transform from
          // ten milliseconds ago.
          const float bv = snap.rtaTopPowLin[k];
          const float tv = snap.rtaTopTfLin[k];
          bd.add((double)snap.rtaTopPowDb[k]);
          td.add((double)snap.rtaTopTfDb[k]);
          // No delta when either side has no reading. Zero would read as
          // "the two agree exactly", which is a claim the transform cannot
          // make while it is switched off: -99 is this file's "no value".
          df.add((double)((bv > 0.0f && tv > 0.0f) ? 10.0f * log10f(bv / tv) : -99.0));
        }
        // The bank's self-test.
        JsonObject dst = b.createNestedObject("self_test");
        dst["done"]             = rtaTestDone;
        dst["pass"]             = rtaTestDone && (rtaTestFails == 0);
        dst["fails"]            = rtaTestFails;
        dst["tone_worst_db"]    = (double)rtaTestWorst;
        dst["tone_worst_hz"]    = (double)rtaTestWorstF;
        dst["white_worst_db"]   = (double)rtaTestWhiteWorst;
        dst["white_worst_hz"]   = (double)rtaTestWhiteWorstF;
        dst["enbw_worst_db"]    = (double)rtaTestEnbwWorst;
        dst["enbw_worst_hz"]    = (double)rtaTestEnbwWorstF;
        dst["neighbour_worst_db"] = (double)rtaTestNbrWorst;
        dst["time_ms"]          = (double)rtaTestTimeMs;
        JsonArray stdb = dst.createNestedArray("tone_db");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) stdb.add((double)rtaTestToneDb[k]);
        JsonArray swdb = dst.createNestedArray("white_db");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) swdb.add((double)rtaTestWhiteDb[k]);
        JsonArray sedb = dst.createNestedArray("enbw_db");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) sedb.add((double)rtaTestEnbwDb[k]);
        JsonArray sndb = dst.createNestedArray("neighbour_db");
        for (int k = 0; k < AR_RTA_TOP_BANDS; k++) sndb.add((double)rtaTestNbrDb[k]);
      }

      // ---- the frame's timing -------------------------------------------
      //
      // Microseconds, everywhere. The firmware stores these in 10 us units,
      // so the conversion happens here and nowhere else.
      {
        JsonObject t = ar.createNestedObject("timing_us");
        t["cycle"]         = (double)(fftTaskCycle * TIMING_UNIT_US);
        t["fastest_cycle"] = (double)(fftTaskMinCycle * TIMING_UNIT_US);
        t["slowest_cycle"] = (double)(fftTaskMaxCycle * TIMING_UNIT_US);
        t["sampling"]      = (double)(sampleTime * TIMING_UNIT_US);
        t["filtering"]     = (double)(filterTime * TIMING_UNIT_US);
        t["fft"]           = (double)(fftTime * TIMING_UNIT_US);
        t["bank"]          = (double)(bankTime * TIMING_UNIT_US);
        t["rtatopbank"]    = (double)(rtaTime * TIMING_UNIT_US);
        t["dma_surplus_ms"] = (double)(FRAME_PERIOD_MS - (sampleTime * TIMING_UNIT_US / 1000.0));
        t["frames_total"]   = (unsigned long)arFramesTotal;
        t["frames_overran"] = (unsigned long)arFramesOverran;
        // The frame's work: the compute without the wait for the audio. The
        // read's time is NOT in here - on a correctly paced board it is the
        // task waiting for the next hop to arrive, and it is the largest
        // single number in the cycle. The work is what has to fit inside the
        // frame period, and it is what frames_overran counts against.
        t["work_ms"] = (double)((fftTime + filterTime) * TIMING_UNIT_US / 1000.0);
        JsonObject p = t.createNestedObject("parts");
        p["read_carry"]    = (double)(sampleTime * TIMING_UNIT_US);
        p["filtering"]     = (double)(filterTime * TIMING_UNIT_US);
        p["fft_region"]    = (double)(fftTime * TIMING_UNIT_US);
        p["tail"]          = (double)(tTail * TIMING_UNIT_US);
        p["tick_floor"]    = (double)(tFloor * TIMING_UNIT_US);
        p["deadline_wait"] = (double)(tWait * TIMING_UNIT_US);
        p["loop_entry"]    = (double)(tLoop * TIMING_UNIT_US);
        p["total"]         = (double)(tParts * TIMING_UNIT_US);
        p["residual"]      = (double)(tResidual * TIMING_UNIT_US);
        // "fft" above is the whole frame from windowing through the band
        // mapping; the transform is only one of its five stages.
        JsonObject fb = t.createNestedObject("fft_breakdown");
        fb["windowing"]   = (double)(tWindow * TIMING_UNIT_US);
        fb["frame_prep"]  = (double)(tPrep * TIMING_UNIT_US);
        fb["esp_dsp_fft"] = (double)(tFft * TIMING_UNIT_US);
        fb["bitrev_mag"]  = (double)(tPost * TIMING_UNIT_US);
        fb["bands_post"]  = (double)(tFinal * TIMING_UNIT_US);
        // The two halves of bitrev_mag: the permutation on its own, and the
        // estimator's reads of the spectrum. What is left of bitrev_mag is
        // the magnitude fold, the peak scan and the pink factor.
        fb["bitrev"]      = (double)(tBitrev * TIMING_UNIT_US);
        fb["estimator"]   = (double)(tEst * TIMING_UNIT_US);
        fb["magnitude"]   = (double)((tPost - tEst) * TIMING_UNIT_US);
        // The pipeline's health, as words. "ok" is what you want to see in
        // all five; anything else says which of the three limits was crossed.
        {
          const ArHealth h = arHealth();
          JsonObject hh = t.createNestedObject("health");
          hh["budget"]        = h.budget;
          hh["pacing"]        = h.pacing;
          hh["fastest_cycle"] = h.fastest;
          hh["dma_surplus"]   = h.surplus;
          hh["read_blocking"] = h.readBlocking;
        }
      }

      // ---- what the init cost -------------------------------------------
      {
        JsonObject i = ar.createNestedObject("init_ms");
        i["buffers"]             = (double)arInitBuffersMs;
        i["calibration"]         = (double)arInitCalMs;
        i["chain_self_test"]     = (double)arInitChainMs;
        i["estimator_self_test"] = (double)arInitEstMs;
        i["total"]               = (double)arInitTotalMs;
        // false: the boot-time self-tests were skipped. The self-test blocks
        // below then say "not run", which is what they mean.
        i["boot_self_tests"]     = arBootSelfTests;
      }

      // ---- the self-tests ------------------------------------------------
      {
        JsonObject s = ar.createNestedObject("self_test");
        JsonObject c = s.createNestedObject("chain");
        c["valid"]        = selfTestResult.valid;
        c["probes"]       = selfTestResult.probes;
        c["fails"]        = selfTestResult.fails;
        c["worst_impl_db"] = (double)selfTestResult.worstImpl;
        c["worst_impl_hz"] = (double)selfTestResult.worstImplF;
        c["worst_sine_db"] = (double)selfTestResult.worstSine;
        c["worst_sine_hz"] = (double)selfTestResult.worstSineF;
        c["worst_lpf_db"]  = (double)selfTestResult.worstLpf;
        c["worst_lpf_hz"]  = (double)selfTestResult.worstLpfF;
        JsonArray pt = c.createNestedArray("probes_table");
        for (int i = 0; i < selfTestResult.probes; i++) {
          JsonObject row = pt.createNestedObject();
          row["hz"]          = (double)selfTestResult.freq[i];
          row["model_db"]    = (double)selfTestResult.modelDb[i];
          row["impl_db"]     = (double)selfTestResult.implDb[i];
          row["sine_db"]     = (double)selfTestResult.sineDb[i];
          row["lpf_model_db"] = (double)selfTestResult.lpfModelDb[i];
          row["lpf_impl_db"]  = (double)selfTestResult.lpfImplDb[i];
          row["ok"]          = selfTestResult.ok[i];
        }
        JsonObject e = s.createNestedObject("estimator");
        e["done"]         = estTestDone;
        e["skipped"]      = estTestSkipped;
        e["fails"]        = estTestFails;
        e["checks"]       = NUM_GEQ_CHANNELS + rtaBands;
        e["worst_db"]     = (double)estTestWorst;
        e["worst_hz"]     = (double)estTestWorstF;
        e["nbr_checks"]   = estTestNbrChecks;
        e["nbr_fails"]    = estTestNbrFails;
        e["worst_nbr_db"] = (double)estTestWorstNbr;
        e["unresolvable"] = estTestUnresolvable;
      }

      // ---- the calibration ------------------------------------------------
      {
        JsonObject c = ar.createNestedObject("calibration");
        c["target"]   = calTargetPink ? "pink" : "flat";
        c["clamps"]   = calClamps;
        c["clamp_db"] = (double)CAL_CLAMP_DB;
        const int n = NUM_GEQ_CHANNELS + rtaBands;
        float lo = 0.0f, hi = 0.0f;
        for (int k = 0; k < n; k++) {
          if (k == 0 || calChainDb[k] < lo) lo = calChainDb[k];
          if (k == 0 || calChainDb[k] > hi) hi = calChainDb[k];
        }
        c["chain_db_min"] = (double)lo;
        c["chain_db_max"] = (double)hi;
        arJsonFloats(c.createNestedArray("chain_db"), calChainDb, n);
      }

      // ---- the codec ------------------------------------------------------
      {
        JsonObject c = ar.createNestedObject("codec");
        const AR_CodecRegs& cregs = arCodecRegs();
        c["registers_valid"] = cregs.valid;
        arJsonBytes(c.createNestedArray("registers"), cregs.v, 32);
        c["want_mclk_hz"]       = (double)ES8311_MCLK_HZ;
        c["want_sample_rate_hz"] = (double)SAMPLE_RATE;
        uint32_t gotMclk = 0, gotFs = 0;
        if (audioSource) {
          gotMclk = audioSource->getMclkHz();
          gotFs   = audioSource->getClockSampleRateHz();
        }
        c["mclk_hz"]          = (double)gotMclk;
        c["sample_rate_hz"]   = (double)gotFs;
        c["ppm"] = gotFs ? (double)((((long long)gotFs - (long long)SAMPLE_RATE) * 1000000LL)
                                    / (long long)SAMPLE_RATE) : 0.0;
      }

      // ---- the memory -----------------------------------------------------
#ifdef ARDUINO_ARCH_ESP32
      {
        JsonObject m = ar.createNestedObject("memory");
        m["fft_buf_bytes"]      = (unsigned)(sizeof(float) * samplesFFT * 2);
        m["fft_buf_internal"]   = (fftBuf != nullptr && arFftBufInternal);
        m["twiddle_bytes"]      = (unsigned)(sizeof(float) * 4 * (samplesFFT >> 1));
        m["twiddle_internal"]   = (fftTwiddle != nullptr && arTwiddleInternal);
        m["internal_free_bytes"] = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        // The web server drops a client without answering it when the free heap
        // is under 8 KB or the largest single free block is under 2 KB, so the
        // block size matters as much as the total.
        m["internal_largest_block"] = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
      }
#endif

      // ---- the UDP sound sync ---------------------------------------------
      {
        JsonObject u = ar.createNestedObject("udp_sync");
        const char* mode = "off";
        if (audioSyncEnabled & AUDIOSYNC_SEND)                mode = "send";
        else if (audioSyncEnabled == AUDIOSYNC_REC)           mode = "receive";
        else if (audioSyncEnabled == AUDIOSYNC_REC_PLUS)      mode = "receive_plus";
        u["enabled"]   = (audioSyncEnabled != 0);
        u["mode"]      = mode;
        u["connected"] = udpSyncConnected;
        u["format"]    = receivedFormat;
        u["last_packet_ms_ago"] = last_UDPTime ? (long)(millis() - last_UDPTime) : -1L;
        // The transmit counters. "connected" only means the group is joined;
        // these say whether anything is actually going out.
        u["tx_calls"]   = (unsigned long)txCalls;
        u["tx_ok"]      = (unsigned long)(txOkIf + txOkPlain);
        u["tx_ok_if"]   = (unsigned long)txOkIf;    // sent naming the adapter
        u["tx_ok_plain"]= (unsigned long)txOkPlain; // sent with a route lookup
        u["tx_fail"]    = (unsigned long)txFail;
        u["tx_bytes"]   = (unsigned long)txBytes;
        u["tx_err"]     = txLastErr;                // the stack's code, 0 = none
        u["tx_if"]      = txIf;                     // the adapter named (-1 = none)
        u["tx_ms_ago"]  = txLastMs ? (long)(millis() - txLastMs) : -1L;
        // "connected" means the multicast group is joined. "live" means
        // packets are actually ARRIVING - the two are not the same thing,
        // and a board that is measuring its own microphone says "connected"
        // and not "live".
        u["live"]      = udpSyncConnected && (last_UDPTime != 0) &&
                          ((millis() - last_UDPTime) <= AUDIOSYNC_IDLE_MS);
      }
    }

    /*
     * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
     * Creating an "u" object allows you to add custom key/value pairs to the Info section of the WLED web UI.
     * Below it is shown how this could be used for e.g. a light sensor
     */
    void addToJsonInfo(JsonObject& root)
    {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");

      JsonArray infoArr = user.createNestedArray(FPSTR(_name));

      String uiDomString = F("<button class=\"btn btn-xs\" onclick=\"requestJson({");
      uiDomString += FPSTR(_name);
      uiDomString += F(":{");
      uiDomString += FPSTR(_enabled);
      uiDomString += enabled ? F(":false}});\">") : F(":true}});\">");
      uiDomString += F("<i class=\"icons");
      uiDomString += enabled ? F(" on") : F(" off");
      uiDomString += F("\">&#xe08f;</i>");
      uiDomString += F("</button>");
      infoArr.add(uiDomString);

      if (enabled) {
        bool audioSyncIDLE = false; // true if sound sync is not receiving

        // audio sync status
        if ((audioSyncEnabled & AUDIOSYNC_REC) && (!udpSyncConnected || (millis() - last_UDPTime > AUDIOSYNC_IDLE_MS))) // connected and nothing received in 2.5sec
          audioSyncIDLE = true;
        if ((audioSource == nullptr) || (!audioSource->isInitialized()))                                   // local audio not configured
          audioSyncIDLE = false;

        // The input level slider is a CONTROL, and controls belong on the
        // usermod's own settings page. The info pane is for reading, and a
        // slider in it is one more thing to look past.

        // The following can be used for troubleshooting user errors and is so not enclosed in #ifdef WLED_DEBUG
        // current Audio input
        //
        // Every row in this pane is ONE short string. The pane renders a row's
        // first two elements back to back with no separator, so prose reads as
        // more value rather than as a unit - and the whole point of the pane
        // is to be read at a glance.
        infoArr = user.createNestedArray(F("Audio Source"));
        if ((audioSyncEnabled == AUDIOSYNC_REC) || (!audioSyncIDLE && (audioSyncEnabled == AUDIOSYNC_REC_PLUS))){
          // UDP sound sync - receive mode
          if (udpSyncConnected) {
            infoArr.add((millis() - last_UDPTime < AUDIOSYNC_IDLE_MS)
                          ? F("UDP sync, receiving") : F("UDP sync, idle"));
          } else {
            infoArr.add(F("UDP sync, no connection"));
          }
#ifndef ARDUINO_ARCH_ESP32  // substitute for 8266
        } else {
          infoArr.add(F("sound sync Off"));
        }
#else  // ESP32 only
        } else {
          // I2S digital input (analog-ADC path removed with I2SAdcSource)
          if (audioSource && (audioSource->isInitialized())) {
            // audio source successfully configured
            infoArr.add((dmType != 51) ? F("I2S digital") : F("legacy I2S PDM"));
          } else {
            // error during audio source setup
            infoArr.add(F("not initialized"));
          }
        }

        // Active codec name (matches the "Audio Source" dropdown in the
        // usermod settings page). This is the verbatim label string used
        // for the currently-selected dmType, so the info pane stays in sync
        // with whatever the user picked. The spelling lives in arCodecName(),
        // which the /json/ar endpoint uses too - the two must not disagree.
        {
          infoArr = user.createNestedArray(F("Active codec"));
          infoArr.add(arCodecName(dmType));
        }

        // The codec's own registers, the clock it is actually fed with, the
        // UDP sync status, the frame's timings and every band array live on
        // the /json/ar endpoint now. They are numbers a script wants, not
        // things a person reads at a glance, and the info pane had grown into
        // a wall of them.

        // Sound processing (FFT and input filters). Only when it is NOT
        // running - "running" is the good case, and the good case does not
        // need a row.
        if (!audioSource || (disableSoundProcessing != false)) {
          infoArr = user.createNestedArray(F("Sound Processing"));
          infoArr.add(F("suspended"));
        }

        // Estimator self-test: the estimator measured against the unit
        // contract, on the hardware, with the real transform and the real
        // window. A centre that reads more than 0.15 dB off the contract is a
        // normalisation error, and it would be invisible in the bars.
        //
        // It only gets a row when it FAILS. A passing self-test is the
        // expected case, and a pane that lists it is a pane with one more
        // line of noise in it.
        if (estTestDone && (estTestSkipped || (estTestFails != 0) || (estTestNbrFails != 0))) {
          infoArr = user.createNestedArray(F("Estimator self-test"));
          char esBuf[96];
          const int total = NUM_GEQ_CHANNELS + lay31.count;
          if (estTestSkipped) {
            // A test that did not run must never report PASS.
            infoArr.add(F("FAILED - could not be tested"));
          } else {
            snprintf(esBuf, sizeof(esBuf),
                     "FAILED: %d of %d centres, %d of %d neighbours, worst %+.3f dB @ %.0f Hz",
                     estTestFails, total, estTestNbrFails, estTestNbrChecks,
                     (double)estTestWorst, (double)estTestWorstF);
            infoArr.add(esBuf);
          }
        }

        // Chain self-test: the input chain MEASURED against its own analytic
        // model. Published here because the serial console is not always
        // reachable - on this build the app's console is UART0 while the USB
        // port carries only the IDF log, so a self-test that exists only as
        // serial output cannot be read.
        //
        // Like the estimator's, it only gets a row when it FAILS.
        if (selfTestResult.valid && (selfTestResult.fails != 0)) {
          infoArr = user.createNestedArray(F("Chain self-test"));
          char stBuf[96];
          snprintf(stBuf, sizeof(stBuf),
                   "FAILED: %d of %d probes, impl %+.3f dB @ %.0f Hz, sine %+.3f dB @ %.0f Hz",
                   selfTestResult.fails, selfTestResult.probes,
                   (double)selfTestResult.worstImpl, (double)selfTestResult.worstImplF,
                   (double)selfTestResult.worstSine, (double)selfTestResult.worstSineF);
          infoArr.add(stBuf);
        }

        // The level tracker: the four numbers that decide what is published.
        //
        // They are the whole of the level / floor / gate / AGC chain, and each
        // one is a DIFFERENT quantity - which is the point. The old code had a
        // single number (a peak-minus-floor difference) feeding all of them,
        // and the floor it subtracted was the same number the gate compared
        // against.
        {
          char lvlBuf[64];
          infoArr = user.createNestedArray(F("Level (dB)"));
          snprintf(lvlBuf, sizeof(lvlBuf), "%.1f", (double)arLevelDb);
          infoArr.add(lvlBuf);

          infoArr = user.createNestedArray(F("Noise floor (dB)"));
          snprintf(lvlBuf, sizeof(lvlBuf), "%.1f", (double)arFloorDb);
          infoArr.add(lvlBuf);

          infoArr = user.createNestedArray(F("Gate"));
          snprintf(lvlBuf, sizeof(lvlBuf), "%s, %+.1f dB over floor",
                   arGateOpen ? "open" : "shut", (double)(arLevelDb - arFloorDb));
          infoArr.add(lvlBuf);

          infoArr = user.createNestedArray(F("Display offset (dB)"));
          snprintf(lvlBuf, sizeof(lvlBuf), "%+.1f", (double)arAgcDb);
          infoArr.add(lvlBuf);

          // The WINDOW: auto-range or fixed, and in the fixed case the two
          // numbers that define it. It is ONE window - the 16 bars and the 31
          // RTA bars are drawn in it, at the same scale, because the 16 bars
          // ARE 16 of the 31.
          //
          // The auto-range has no absolute scale - its top is wherever the
          // last peak put it - so the implied absolute window is reported as
          // well. That is the number to compare with an analyser's own grid:
          // "our display is currently showing -12 to -52 dB" is comparable
          // with the analyser's, "our bars look like that" is not.
          infoArr = user.createNestedArray(F("RTA window"));
          if (arAutoRange) {
            snprintf(lvlBuf, sizeof(lvlBuf), "auto, %.1f..%.1f dB",
                     (double)(AR_WINDOW_TARGET_DB - arAgcDb - arWindowRangeDb),
                     (double)(AR_WINDOW_TARGET_DB - arAgcDb));
          } else {
            snprintf(lvlBuf, sizeof(lvlBuf), "fixed, %.1f..%.1f dB",
                     (double)arWindowFloorDb, (double)arWindowTopDb);
          }
          infoArr.add(lvlBuf);

          // The reference offset: the difference between our dB and the
          // analyser's. It is applied to the MEASUREMENT, so the band levels
          // below are already on the analyser's scale.
          infoArr = user.createNestedArray(F("RTA reference (dB)"));
          snprintf(lvlBuf, sizeof(lvlBuf), "%+.1f", (double)rtaRefDb);
          infoArr.add(lvlBuf);

          // The display trim and the span: the last two numbers that decide
          // where a bar lands. The trim is the user's ('d'/'D'), the span is
          // how much of the spectrum's shape the bar covers below the top.
          // It is the shared window's span - both views are drawn in it.
          infoArr = user.createNestedArray(F("Display trim (dB)"));
          snprintf(lvlBuf, sizeof(lvlBuf), "%+.1f, span %.0f dB",
                   (double)TROYHACKS_DBREF, (double)arWindowRangeDb);
          infoArr.add(lvlBuf);

          // The 31 RTA band levels are NOT a row here any more. They are a
          // wall of thirty-one numbers, and the info pane is for the short
          // facts - the machine detail belongs on /json/ar, where the same
          // numbers live in the "rta" object, one per band, with the band
          // centres beside them.

          // The filter bank that measures the bottom eight of those bands
          // reports the SHAPED power - the bandpass's output power - which is
          // what a hardware analyser shows, and it is the only form in which
          // these bars can be compared with one bar for bar.
          //
          // The bank's own numbers - its band count, its rate, its detector's
          // time constant, its cost per frame - live on the /json/ar endpoint.
          //
          // Its self-test still gets a row, because a self-test that FAILED
          // is a fault, and faults belong where they are seen.
          if (bankReady) {
            // The self-test's verdict - only when it FAILED. A passing
            // self-test is the expected case and does not need a row.
            if (!bankTestDone) {
              infoArr = user.createNestedArray(F("RTA bank self-test"));
              infoArr.add(F("not run"));
            } else if ((bankTestFails != 0) || (bankTestNbrFails != 0)) {
              char bankBuf[96];
              infoArr = user.createNestedArray(F("RTA bank self-test"));
              snprintf(bankBuf, sizeof(bankBuf),
                       "FAILED: %d bands, %d of %d neighbours, worst %+.3f dB @ %.0f Hz",
                       bankTestFails, bankTestNbrFails, bankTestNbrChecks,
                       (double)bankTestWorst, (double)bankTestWorstF);
              infoArr.add(bankBuf);
            }
          }
        }
#endif

        // The button that puts the display's auto-range back where it started.
        //
        // The serial console's 'A' does the same thing: the trim and the span
        // go back to their boot values, and both auto-ranges go back to zero
        // with the RTA view in auto-range mode again.
        {
          infoArr = user.createNestedArray(F("Reset range"));
          infoArr.add(F("<button class=\"btn btn-xs\" onclick=\"requestJson({AudioReactive:{resetRange:true}});\">reset</button>"));
        }

        // The link to the machine-readable twin of this page.
        //
        // Everything the analysis knows is published there as NUMBERS - the
        // timings, the codec's registers, the calibration, every band array -
        // so a script can read it without scraping prose off this pane.
        {
          infoArr = user.createNestedArray(F("Statistics"));
          infoArr.add(F("<a href=\"/json/ar\" target=\"_blank\">/json/ar</a>"));
        }
        // The UDP sync status, the frame's timings, the codec's registers and
        // every band array live on the /json/ar endpoint now. They are numbers
        // a script wants, not things a person reads at a glance, and the info
        // pane had grown into a wall of them.

        #if 1 || (defined(WLED_DEBUG) || defined(SR_DEBUG) || defined(SR_STATS))
        #ifdef ARDUINO_ARCH_ESP32
        // These print in ms directly from the same 10 µs stored units, so they
        // now AGREE with the info page above by construction. They always did -
        // the info page was the side that was wrong - but the agreement is now a
        // useful check: if the two ever disagree, someone has reintroduced a raw
        // value into one of the formatters.
        DEBUGSR_PRINTF("AR I2S cycle time: %5.2f ms\n", roundf(fftTaskCycle)/100.0f);
        DEBUGSR_PRINTF("AR Sampling time : %5.2f ms\n", roundf(sampleTime)/100.0f);
        DEBUGSR_PRINTF("AR filter time   : %5.2f ms\n", roundf(filterTime)/100.0f);
        DEBUGSR_PRINTF("AR FFT time      : %5.2f ms\n", roundf(fftTime)/100.0f);
        DEBUGSR_PRINTF("AR   windowing   : %5.2f ms\n", roundf(tWindow)/100.0f);
        DEBUGSR_PRINTF("AR   frame prep  : %5.2f ms\n", roundf(tPrep)/100.0f);
        DEBUGSR_PRINTF("AR   ESP-DSP FFT : %5.2f ms\n", roundf(tFft)/100.0f);
        DEBUGSR_PRINTF("AR   bitrev +mag : %5.2f ms\n", roundf(tPost)/100.0f);
        DEBUGSR_PRINTF("AR   bands +post : %5.2f ms\n", roundf(tFinal)/100.0f);
        DEBUGSR_PRINTF("AR   tail        : %5.2f ms\n", roundf(tTail)/100.0f);
        DEBUGSR_PRINTF("AR   tick floor  : %5.2f ms\n", roundf(tFloor)/100.0f);
        DEBUGSR_PRINTF("AR   deadline w/o: %5.2f ms\n", roundf(tWait)/100.0f);
        DEBUGSR_PRINTF("AR   loop entry  : %5.2f ms\n", roundf(tLoop)/100.0f);
        {
          DEBUGSR_PRINTF("AR cycle parts  : %5.2f ms (residual %+.3f ms)\n",
                         roundf(tParts)/100.0f, tResidual/100.0f);
        }
        #endif
        #endif
      }
    }


    /*
     * addToJsonState() can be used to add custom entries to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void addToJsonState(JsonObject& root)
    {
      if (!initDone) return;  // prevent crash on boot applyPreset()
      JsonObject usermod = root[FPSTR(_name)];
      if (usermod.isNull()) {
        usermod = root.createNestedObject(FPSTR(_name));
      }
      usermod["on"] = enabled;
      // The transform's SWITCH position - what was asked for, not what is
      // running. It is the request, because the state object is also the
      // thing a client writes to: reading it back and posting it again must
      // not change anything. What is actually RUNNING is published in
      // /json/ar as "transform.on" / "transform.forced", because a bank that
      // failed its self-test forces the transform back on whatever the
      // request says.
      usermod["transform"] = arUseTransform;
    }


    /*
     * readFromJsonState() can be used to receive data clients send to the /json/state part of the JSON API (state object).
     * Values in the state object may be modified by connected clients
     */
    void readFromJsonState(JsonObject& root)
    {
      if (!initDone) return;  // prevent crash on boot applyPreset()
      bool prevEnabled = enabled;
      JsonObject usermod = root[FPSTR(_name)];
      if (!usermod.isNull()) {
        if (usermod[FPSTR(_enabled)].is<bool>()) {
          enabled = usermod[FPSTR(_enabled)].as<bool>();
          if (prevEnabled != enabled) onUpdateBegin(!enabled);
        }
#ifdef ARDUINO_ARCH_ESP32
        if (usermod[FPSTR(_inputLvl)].is<int>()) {
          inputLevel = min(255,max(0,usermod[FPSTR(_inputLvl)].as<int>()));
        }
#endif
        // The measurement mode and the calibration's target curve.
        //
        // Both are also config keys; these are the runtime path, so a change
        // takes effect without a reboot. The target change sets arCalDirty,
        // and the FFT task rebuilds the calibration - the rebuild measures an
        // impulse response, and it must not run in the HTTP handler.
        if (usermod[F("measure")].is<int>()) {
          freqMeasureIntegrated = (usermod[F("measure")].as<int>() != 0);
        }
        if (usermod[F("target")].is<int>()) {
          const bool pink = (usermod[F("target")].as<int>() != 0);
          if (pink != calTargetPink) {
            calTargetPink = pink;
            arCalDirty = true;
          }
        }
        // The per-band display trim is GONE. The sixteen display bands are
        // sixteen of the thirty-one RTA bands, so their trim is the RTA's
        // rtaTrim[] - the same numbers, read at the bands that are shown.
        // An old config may still carry the array; it is ignored, and the
        // config is rewritten without it.
        //
        // The display bank's A/B switch is gone with the bank itself: the
        // sixteen display bands are sixteen of the thirty-one, so there is no
        // second measurement to choose between.
        // The proof-of-concept switch: the transform on or off.
        //
        // This and the serial console's 'a' are the same entry point, so the
        // two can never disagree. (An earlier note here said the serial
        // console was UART0 and that a keystroke never reached WLED's
        // handler - that was wrong for this board: the USB-Serial-JTAG CDC
        // console on COM4 does reach it, and 'a' was measured working there.)
        if (usermod[F("transform")].is<bool>()) {
          audioReactiveTransformSet(usermod[F("transform")].as<bool>());
        }
        // Put the display's auto-range back where it started.
        //
        // This is what the serial console's 'A' does, and it does the whole
        // job: the display's manual trim goes back to the value the board
        // booted with, and both auto-range offsets (the 16 display bands and
        // the 31 RTA bands) go back to zero, with the display back in
        // auto-range MODE rather than left in a fixed window the user had
        // asked to get rid of.
        //
        // The calibration - the reference offset and the per-band trim - is
        // NOT reset: those correct the MEASUREMENT, and this is about the
        // display.
        if (usermod[F("resetRange")].is<bool>()) {
          if (usermod[F("resetRange")].as<bool>()) {
            TROYHACKS_DBREF = TROYHACKS_DBREF_BOOT;
            audioReactiveResetAutoRange();
            USER_PRINTF("AR: display back to auto-range - trim %+.1f dB, span %.0f dB\n",
                        (double)TROYHACKS_DBREF, (double)arWindowRangeDb);
          }
        }
        // Diagnostic: the codec's MCLK pre-divider. The vendor coefficient
        // tables all use 1 for MCLK = 256 x rate; this exists so 1 vs 3 can be
        // compared live, without reflashing, when working out what the codec's
        // clock tree is really doing. Not persisted - it is a diagnostic knob,
        // not a setting.
        if (usermod[FPSTR("codecDiv")].is<int>()) {
          int div = usermod[FPSTR("codecDiv")].as<int>();
          if (div < 1) div = 1;
          if (div > 8) div = 8;
          if (audioSource) {
            audioSource->setCodecClockDiv((uint8_t)div);
            USER_PRINTF("AR: codec MCLK pre-divider set to %d.\n", div);
          }
        }
      }
    }


    /*
     * addToConfig() can be used to add custom persistent settings to the cfg.json file in the "um" (usermod) object.
     * It will be called by WLED when settings are actually saved (for example, LED settings are saved)
     * If you want to force saving the current state, use serializeConfig() in your loop().
     * 
     * CAUTION: serializeConfig() will initiate a filesystem write operation.
     * It might cause the LEDs to stutter and will cause flash wear if called too often.
     * Use it sparingly and always in the loop, never in network callbacks!
     * 
     * addToConfig() will make your settings editable through the Usermod Settings page automatically.
     *
     * Usermod Settings Overview:
     * - Numeric values are treated as floats in the browser.
     *   - If the numeric value entered into the browser contains a decimal point, it will be parsed as a C float
     *     before being returned to the Usermod.  The float data type has only 6-7 decimal digits of precision, and
     *     doubles are not supported, numbers will be rounded to the nearest float value when being parsed.
     *     The range accepted by the input field is +/- 1.175494351e-38 to +/- 3.402823466e+38.
     *   - If the numeric value entered into the browser doesn't contain a decimal point, it will be parsed as a
     *     C int32_t (range: -2147483648 to 2147483647) before being returned to the usermod.
     *     Overflows or underflows are truncated to the max/min value for an int32_t, and again truncated to the type
     *     used in the Usermod when reading the value from ArduinoJson.
     * - Pin values can be treated differently from an integer value by using the key name "pin"
     *   - "pin" can contain a single or array of integer values
     *   - On the Usermod Settings page there is simple checking for pin conflicts and warnings for special pins
     *     - Red color indicates a conflict.  Yellow color indicates a pin with a warning (e.g. an input-only pin)
     *   - Tip: use int8_t to store the pin value in the Usermod, so a -1 value (pin not set) can be used
     *
     * See usermod_v2_auto_save.h for an example that saves Flash space by reusing ArduinoJson key name strings
     * 
     * If you need a dedicated settings page with custom layout for your Usermod, that takes a lot more work.  
     * You will have to add the setting to the HTML, xml.cpp and set.cpp manually.
     * See the WLED Soundreactive fork (code and wiki) for reference.  https://github.com/atuline/WLED
     * 
     * I highly recommend checking out the basics of ArduinoJson serialization and deserialization in order to use custom settings!
     */
    void addToConfig(JsonObject& root)
    {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
#ifdef ARDUINO_ARCH_ESP32
    #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)
      JsonObject amic = top.createNestedObject(FPSTR(_analogmic));
      amic["pin"] = audioPin;
    #endif
      JsonObject dmic = top.createNestedObject(FPSTR(_digitalmic));
      dmic[F("type")] = dmType;
      // WLEDMM: align with globals I2C pins
      if ((dmType == 2) || (dmType == 6) || (dmType >= 10 && dmType <= 18)) {         // ES7243, ES8388, and IDF codecs
        if (i2c_sda >= 0) sdaPin = -1;              // -1 = use global
        if (i2c_scl >= 0) sclPin = -1;              // -1 = use global
      }
      JsonArray pinArray = dmic.createNestedArray("pin");
      pinArray.add(i2ssdPin);
      pinArray.add(i2swsPin);
      pinArray.add(i2sckPin);
      pinArray.add(mclkPin);
      pinArray.add(sdaPin);
      pinArray.add(sclPin);

      dmic[F("bits")]    = i2sBitsPerSample;
      // 0/1 numbers, not booleans - see the note on "limiter" below.
      dmic[F("rightSlot")] = i2sUseRightSlot ? 1 : 0;
      dmic[F("master")]  = i2sMaster ? 1 : 0;

      JsonObject cfg = top.createNestedObject("config");
      cfg[F("squelch")] = soundSquelch;
      // "gain" and "AGC" are GONE.
      //
      // "gain" (sampleGain) was never applied to anything - it was written,
      // read, and then ignored, so it was a field that did nothing.
      // "AGC" (soundAgc) was the old bang-bang AGC's preset (none / normal /
      // vivid / lazy). The AGC now is a single dB offset with a fixed attack
      // and release, always on, so there is no preset left to choose.
      //
      // Neither is written any more, and readFromConfig() removes them from
      // a stored config. That last part is not a detail: the settings page
      // renders whatever cfg.json holds, so a key that is merely "no longer
      // written" would sit on the page for ever.

      //WLEDMM: experimental settings
      JsonObject poweruser = top.createNestedObject("experiments");
      // "micLev", "Mic_Quality" and "freqDist" are GONE - the first is
      // deprecated (the floor tracker subsumes its three behaviours) and the
      // other two were never used at all.
      //
      // The five band-mutating toggles (AutoLevel, BandCompress,
      // NoiseFloorSub, HistogramNorm, Band0Lifter) are gone too. Each of
      // them mutated the band values - a per-band gain, a per-band
      // compression, a floor subtraction, a histogram normalisation, a
      // band-0 lift - and a mutated band value is not a measurement.
      //
      // "freqRMS" and "FFT_Window" were already gone: the averaging mode
      // belonged to the deleted bin-averaging helpers, and the windowing
      // function is hard-coded to Blackman-Harris
      // (dsps_wind_blackman_harris_f32 in alocateFFTBuffers).
      //
      // All of them are removed from a stored config by readFromConfig().
      poweruser[F("I2S_FastPath")] = doSlidingFFT;
      // Whether the transform runs is NOT saved here, any more.
      //
      // It used to be, because the transform was the measurement and the
      // banks were the experiment. Now it is the other way round: the banks
      // are the measurement and the transform is the reference they are
      // checked against. A reference that silently survives a reboot is a
      // liability - it would put 2.4 ms of FFT back into every frame and
      // nobody would remember asking for it.
      //
      // So the transform is a RUNTIME diagnostic: it is off at every boot,
      // and one keypress ('a') or one POST to the state object turns it on
      // for as long as you want the comparison. The live state is published
      // in the state object as "transform".
      //
      // A config that still carries "experiments.transform" from an earlier
      // build is simply ignored - see the read side.
      // The five "experiment" toggles (AutoLevel, BandCompress, NoiseFloorSub,
      // HistogramNorm, Band0Lifter) are gone. Each of them mutated the band
      // values - a per-band gain, a per-band compression, a floor subtraction,
      // a histogram normalisation, a band-0 lift - and a mutated band value is
      // not a measurement. They are removed from the config as well, so a
      // stored config that still has them is simply ignored.

      JsonObject freqScale = top.createNestedObject("frequency");
      // "scale" and "profile" are GONE. They chose the bar mapping (linear,
      // square root, logarithmic) and a per-band gain table
      // (fftResultPink[][]); both multiplied or re-mapped the measured value,
      // and the band value is now a calibrated measurement that is not
      // re-mapped. readFromConfig() removes them from a stored config.
      //
      // "dBtrim" is GONE as well - it was the user's trim on the bar heights,
      // in dB, applied after the auto-range. It is still a live runtime value
      // (the serial 'd'/'D' keys move TROYHACKS_DBREF) and it still starts at
      // 0 at every boot, but it is not a stored setting: a trim that only
      // holds for the signal it was measured on is not worth persisting.
      //
      // There is no "dBspan" any more: the span is now "rtaRange" below, and
      // it is the AUTO window's own width. The old "dBspan" was a per-install
      // span applied the same way in both modes, which is how the 16 bars came
      // to be drawn in a 24 dB window against a spectrum that spans 25 to 46
      // dB, which put half of them on the floor.
      //
      // Which measurement the TRANSFORM reads: 0 = the point PSD at the band
      // centre (the default), 1 = the integrated band power. The sixteen
      // display bands are sixteen of the thirty-one RTA bands and are always
      // integrated band powers, so this does not touch them.
      freqScale[F("measure")] = freqMeasureIntegrated ? 1 : 0;
      // The calibration's target curve: 0 = flat (a flat input reads flat),
      // 1 = pink (a pink input reads flat).
      freqScale[F("target")] = calTargetPink ? 1 : 0;
      // The window and the RTA view's calibration.
      //
      // "rtaAuto"  1 = the auto-range, 0 = the fixed window. The mode is the
      //            display's, not the RTA view's: there is ONE window and both
      //            views are drawn in it.
      // "rtaRange" the AUTO window's span, in dB: how far below the
      //            auto-ranged top the window reaches. The analyser's
      //            "Scale Range". Narrower clips the bottom of the spectrum
      //            off the display and stretches the shape over the height;
      //            wider brings the bottom back. The fixed window's own width
      //            is implied by its floor and its top, so it is not here.
      // "rtaFloor" the fixed window's floor, in dB.
      // "rtaTop"   the fixed window's top, in dB.
      // "rtaRef"   the reference offset, in dB: our dB = the analyser's dB
      //            + this. One number for the whole installation.
      // "rtaTau"   the filter bank's detector time constant, in ms: how fast
      //            the bottom 8 bars follow the signal.
      //
      // There is no "rtaTrim" array: the per-band trim is a live runtime
      // value (the serial 'n'/'p'/'y'/'Y' keys move it), not a stored
      // setting. The same goes for the sixteen-band "trim" array of an older
      // build - the sixteen bands are sixteen of the thirty-one, so they
      // share the thirty-one's trim.
      freqScale[F("rtaAuto")]  = arAutoRange ? 1 : 0;
      freqScale[F("rtaRange")] = arWindowRangeDb;
      freqScale[F("rtaFloor")] = arWindowFloorDb;
      freqScale[F("rtaTop")]   = arWindowTopDb;
      freqScale[F("rtaRef")]   = rtaRefDb;
      freqScale[F("rtaTau")]   = bankTauMs;
#endif
      JsonObject dynLim = top.createNestedObject("dynamics");
      // The limiter and the sync's "check_sequence" are written as 0/1
      // numbers, not as booleans. The settings page draws a boolean as a
      // checkbox, and addDropdown() silently does nothing on a checkbox - so
      // a dropdown on a boolean field never reaches the page and the choice
      // is lost. A number is drawn as a number field, which addDropdown()
      // can turn into a dropdown. readFromConfig() reads either form.
      dynLim[F("limiter")] = limiterOn ? 1 : 0;
      dynLim[F("rise")] = attackTime;
      dynLim[F("fall")] = decayTime;

      JsonObject sync = top.createNestedObject("sync");
      sync[F("port")] = audioSyncPort;
      sync[F("mode")] = audioSyncEnabled;
      sync[F("check_sequence")] = audioSyncSequence ? 1 : 0;
    }


    /*
     * readFromConfig() can be used to read back the custom settings you added with addToConfig().
     * This is called by WLED when settings are loaded (currently this only happens immediately after boot, or after saving on the Usermod Settings page)
     * 
     * readFromConfig() is called BEFORE setup(). This means you can use your persistent values in setup() (e.g. pin assignments, buffer sizes),
     * but also that if you want to write persistent values to a dynamic buffer, you'd need to allocate it here instead of in setup.
     * If you don't know what that is, don't fret. It most likely doesn't affect your use case :)
     * 
     * Return true in case the config values returned from Usermod Settings were complete, or false if you'd like WLED to save your defaults to disk (so any missing values are editable in Usermod Settings)
     * 
     * getJsonValue() returns false if the value is missing, or copies the value into the variable provided and returns true if the value is present
     * The configComplete variable is true only if the "exampleUsermod" object and all values are present.  If any values are missing, WLED will know to call addToConfig() to save them
     * 
     * This function is guaranteed to be called on boot, but could also be called every time settings are updated
     */
    bool readFromConfig(JsonObject& root)
    {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();

#ifdef ARDUINO_ARCH_ESP32
      // remember previous values
      auto oldEnabled = enabled;
      auto oldDMType = dmType;
      auto oldI2SsdPin = i2ssdPin;
      auto oldI2SwsPin = i2swsPin;
      auto oldI2SckPin = i2sckPin;
      auto oldI2SmclkPin = mclkPin;
      auto oldBits = i2sBitsPerSample;
      auto oldRightSlot = i2sUseRightSlot;
      auto oldMaster = i2sMaster;
#endif

      configComplete &= arGetBool(top[FPSTR(_enabled)], enabled);
#ifdef ARDUINO_ARCH_ESP32
    #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)
      configComplete &= arGetNumber(top[FPSTR(_analogmic)]["pin"], audioPin);
    #else
      audioPin = -1; // MCU does not support analog mic
    #endif

      configComplete &= arGetNumber(top[FPSTR(_digitalmic)]["type"],   dmType);
    #if  defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
      if (dmType == 0) dmType = SR_DMTYPE;   // MCU does not support analog
      #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3)
      if (dmType == 5) dmType = SR_DMTYPE;   // MCU does not support PDM
      if (dmType == 51) dmType = SR_DMTYPE;  // MCU does not support legacy PDM
      #endif
    #else
      if (dmType == 5) useInputFilter = 1;      // enable filter for PDM
      if (dmType == 51) useInputFilter = 1;     // switch on filter for legacy PDM    
    #endif

      configComplete &= arGetNumber(top[FPSTR(_digitalmic)]["pin"][0], i2ssdPin);
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)]["pin"][1], i2swsPin);
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)]["pin"][2], i2sckPin);
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)]["pin"][3], mclkPin);
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)]["pin"][4], sdaPin);
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)]["pin"][5], sclPin);

      uint8_t bitsRead = i2sBitsPerSample;
      // "rightSlot" and "master" are stored as 0/1 numbers so that the
      // settings page can draw them as dropdowns. A stored config from an
      // older build still holds booleans; they read fine, and the type check
      // below marks the config incomplete so that it is rewritten in the new
      // form.
      uint8_t rightSlotRead = i2sUseRightSlot ? 1 : 0;
      uint8_t masterRead    = i2sMaster ? 1 : 0;
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)][F("bits")],      bitsRead);
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)][F("rightSlot")], rightSlotRead);
      configComplete &= arGetNumber(top[FPSTR(_digitalmic)][F("master")],    masterRead);
      // Sanitize bit-width: only 16/24/32 are valid.
      if (bitsRead == 16 || bitsRead == 24 || bitsRead == 32) i2sBitsPerSample = bitsRead;
      i2sUseRightSlot = (rightSlotRead != 0);
      i2sMaster = (masterRead != 0);
      if (top[FPSTR(_digitalmic)][F("rightSlot")].is<bool>()) configComplete = false;
      if (top[FPSTR(_digitalmic)][F("master")].is<bool>())    configComplete = false;

      configComplete &= arGetNumber(top["config"][F("squelch")], soundSquelch);

      //WLEDMM: experimental settings
      // "micLev" is DEPRECATED. Its three behaviours - a floating level, a
      // frozen level and a fast-frozen level - are all subsumed by the new
      // floor tracker, which reads an absolute level and a floor that are two
      // independent numbers.
      //
      // "Mic_Quality" and "freqDist" were never used at all.
      //
      // All three are removed from a stored config below, together with the
      // five band-mutating toggles of an older build.

      configComplete &= arGetNumber(top["experiments"][F("I2S_FastPath")], doSlidingFFT);
      // "experiments.transform" is deliberately NOT read.
      //
      // The transform is a runtime diagnostic now - the filter banks are the
      // measurement, and the transform is what they are checked against. It
      // is off at every boot, and a stored "transform" from an earlier build
      // is ignored rather than honoured: a reference that puts the FFT back
      // into every frame after a reboot, with nobody remembering asking for
      // it, is worse than not having the switch at all.
      //
      // To have the transform running, turn it on for the session: the 'a'
      // key, or the "transform" key in the state object.
      // The five deleted experiment toggles are not read any more - see the
      // note in addToConfig.


      // The dB display trim is GONE as a stored setting.
      //
      // "dBtrim" was the user's trim on the bar heights, in dB, applied after
      // the auto-range. It is still a live runtime value - the serial 'd'/'D'
      // keys move TROYHACKS_DBREF and the serial 'A' key puts it back - but
      // it is not read from the config any more, so it starts at 0 at every
      // boot.
      //
      // The old "dBref" key held a LINEAR gain coefficient and the old
      // "dBspan" key a per-install window span; both are gone for good
      // reasons (a legacy number read as a different quantity, and a span
      // that could put the bars on the floor), and both are removed from a
      // stored config below.
      //
      // What the board boots with is what the serial 'A' command restores, so
      // remember it. The serial adjustments are not written back to the
      // configuration, so this is the only record of where the display
      // started.
      TROYHACKS_DBREF_BOOT = TROYHACKS_DBREF;
      // The measurement mode: 0 = the point PSD at the band centre (the
      // default), 1 = the integrated band power.
      //
      // The three new keys ARE part of configComplete: a config written before
      // this build does not have them, and configComplete is what tells WLED
      // to rewrite the config with the defaults. Without that, the keys would
      // only ever appear the next time the user saved the settings by hand.
      {
        uint8_t measureRead = 0;
        if (arGetNumber(top["frequency"][F("measure")], measureRead))
          freqMeasureIntegrated = (measureRead != 0);
        else
          configComplete = false;   // the key is missing - the config gets rewritten
      }
      // The calibration's target curve: 0 = flat, 1 = pink. This is read
      // BEFORE the calibration is built (readFromConfig runs before setup),
      // so the first frame is already calibrated against the right target.
      {
        uint8_t targetRead = 0;
        if (arGetNumber(top["frequency"][F("target")], targetRead)) {
          const bool pink = (targetRead != 0);
          // At boot this runs BEFORE setup(), so the calibration has not been
          // built yet and it picks the target up by itself. If the config is
          // re-read later (the usermod settings page), the calibration already
          // exists and has to be rebuilt - the FFT task does that, because the
          // rebuild measures an impulse response and may not run here.
          if (pink != calTargetPink) {
            calTargetPink = pink;
            if (initDone) arCalDirty = true;
          }
        } else {
          configComplete = false;   // the key is missing - the config gets rewritten
        }
      }
      // The per-band trims are GONE as stored settings.
      //
      // The sixteen display bands are sixteen of the thirty-one RTA bands,
      // so there is one per-band trim (rtaTrimDb[]) and it is a live runtime
      // value: the serial 'n'/'p' keys walk the bands and 'y'/'Y' move the
      // trim of the selected one. A trim that only holds for the signal it
      // was measured on is not worth persisting, and 31 number fields on a
      // settings page is not a setting anybody wants.
      //
      // The sixteen-band "trim" array of an older build is gone as well, and
      // so is the "rtaTrim" array - both are removed from a stored config
      // below.
      // The RTA view's window and its calibration.
      //
      // These are read here and applied to the statics, and the statics are
      // read by the estimator every frame - so a changed value takes effect
      // on the next frame, with no rebuild of anything. The calibration is
      // part of the MEASUREMENT, not of the display, so it does not need the
      // FFT task's cooperation.
      {
        uint8_t autoRead = 1;
        if (arGetNumber(top["frequency"][F("rtaAuto")], autoRead)) {
          arAutoRange = (autoRead != 0);
        } else {
          configComplete = false;   // the key is missing - the config gets rewritten
        }
        // Each of these three is read on its own. They used to be read as one
        // all-or-nothing group, and that was a trap: a POST that set the
        // window's floor and top but left the reference offset out silently
        // discarded BOTH - the && short-circuited and the window never moved.
        // A missing key still marks the config incomplete (so it gets
        // rewritten with every key), but it does not undo the keys that were
        // there.
        float floorRead = arWindowFloorDb, topRead = arWindowTopDb, refRead = rtaRefDb;
        // The auto window's span. It is read on its own, like the floor and
        // the top, so that a POST which leaves it out does not throw the
        // others away.
        float rangeRead = arWindowRangeDb;
        if (arGetNumber(top["frequency"][F("rtaRange")], rangeRead)) {
          arWindowRangeDb = constrain(rangeRead, AR_WINDOW_SPAN_MIN, AR_WINDOW_SPAN_MAX);
        } else {
          configComplete = false;
        }
        if (arGetNumber(top["frequency"][F("rtaFloor")], floorRead)) {
          arWindowFloorDb = constrain(floorRead, AR_WINDOW_FLOOR_MIN, AR_WINDOW_FLOOR_MAX);
        } else {
          configComplete = false;
        }
        if (arGetNumber(top["frequency"][F("rtaTop")], topRead)) {
          arWindowTopDb = constrain(topRead, AR_WINDOW_FLOOR_MIN, AR_WINDOW_FLOOR_MAX);
        } else {
          configComplete = false;
        }
        if (arGetNumber(top["frequency"][F("rtaRef")], refRead)) {
          rtaRefDb = constrain(refRead, -AR_RTA_REF_MAX, AR_RTA_REF_MAX);
        } else {
          configComplete = false;
        }
        // The window has to have a width, or the bar mapping divides by
        // zero.
        if (arWindowTopDb - arWindowFloorDb < AR_WINDOW_SPAN_MIN) arWindowTopDb = arWindowFloorDb + AR_WINDOW_SPAN_MIN;
        // The bank's detector time constant. It is applied through
        // bankSetTau(), which recomputes the one-pole coefficient, so the two
        // cannot disagree - and it works whether or not the bank has been
        // built yet, because bankInit() reads the same variable.
        //
        // The RTA's top bank walks with it: the thirty-one bars are one view
        // and they move together.
        float tauRead = bankTauMs;
        if (arGetNumber(top["frequency"][F("rtaTau")], tauRead)) {
          bankSetTau(tauRead);
          rtaTauMs = bankTauMs;
          rtaTauSet = true;
          rtaTopSetTau(rtaTauMs);
        } else {
          configComplete = false;
        }
      }

      // ---- keys that are gone ----
      //
      // The settings page renders whatever the saved config holds, so a key
      // that addToConfig() merely stops writing would sit on the page for
      // ever. They are removed from the config here, and their presence
      // marks the config incomplete so that it is rewritten without them.
      {
        JsonObject cfgObj = top["config"];
        if (!cfgObj.isNull()) {
          if (cfgObj.containsKey("gain")) { cfgObj.remove("gain"); configComplete = false; }
          if (cfgObj.containsKey("AGC"))  { cfgObj.remove("AGC");  configComplete = false; }
        }
        JsonObject expObj = top["experiments"];
        if (!expObj.isNull()) {
          // deprecated or never used
          if (expObj.containsKey("micLev"))      { expObj.remove("micLev");      configComplete = false; }
          if (expObj.containsKey("Mic_Quality")) { expObj.remove("Mic_Quality"); configComplete = false; }
          if (expObj.containsKey("freqDist"))    { expObj.remove("freqDist");    configComplete = false; }
          // the five band-mutating toggles of an older build
          if (expObj.containsKey("AutoLevel"))      { expObj.remove("AutoLevel");      configComplete = false; }
          if (expObj.containsKey("BandCompress"))   { expObj.remove("BandCompress");   configComplete = false; }
          if (expObj.containsKey("NoiseFloorSub"))  { expObj.remove("NoiseFloorSub");  configComplete = false; }
          if (expObj.containsKey("HistogramNorm"))  { expObj.remove("HistogramNorm");  configComplete = false; }
          if (expObj.containsKey("Band0Lifter"))    { expObj.remove("Band0Lifter");    configComplete = false; }
          // the transform's own switch - it is a runtime diagnostic now
          if (expObj.containsKey("transform"))      { expObj.remove("transform");      configComplete = false; }
        }
        JsonObject freqObj = top["frequency"];
        if (!freqObj.isNull()) {
          // the bar mapping and the per-band gain table
          if (freqObj.containsKey("scale"))   { freqObj.remove("scale");   configComplete = false; }
          if (freqObj.containsKey("profile")) { freqObj.remove("profile"); configComplete = false; }
          // the display trim, and the per-band trims
          if (freqObj.containsKey("dBtrim"))  { freqObj.remove("dBtrim");  configComplete = false; }
          if (freqObj.containsKey("dBref"))   { freqObj.remove("dBref");   configComplete = false; }
          if (freqObj.containsKey("dBspan"))  { freqObj.remove("dBspan");  configComplete = false; }
          if (freqObj.containsKey("trim"))    { freqObj.remove("trim");    configComplete = false; }
          if (freqObj.containsKey("rtaTrim")) { freqObj.remove("rtaTrim"); configComplete = false; }
        }
      }
#endif
      // The limiter is stored as 0/1 (see addToConfig). A config written by
      // an older build holds a boolean; it reads the same way.
      uint8_t limiterRead = limiterOn ? 1 : 0;
      configComplete &= arGetNumber(top["dynamics"][F("limiter")], limiterRead);
      limiterOn = (limiterRead != 0);
      configComplete &= arGetNumber(top["dynamics"][F("rise")],  attackTime);
      configComplete &= arGetNumber(top["dynamics"][F("fall")],  decayTime);

      configComplete &= arGetNumber(top["sync"][F("port")], audioSyncPort);
      configComplete &= arGetNumber(top["sync"][F("mode")], audioSyncEnabled);
      uint8_t seqRead = audioSyncSequence ? 1 : 0;
      configComplete &= arGetNumber(top["sync"][F("check_sequence")], seqRead);
      audioSyncSequence = (seqRead != 0);

      // WLEDMM notify user when a reboot is necessary
      #ifdef ARDUINO_ARCH_ESP32
      if (initDone) {
        if ((audioSource != nullptr) && (oldDMType != dmType)) errorFlag = ERR_REBOOT_NEEDED;  // changing mic type requires reboot
        if (   (audioSource != nullptr) && (enabled==true)
            && ((oldI2SsdPin != i2ssdPin) || (oldI2SwsPin != i2swsPin) || (oldI2SckPin != i2sckPin)) ) errorFlag = ERR_REBOOT_NEEDED;  // changing mic pins requires reboot
        if ((audioSource != nullptr) && (oldI2SmclkPin != mclkPin)) errorFlag = ERR_REBOOT_NEEDED;  // changing MCLK pin requires reboot
        if ((audioSource != nullptr) && ((oldBits != i2sBitsPerSample) || (oldRightSlot != i2sUseRightSlot) || (oldMaster != i2sMaster))) errorFlag = ERR_REBOOT_NEEDED;  // changing I2S sample format requires reboot
        if ((oldDMType != dmType) && (oldDMType == 0)) errorFlag = ERR_POWEROFF_NEEDED;  // changing from analog mic requires power cycle
        if ((oldDMType != dmType) && (dmType == 0)) errorFlag = ERR_POWEROFF_NEEDED;  // changing to analog mic requires power cycle
      }
      #endif
      return configComplete;
    }


    void appendConfigData()
    {
      oappend(SET_F("ux='AudioReactive';"));        // ux = shortcut for Audioreactive - fingers crossed that "ux" isn't already used as JS var, html post parameter or css style
      oappend(SET_F("uxp=ux+':digitalmic:pin[]';")); // uxp = shortcut for AudioReactive:digitalmic:pin[]
      oappend(SET_F("addInfo(ux+':help',0,'<button onclick=\"location.href=&quot;https://mm.kno.wled.ge/soundreactive/Sound-Settings&quot;\" type=\"button\">?</button>');"));
#ifdef ARDUINO_ARCH_ESP32      
      //WLEDMM: add defaults
      #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)  // -S3/-S2/-C3 don't support analog audio
      #ifdef AUDIOPIN
        oappend(SET_F("xOpt(ux+':analogmic:pin',1,' ⎌',")); oappendi(AUDIOPIN); oappend(");"); 
      #endif
      oappend(SET_F("aOpt(ux+':analogmic:pin',1);")); //only analog options
      #endif

      oappend(SET_F("dd=addDropdown(ux,'digitalmic:type');"));
      #if SR_DMTYPE==254
        oappend(SET_F("addOption(dd,'None - network receive only (⎌)',254);"));
      #else
        oappend(SET_F("addOption(dd,'None - network receive only',254);"));
      #endif
      #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)
        #if SR_DMTYPE==0
          oappend(SET_F("addOption(dd,'Generic Analog (⎌)',0);"));
        #else
          oappend(SET_F("addOption(dd,'Generic Analog',0);"));
        #endif
      #endif
      #if SR_DMTYPE==1
        oappend(SET_F("addOption(dd,'Generic I2S (⎌)',1);"));
      #else
        oappend(SET_F("addOption(dd,'Generic I2S',1);"));
      #endif
      #if SR_DMTYPE==2
        oappend(SET_F("addOption(dd,'ES7243 (⎌)',2);"));
      #else
        oappend(SET_F("addOption(dd,'ES7243',2);"));
      #endif
      #if SR_DMTYPE==3
        oappend(SET_F("addOption(dd,'SPH0654 (⎌)',3);"));
      #else
        oappend(SET_F("addOption(dd,'SPH0654',3);"));
      #endif
      #if SR_DMTYPE==4
        oappend(SET_F("addOption(dd,'Generic I2S with Mclk (⎌)',4);"));
      #else
        oappend(SET_F("addOption(dd,'Generic I2S with Mclk',4);"));
      #endif
      #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3)
        #if SR_DMTYPE==5
          oappend(SET_F("addOption(dd,'Generic I2S PDM (⎌)',5);"));
        #else
          oappend(SET_F("addOption(dd,'Generic I2S PDM',5);"));
        #endif
        #if SR_DMTYPE==51
          oappend(SET_F("addOption(dd,'.Legacy I2S PDM ☾ (⎌)',51);"));
        #else
          oappend(SET_F("addOption(dd,'.Legacy I2S PDM ☾',51);"));
        #endif
      #endif
      #if SR_DMTYPE==6
        oappend(SET_F("addOption(dd,'ES8388 ☾ (⎌)',6);"));
      #else
        oappend(SET_F("addOption(dd,'ES8388 ☾',6);"));
      #endif
      #if SR_DMTYPE==7
        oappend(SET_F("addOption(dd,'WM8978 ☾ (⎌)',7);"));
      #else
        oappend(SET_F("addOption(dd,'WM8978 ☾',7);"));
      #endif
      #if SR_DMTYPE==8
        oappend(SET_F("addOption(dd,'AC101 ☾ (⎌)',8);"));
      #else
        oappend(SET_F("addOption(dd,'AC101 ☾',8);"));
      #endif
      #if SR_DMTYPE==9
        oappend(SET_F("addOption(dd,'ES8311 ☾ (⎌)',9);"));
      #else
        oappend(SET_F("addOption(dd,'ES8311 ☾',9);"));
      #endif
      #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
        #ifdef CONFIG_CODEC_ES8311_SUPPORT
          #if SR_DMTYPE==10
            oappend(SET_F("addOption(dd,'ES8311 (esp_codec_dev) (⎌)',10);"));
          #else
            oappend(SET_F("addOption(dd,'ES8311 (esp_codec_dev)',10);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_ES8388_SUPPORT
          #if SR_DMTYPE==11
            oappend(SET_F("addOption(dd,'ES8388 (esp_codec_dev) (⎌)',11);"));
          #else
            oappend(SET_F("addOption(dd,'ES8388 (esp_codec_dev)',11);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_ES8374_SUPPORT
          #if SR_DMTYPE==12
            oappend(SET_F("addOption(dd,'ES8374 (esp_codec_dev) (⎌)',12);"));
          #else
            oappend(SET_F("addOption(dd,'ES8374 (esp_codec_dev)',12);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_ZL38063_SUPPORT
          #if SR_DMTYPE==13
            oappend(SET_F("addOption(dd,'ZL38063 (esp_codec_dev) (⎌)',13);"));
          #else
            oappend(SET_F("addOption(dd,'ZL38063 (esp_codec_dev)',13);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_ES8389_SUPPORT
          #if SR_DMTYPE==14
            oappend(SET_F("addOption(dd,'ES8389 (esp_codec_dev) (⎌)',14);"));
          #else
            oappend(SET_F("addOption(dd,'ES8389 (esp_codec_dev)',14);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_ES7210_SUPPORT
          #if SR_DMTYPE==15
            oappend(SET_F("addOption(dd,'ES7210 (esp_codec_dev) (⎌)',15);"));
          #else
            oappend(SET_F("addOption(dd,'ES7210 (esp_codec_dev)',15);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_ES7243_SUPPORT
          #if SR_DMTYPE==16
            oappend(SET_F("addOption(dd,'ES7243 (esp_codec_dev) (⎌)',16);"));
          #else
            oappend(SET_F("addOption(dd,'ES7243 (esp_codec_dev)',16);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_ES7243E_SUPPORT
          #if SR_DMTYPE==17
            oappend(SET_F("addOption(dd,'ES7243E (esp_codec_dev) (⎌)',17);"));
          #else
            oappend(SET_F("addOption(dd,'ES7243E (esp_codec_dev)',17);"));
          #endif
        #endif
        #ifdef CONFIG_CODEC_CJC8910_SUPPORT
          #if SR_DMTYPE==18
            oappend(SET_F("addOption(dd,'CJC8910 (esp_codec_dev) (⎌)',18);"));
          #else
            oappend(SET_F("addOption(dd,'CJC8910 (esp_codec_dev)',18);"));
          #endif
        #endif
      #endif

      // I2S sample format (runtime-configurable; replaces legacy I2S_USE_RIGHT_CHANNEL / I2S_USE_16BIT_SAMPLES macros)
      oappend(SET_F("dd=addDropdown(ux,'digitalmic:bits');"));
      oappend(SET_F("addOption(dd,'32 bits (⎌)',32);"));
      oappend(SET_F("addOption(dd,'24 bits',24);"));
      oappend(SET_F("addOption(dd,'16 bits',16);"));
      oappend(SET_F("dd=addDropdown(ux,'digitalmic:rightSlot');"));
      oappend(SET_F("addOption(dd,'Left slot (⎌)',0);"));
      oappend(SET_F("addOption(dd,'Right slot',1);"));
      oappend(SET_F("dd=addDropdown(ux,'digitalmic:master');"));
      oappend(SET_F("addOption(dd,'Master (⎌)',1);"));
      oappend(SET_F("addOption(dd,'Slave',0);"));
      // The squelch is NOT the noise floor. The floor is TRACKED - it
      // follows the level down at once and creeps up with a 10 s time
      // constant - so it is not a setting and it is not on the page.
      //
      // The squelch is the MARGIN above that floor: the gate opens when
      // the level is this many dB above the floor, and shuts 4 dB below
      // that. It also seeds the floor at boot, below the level, so the
      // gate starts open.
      //
      // So "squelch = 1" means "the gate sits 1 dB above the noise", not
      // "the floor is 1 dB". The page says so, because the name - and the
      // old meaning of the setting, an absolute threshold - made it read
      // like a floor.
      oappend(SET_F("addInfo(ux+':config:squelch',1,'dB <i>above the tracked noise floor: the gate opens here and shuts 4 dB below it. The floor is measured, not set. Default ")); oappendi(SR_SQUELCH); oappend("</i>');");  // 0 is field type, 1 is actual field
      // There is no "config:gain" field any more: sampleGain was never
      // applied to anything, so it was a field that did nothing. The key is
      // removed from a stored config by readFromConfig().

      // The "config:AGC" dropdown is GONE. It chose between the legacy AGC's
      // three presets (normal / vivid / lazy), each one a table of PI gains
      // driving a bang-bang integrator. The new AGC is a single dB offset
      // applied to the band levels, with a fixed attack and release, and it is
      // always on - there is nothing left for a preset to select. The config
      // key is removed from a stored config as well.

      //WLEDMM: experimental settings
      oappend(SET_F("xx='experiments';")); // shortcut
      //
      // The "micLev", "Mic_Quality" and "freqDist" dropdowns are GONE - all
      // three are deprecated or never used (see readFromConfig). Their config
      // keys are removed from a stored config, so they do not linger on the
      // page as plain number fields.
      //
      // The five experiment toggles below the I2S_FastPath dropdown -
      // AutoLevel, BandCompress, NoiseFloorSub, HistogramNorm, Band0Lifter -
      // are GONE too, and their config keys with them. Each of them MUTATED
      // the band values - a per-band gain, a per-band compression, a floor
      // subtraction, a histogram normalisation, a band-0 lift - and a mutated
      // band value is not a measurement. They are removed from a stored config
      // as well.
      //
      // The "freqRMS" and "FFT_Window" dropdowns were already gone: the
      // averaging mode belonged to the deleted bin-averaging helpers, and the
      // windowing function is hard-coded to Blackman-Harris
      // (dsps_wind_blackman_harris_f32 in alocateFFTBuffers).

      oappend(SET_F("dd=addDropdown(ux,xx+':I2S_FastPath');"));
      oappend(SET_F("addOption(dd,'Off',0);"));
      oappend(SET_F("addOption(dd,'On  (⎌)',1);"));
      oappend(SET_F("addInfo(ux+':'+xx+':I2S_FastPath',1,'☾');"));

      oappend(SET_F("dd=addDropdown(ux,'dynamics:limiter');"));
      oappend(SET_F("addOption(dd,'Off',0);"));
      oappend(SET_F("addOption(dd,'On',1);"));
      // The index is 1, not 0: element 0 of a field's name collection is the
      // hidden input that carries the field's type, so " On " used to land
      // BEFORE the dropdown instead of after it.
      oappend(SET_F("addInfo(ux+':dynamics:limiter',1,'<i>slew-limits the level the volume effects see</i>');"));
      oappend(SET_F("addInfo(ux+':dynamics:rise',1,'ms <i>(&#x266A; effects only)</i>');"));
      oappend(SET_F("addInfo(ux+':dynamics:fall',1,'ms <i>(&#x266A; effects only)</i>');"));

      // The "frequency:scale" and "frequency:profile" dropdowns are GONE.
      //
      // "scale" chose the bar mapping - linear, square root, logarithmic - and
      // "profile" chose a per-band gain table (fftResultPink[][]), the hand-
      // measured microphone profiles. Both multiplied or re-mapped the
      // measured value, which is exactly what the new path does not do: the
      // band value is a calibrated measurement, and the display mapping is
      // applied once, in arPublishBands().
      //
      // Their config keys are removed from a stored config by
      // readFromConfig(), so they do not linger on the page.
      //
      // What is here instead:
      oappend(SET_F("dd=addDropdown(ux,'frequency:measure');"));
      oappend(SET_F("addOption(dd,'Point (narrowband level at the band centre)',0);"));
      oappend(SET_F("addOption(dd,'Integrated (band power over the band)',1);"));
      oappend(SET_F("addInfo(ux+':frequency:measure',1,'<i>what the 16 bars read. Point: a tone at the band centre reads its own level. Integrated: the energy over the whole band, like a hardware analyser - a tone away from the centre reads low</i>');"));

      oappend(SET_F("dd=addDropdown(ux,'frequency:target');"));
      oappend(SET_F("addOption(dd,'Flat',0);"));
      oappend(SET_F("addOption(dd,'Pink',1);"));
      oappend(SET_F("addInfo(ux+':frequency:target',1,'<i>the calibration target: a flat input reads flat, or a pink input (-3 dB/octave) reads flat. The chain itself is always measured and divided out</i>');"));

      // The window's mode: the auto-range, or the fixed floor/top below.
      // This one used to be a plain number field, because it was written as
      // 0/1 and nothing turned it into a dropdown - a field that says
      // "0" or "1" where "Auto" and "Fixed" are the choices.
      oappend(SET_F("dd=addDropdown(ux,'frequency:rtaAuto');"));
      oappend(SET_F("addOption(dd,'Auto-range',1);"));
      oappend(SET_F("addOption(dd,'Fixed window',0);"));
      oappend(SET_F("addInfo(ux+':frequency:rtaAuto',1,'<i>Auto: the window follows the loudest band. Fixed: the floor and the top below are used as they are</i>');"));
      oappend(SET_F("addInfo(ux+':frequency:rtaRange',1,'dB <i>how far below the top the auto window reaches - the Scale Range of a hardware analyser. Narrower clips the bottom off the bars and stretches the shape over the height</i>');"));
      oappend(SET_F("addInfo(ux+':frequency:rtaFloor',1,'dB <i>(fixed window)</i>');"));
      oappend(SET_F("addInfo(ux+':frequency:rtaTop',1,'dB <i>(fixed window)</i>');"));
      oappend(SET_F("addInfo(ux+':frequency:rtaRef',1,'dB <i>our dB = the reference dB + this</i>');"));
      oappend(SET_F("addInfo(ux+':frequency:rtaTau',1,'ms <i>the filter bank detector time constant</i>');"));

      // The values that are actually RUNNING.
      //
      // Six of these settings are also tuned from the serial console: 'b'/'B'
      // the window's floor, 'u'/'U' its top, 's'/'S' the auto window's span,
      // 'o'/'O' the reference offset, 'k'/'K' the bank's detector, and 'A' the
      // auto-range. Those keys change the RUNNING values - the stored config
      // keeps the old ones, because a serial keypress does not save the
      // config.
      //
      // The page is rendered from the stored config, so without this it would
      // show the OLD numbers, and a save would PUT THEM BACK: the page would
      // silently undo the tuning. That is the trap this closes.
      //
      // So the page is patched with what is running. A save then writes what
      // is running, and the stored config follows the tuning instead of
      // fighting it.
      {
        oappend(SET_F("arv=document.getElementsByName(ux+':frequency:rtaAuto')[1];if(arv)arv.value="));
        oappendi(arAutoRange ? 1 : 0);
        oappend(";");
        const char *liveFields[5] = {"rtaRange", "rtaFloor", "rtaTop", "rtaRef", "rtaTau"};
        const float liveValues[5] = {arWindowRangeDb, arWindowFloorDb, arWindowTopDb, rtaRefDb, bankTauMs};
        char num[24];
        for (int i = 0; i < 5; i++) {
          snprintf(num, sizeof(num), "%.3f", (double)liveValues[i]);
          oappend(SET_F("arv=document.getElementsByName(ux+':frequency:"));
          oappend(liveFields[i]);
          oappend(SET_F("')[1];if(arv)arv.value="));
          oappend(num);
          oappend(";");
        }
      }
#endif
      oappend(SET_F("dd=addDropdown(ux,'sync:mode');"));
      oappend(SET_F("addOption(dd,'Off',0);"));               // AUDIOSYNC_NONE
#ifdef ARDUINO_ARCH_ESP32
      oappend(SET_F("addOption(dd,'Send',1);"));              // AUDIOSYNC_SEND
#endif
      oappend(SET_F("addOption(dd,'Receive',2);"));           // AUDIOSYNC_REC
#ifdef ARDUINO_ARCH_ESP32
      oappend(SET_F("addOption(dd,'Receive or Local',6);"));  // AUDIOSYNC_REC_PLUS
#endif
      // check_sequence: Receiver skips out-of-sequence packets when enabled
      oappend(SET_F("dd=addDropdown(ux,'sync:check_sequence');"));
      oappend(SET_F("addOption(dd,'Off',0);"));
      oappend(SET_F("addOption(dd,'On',1);"));

      oappend(SET_F("addInfo(ux+':sync:check_sequence',1,'<i>when receiving</i> ☾<br> Sync audio data with other WLEDs');"));  // must append this to the last field of 'sync'

      oappend(SET_F("addInfo(ux+':digitalmic:type',1,'<i>requires reboot!</i>');"));  // 0 is field type, 1 is actual field
#ifdef ARDUINO_ARCH_ESP32
      oappend(SET_F("addInfo(uxp,0,'<i>sd/data/dout</i>','I2S SD');"));
      #ifdef I2S_SDPIN
        oappend(SET_F("xOpt(uxp,0,' ⎌',")); oappendi(I2S_SDPIN); oappend(");"); 
      #endif

      oappend(SET_F("addInfo(uxp,1,'<i>ws/clk/lrck</i>','I2S WS');"));
      oappend(SET_F("dRO(uxp,1);")); // disable read only pins
      #ifdef I2S_WSPIN
        oappend(SET_F("xOpt(uxp,1,' ⎌',")); oappendi(I2S_WSPIN); oappend(");"); 
      #endif

      oappend(SET_F("addInfo(uxp,2,'<i>sck/bclk</i>','I2S SCK');"));
      oappend(SET_F("dRO(uxp,2);")); // disable read only pins
      #ifdef I2S_CKPIN
        oappend(SET_F("xOpt(uxp,2,' ⎌',")); oappendi(I2S_CKPIN); oappend(");"); 
      #endif

      oappend(SET_F("addInfo(uxp,3,'<i>master clock</i>','I2S MCLK');"));
      oappend(SET_F("dRO(uxp,3);")); // disable read only pins
      #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32P4)
        oappend(SET_F("dOpt(uxp,3,2,2);")); //only use -1, 0, 1 or 3
        oappend(SET_F("dOpt(uxp,3,4,39);")); //only use -1, 0, 1 or 3
      #endif
      #ifdef MCLK_PIN
        oappend(SET_F("xOpt(uxp,3,' ⎌',")); oappendi(MCLK_PIN); oappend(");"); 
      #endif

      oappend(SET_F("addInfo(uxp,4,'','I2C SDA');"));
      oappend(SET_F("rOpt(uxp,4,'use global (")); oappendi(i2c_sda); oappend(")',-1);"); 
      #ifdef ES7243_SDAPIN
        oappend(SET_F("xOpt(uxp,4,' ⎌',")); oappendi(ES7243_SDAPIN); oappend(");"); 
      #endif

      oappend(SET_F("addInfo(uxp,5,'','I2C SCL');"));
      oappend(SET_F("rOpt(uxp,5,'use global (")); oappendi(i2c_scl); oappend(")',-1);"); 
      #ifdef ES7243_SCLPIN
        oappend(SET_F("xOpt(uxp,5,' ⎌',")); oappendi(ES7243_SCLPIN); oappend(");"); 
      #endif
      oappend(SET_F("dRO(uxp,5);")); // disable read only pins
#endif
    }


    /*
     * handleOverlayDraw() is called just before every show() (LED strip update frame) after effects have set the colors.
     * Use this to blank out some LEDs or set them to a different color regardless of the set effect mode.
     * Commonly used for custom clocks (Cronixie, 7 segment)
     */
    //void handleOverlayDraw()
    //{
      //strip.setPixelColor(0, RGBW32(0,0,0,0)) // set the first pixel to black
    //}

   
    /*
     * getId() allows you to optionally give your V2 usermod an unique ID (please define it in const.h!).
     * This could be used in the future for the system to determine whether your usermod is installed.
     */
    uint16_t getId()
    {
      return USERMOD_ID_AUDIOREACTIVE;
    }
};

// strings to reduce flash memory usage (used more than twice)
const char AudioReactive::_name[]       PROGMEM = "AudioReactive";
const char AudioReactive::_enabled[]    PROGMEM = "enabled";
const char AudioReactive::_inputLvl[]   PROGMEM = "inputLevel";
#if defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)
const char AudioReactive::_analogmic[]  PROGMEM = "analogmic";
#endif
const char AudioReactive::_digitalmic[] PROGMEM = "digitalmic";
const char AudioReactive::UDP_SYNC_HEADER[]    PROGMEM = "00002"; // new sync header version, as format no longer compatible with previous structure
const char AudioReactive::UDP_SYNC_HEADER_v1[] PROGMEM = "00001"; // old sync header version - need to add backwards-compatibility feature
