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
#include <esp_timer.h>
#define FFT_PREFER_EXACT_PEAKS  // use different FFT windowing -> results in "sharper" peaks and less "leaking" into other frequencies
// #define SR_STATS
#if !defined(FFTTASK_PRIORITY)
  #define FFTTASK_PRIORITY 6   // above async_tcp and USB Mass Storage
#endif
// #define FFT_MAJORPEAK_HUMAN_EAR // removed guards for this, so always on
#define SR_HIRES_TYPE double  // double precision is affordable on any target with an FPU; the direct-DFT sub-bin path leans on it
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

#define NUM_GEQ_CHANNELS 16                                           // number of frequency channels. Don't change !!

// audioreactive variables

static float    micDataReal = 0.0f;             // MicIn data with full 24bit resolution - lowest 8bit after decimal point
static float    multAgc = 1.0f;                 // sample * multAgc = sampleAgc. Our AGC multiplier
static float    sampleAvg = 0.0f;               // Smoothed Average sample - sampleAvg < 1 means "quiet" (simple noise gate)
static float    sampleAgc = 0.0f;               // Smoothed AGC sample
#ifdef SR_SQUELCH
  static uint8_t  soundAgc = 1;                   // Automagic gain control: 0 - none, 1 - normal, 2 - vivid, 3 - lazy (config value) - enable AGC if default "squelch" was provided
#else
  static uint8_t  soundAgc = 0;                   // Automagic gain control: 0 - none, 1 - normal, 2 - vivid, 3 - lazy (config value)
#endif

static float    volumeSmth = 0.0f;              // either sampleAvg or sampleAgc depending on soundAgc; smoothed sample
static float FFT_MajorPeak = 1.0f;              // FFT: strongest (peak) frequency
static float FFT_Magnitude = 0.0f;              // FFT: volume (magnitude) of peak frequency
static bool samplePeak = false;      // Boolean flag for peak - used in effects. Responding routine may reset this flag. Auto-reset after strip.getMinShowDelay()
static bool udpSamplePeak = false;   // Boolean flag for peak. Set at the same time as samplePeak, but reset by transmitAudioData
static unsigned long timeOfPeak = 0; // time of last sample peak detection.
volatile bool haveNewFFTResult = false; // flag to directly inform UDP sound sender when new FFT results are available (to reduce latency). Flag is reset at next UDP send

static uint8_t fftResult[NUM_GEQ_CHANNELS]= {0};   // Our calculated freq. channel result table to be used by effects
static float   fftCalc[NUM_GEQ_CHANNELS] = {0.0f}; // Try and normalize fftBin values to a max of 4096, so that 4096/16 = 256. (also used by dynamics limiter)
static float   fftAvg[NUM_GEQ_CHANNELS] = {0.0f};  // Calculated frequency channel results, with smoothing (used if dynamics limiter is ON)

static uint16_t zeroCrossingCount = 0; // number of zero crossings in the current batch of 512 samples

// TODO: probably best not used by receive nodes
static float agcSensitivity = 128;            // AGC sensitivity estimation, based on agc gain (multAgc). calculated by getSensitivity(). range 0..255

// user settable parameters for limitSoundDynamics()
#ifdef UM_AUDIOREACTIVE_DYNAMICS_LIMITER_OFF
static bool limiterOn = false;                 // bool: enable / disable dynamics limiter
#else
static bool limiterOn = true;
#endif
static uint8_t micQuality = 0;   // affects input filtering; 0 normal, 1 minimal filtering, 2 no filtering
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
#ifndef SR_SQUELCH
  uint8_t soundSquelch = 10;                  // squelch value for volume reactive routines (config value)
#else
  uint8_t soundSquelch = SR_SQUELCH;          // squelch value for volume reactive routines (config value)
#endif
#ifndef SR_GAIN
  uint8_t sampleGain = 60;                    // sample gain (config value)
#else
  uint8_t sampleGain = SR_GAIN;               // sample gain (config value)
#endif

// user settable options for FFTResult scaling
static uint8_t FFTScalingMode = 3;            // 0 none; 1 optimized logarithmic; 2 optimized linear; 3 optimized dB

// Index of the "Auto" row in fftResultPink[] - the profile derived at runtime
// from the actual filter chain. Declared here because the default below needs
// it; MAX_PINK (which it equals) is defined further down with the table itself.
#define PINK_AUTO 12

#ifndef SR_FREQ_PROF
  // Default to the derived (Auto) profile. The hand tables in fftResultPink[]
  // were captured against the old mean-over-bins estimator and the old filter
  // chain; with total-power bands they over-boost the top by ~20 dB. Auto is
  // computed at runtime from the same constants the filters use, so it stays
  // correct if SAMPLE_RATE or the FFT length change.
  static uint8_t pinkIndex = PINK_AUTO;       // 12 = Auto (derived); 0/1/2..9 = hand tables
#else
  static uint8_t pinkIndex = SR_FREQ_PROF;    // 0: default; 1: line-in; 2: IMNP441
#endif


// 
// AGC presets
//  Note: in C++, "const" implies "static" - no need to explicitly declare everything as "static const"
// 
#define AGC_NUM_PRESETS 3 // AGC presets:          normal,   vivid,    lazy
const double agcSampleDecay[AGC_NUM_PRESETS]  = { 0.9994f, 0.9985f, 0.9997f}; // decay factor for sampleMax, in case the current sample is below sampleMax
const float agcZoneLow[AGC_NUM_PRESETS]       = {      32,      28,      36}; // low volume emergency zone
const float agcZoneHigh[AGC_NUM_PRESETS]      = {     240,     240,     248}; // high volume emergency zone
const float agcZoneStop[AGC_NUM_PRESETS]      = {     336,     448,     304}; // disable AGC integrator if we get above this level
const float agcTarget0[AGC_NUM_PRESETS]       = {     112,     144,     164}; // first AGC setPoint -> between 40% and 65%
const float agcTarget0Up[AGC_NUM_PRESETS]     = {      88,      64,     116}; // setpoint switching value (a poor man's bang-bang)
const float agcTarget1[AGC_NUM_PRESETS]       = {     220,     224,     216}; // second AGC setPoint -> around 85%
const double agcFollowFast[AGC_NUM_PRESETS]   = { 1/192.f, 1/128.f, 1/256.f}; // quickly follow setpoint - ~0.15 sec
const double agcFollowSlow[AGC_NUM_PRESETS]   = {1/6144.f,1/4096.f,1/8192.f}; // slowly follow setpoint  - ~2-15 secs
const double agcControlKp[AGC_NUM_PRESETS]    = {    0.6f,    1.5f,   0.65f}; // AGC - PI control, proportional gain parameter
const double agcControlKi[AGC_NUM_PRESETS]    = {    1.7f,   1.85f,    1.2f}; // AGC - PI control, integral gain parameter
#if defined(WLEDMM_FASTPATH)
const float agcSampleSmooth[AGC_NUM_PRESETS]  = {  1/8.f,   1/5.f,  1/12.f}; // smoothing factor for sampleAgc (use rawSampleAgc if you want the non-smoothed value)
#else
const float agcSampleSmooth[AGC_NUM_PRESETS]  = {  1/12.f,   1/6.f,  1/16.f}; // smoothing factor for sampleAgc (use rawSampleAgc if you want the non-smoothed value)
#endif
// AGC presets end

static AudioSource *audioSource = nullptr;
static uint8_t useInputFilter = 0;                        // enables low-cut filtering. Applies before FFT.

//WLEDMM add experimental settings
static uint8_t micLevelMethod = 0;                        // 0=old "floating" miclev, 1=new  "freeze" mode, 2=fast freeze mode (mode 2 may not work for you)
#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32C3)
static constexpr uint8_t averageByRMS = false;                      // false: use mean value, true: use RMS (root mean squared).
#else
static constexpr uint8_t averageByRMS = true;                       // false: use mean value, true: use RMS (root mean squared). use better method on fast MCUs.
#endif
static uint8_t freqDist = 2;                              // 0=old 1=rightshift 2=psychoacoustic (log)
static uint8_t fftWindow = 0;                             // FFT windowing function (0 = default)
static uint8_t doSlidingFFT = 1;                            // 1 = use sliding window FFT (faster & more accurate)
static uint8_t useAutoLevel = 1;                           // 1 = per-band slow-decay AGC (auto-balance loudest band to fill display)
static float bandAutoGain[NUM_GEQ_CHANNELS] = {0.0f};     // per-band auto-gain (slow-decay AGC state)
static bool  bandAutoGainInited = false;                   // first-call init flag (seed at 1.0 once)
static uint8_t useBandCompress = 0;                        // 1 = per-band log compression: log(1+k*x) / log(1+k*Xmax) — compresses within-band dynamic range
static constexpr float BAND_COMPRESS_K = 4.0f;             // compression factor (k=4: ~3 dB at top, ~20 dB at floor)
static uint8_t useNoiseFloorSub = 0;                       // 1 = subtract per-band slow min (noise floor) from each band — helps in quiet environments
static uint8_t useHistogramNorm = 0;                       // 1 = single global gain via loudest-band peak (alternative to per-band AGC)
static uint8_t useBand0Lifter = 0;                        // 1 = soft "dance music fixer": weak coupling from band 1 to band 0 to compensate for poor low-freq mic response. band0 follows band1's contour without matching it.

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
static float fftAddAvg(int from, int to);   // average of several FFT result bins
void FFTcode(void * parameter);             // audio processing task: read samples, run FFT, fill GEQ channels from FFT results
static void runMicFilter(uint16_t numSamples, float *sampleBuffer);          // pre-filtering of raw samples (band-pass)
static void postProcessFFTResults(bool noiseGateOpen, int numberOfChannels, bool i2sFastpath); // post-processing and post-amp of GEQ channels


static TaskHandle_t FFT_Task = nullptr;

// Table of multiplication factors so that we can even out the frequency response.
#define MAX_PINK 12  // 0 = standard, 1= line-in (pink noise only), 2..4 = IMNP441, 5..6 = ICS-43434, ,7=SPM1423, 8..9 = userdef, 10= flat (no pink noise adjustment), 12= Auto (derived at runtime - see derivedPink)
static const float fftResultPink[MAX_PINK+1][NUM_GEQ_CHANNELS] = { 
          { 1.70f, 1.71f, 1.73f, 1.78f, 1.68f, 1.56f, 1.55f, 1.63f, 1.79f, 1.62f, 1.80f, 2.06f, 2.47f, 3.35f, 6.83f, 9.55f },  //  0 default from SR WLED
      //  { 1.30f, 1.32f, 1.40f, 1.46f, 1.52f, 1.57f, 1.68f, 1.80f, 1.89f, 2.00f, 2.11f, 2.21f, 2.30f, 2.39f, 3.09f, 4.34f },  //  - Line-In Generic -> pink noise adjustment only
          { 2.35f, 1.32f, 1.32f, 1.40f, 1.48f, 1.57f, 1.68f, 1.80f, 1.89f, 1.95f, 2.14f, 2.26f, 2.50f, 2.90f, 4.20f, 6.50f },  //  1 Line-In CS5343 + DC blocker

          { 1.82f, 1.72f, 1.70f, 1.50f, 1.52f, 1.57f, 1.68f, 1.80f, 1.89f, 2.00f, 2.11f, 2.21f, 2.30f, 2.90f, 3.86f, 6.29f},   //  2 IMNP441 datasheet response profile * pink noise
          { 2.80f, 2.20f, 1.30f, 1.15f, 1.55f, 2.45f, 4.20f, 2.80f, 3.20f, 3.60f, 4.20f, 4.90f, 5.70f, 6.05f,10.50f,14.85f},   //  3 IMNP441 - big speaker, strong bass
          // next one has not much visual differece compared to default IMNP441 profile
          { 12.0f, 6.60f, 2.60f, 1.15f, 1.35f, 2.05f, 2.85f, 2.50f, 2.85f, 3.30f, 2.25f, 4.35f, 3.80f, 3.75f, 6.50f, 9.00f},   //  4 IMNP441 - voice, or small speaker

          { 2.75f, 1.60f, 1.40f, 1.46f, 1.52f, 1.57f, 1.68f, 1.80f, 1.89f, 2.00f, 2.11f, 2.21f, 2.30f, 1.75f, 2.55f, 3.60f },  //  5 ICS-43434 datasheet response * pink noise
          { 2.90f, 1.25f, 0.75f, 1.08f, 2.35f, 3.55f, 3.60f, 3.40f, 2.75f, 3.45f, 4.40f, 6.35f, 6.80f, 6.80f, 8.50f,10.64f },  //  6 ICS-43434 - big speaker, strong bass

          { 1.65f, 1.00f, 1.05f, 1.30f, 1.48f, 1.30f, 1.80f, 3.00f, 1.50f, 1.65f, 2.56f, 3.00f, 2.60f, 2.30f, 5.00f, 3.00f },  //  7 SPM1423
          { 2.25f, 1.60f, 1.30f, 1.60f, 2.20f, 3.20f, 3.06f, 2.60f, 2.85f, 3.50f, 4.10f, 4.80f, 5.70f, 6.05f,10.50f,14.85f },  //  8 userdef #1 for ewowi (enhance median/high freqs)
          { 4.75f, 3.60f, 2.40f, 2.46f, 3.52f, 1.60f, 1.68f, 3.20f, 2.20f, 2.00f, 2.30f, 2.41f, 2.30f, 1.25f, 4.55f, 6.50f },  //  9 userdef #2 for softhack (mic hidden inside mini-shield)

          { 2.38f, 2.18f, 2.07f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.95f, 1.70f, 2.13f, 2.47f },   // 10 almost FLAT (IMNP441 but no PINK noise adjustments)
          { 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f },   // 11 DEAD FLAT (placeholder; the row is skipped)
          { 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f }    // 12 AUTO - placeholder, real values live in derivedPink[]
};

  /* how to make your own profile:
  * ===============================
   * preparation: make sure your microphone has direct line-of-sigh with the speaker, 1-2meter distance is best
   * Prepare your HiFi equipment: disable all "Sound enhancements" - like Loudness, Equalizer, Bass Boost. Bass/Treble controls set to middle.
   * Your HiFi equipment should receive its audio input from Line-In, SPDIF, HDMI, or another "undistorted" connection (like CDROM). 
   * Try not to use Bluetooth or MP3 when playing the "pink noise" audio. BT-audio and MP3 both perform "acoustic adjustments" that we don't want now.

   * SR WLED: enable AGC ("standard" or "lazy"), set squelch to a low level, check that LEDs don't reacts in silence.
   * SR WLED: select "Generic Line-In" as your Frequency Profile, "Linear" or "Square Root" as Frequency Scale
   * SR WLED: Dynamic Limiter On, Dynamics Fall Time around 4200 - makes GEQ hold peaks for much longer
   * SR WLED: Select GEQ effect, move all effect slider to max (i.e. right side)

   * Measure: play Pink Noise for 2-3 minutes - for examples from youtube https://www.youtube.com/watch?v=ZXtimhT-ff4
   * Measure: Take a Photo. Make sure that LEDs for each "bar" are well visible (ou need to count them later)

   * Your own profile: 
   *  - Target for each LED bar is 50% to 75% of the max height --> 8(high) x 16(wide) panel means target = 5. 32 x 16 means target = 22.
   *  - From left to right - count the LEDs in each of the 16 frequency columns (that's why you need the photo). This is the barheight for each channel.
   *  - math time! Find the multiplier that will bring each bar to to target.
   *    * in case of square root scale: multiplier = (target * target) / (barheight * barheight)
   *    * in case of linear scale:      multiplier = target / barheight
   * 
   *  - replace one of the "userdef" lines with a copy of the parameter line for "Line-In", 
   *  - go through your new "userdef" parameter line, multiply each entry with the mutliplier you found for that column.

   * Compile + upload
   * Test your new profile (same procedure as above). Iterate the process to improve results.
   */

// globals and FFT Output variables shared with animations
static float FFT_MajPeakSmth = 1.0f;            // FFT: (peak) frequency, smooth
#if 1 || defined(WLED_DEBUG) || defined(SR_DEBUG) || defined(SR_STATS)
static float fftTaskCycle = 0;      // avg cycle time for FFT task
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

// Breakdown of fftTime. fftTime was a single ~25 ms number with no way to tell
// which of the five things inside it was responsible, and that answer is what
// decides whether samplesFFT can stay at 2048. Same units as fftTime.
static float tWindow  = 0;          // memcpy + max-sample + zero-crossing + windowing
static float tBandDft = 0;          // biquad LPF write-back + captureSubBinBands()
static float tFft     = 0;          // dsps_fft4r_fc32 - the hardware transform itself
static float tPost    = 0;          // bitrev + cplx2real + magnitude fold + peak scan + pink
static float tFinal   = 0;          // band mapping + postProcessFFTResults()
#endif

// FFT Task variables (filtering and post-processing)
static float   lastFftCalc[NUM_GEQ_CHANNELS] = {0.0f};                // backup of last FFT channels (before postprocessing)

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
// audio source parameters and constant
constexpr SRate_t SAMPLE_RATE = 22050;        // Base sample rate in Hz - 22Khz is a standard rate. Physical sample time -> 23ms
//constexpr SRate_t SAMPLE_RATE = 16000;        // 16kHz - use if FFTtask takes more than 20ms. Physical sample time -> 32ms
//constexpr SRate_t SAMPLE_RATE = 20480;        // Base sample rate in Hz - 20Khz is experimental.    Physical sample time -> 25ms
//constexpr SRate_t SAMPLE_RATE = 10240;        // Base sample rate in Hz - previous default.         Physical sample time -> 50ms
#ifndef WLEDMM_FASTPATH
#define FFT_MIN_CYCLE 21                      // minimum time before FFT task is repeated. Use with 22Khz sampling
#else
    #define FFT_MIN_CYCLE 8                      // update period, ms - see note below
#endif
// FFT_MIN_CYCLE is the UPDATE RATE, not a CPU budget: it is how often the task
// wakes and publishes a result. The comment this replaced ("we only have 12ms to
// take 1/2 batch of samples") was an estimate of FFT cost made when the transform
// was software and the rate was 16 kHz. On ESP32-P4 the hardware FFT makes the
// whole task a small fraction of this, so the real ceiling is set by something
// else entirely - see I2S_RING_MS below, which is ~17x larger.

//#define FFT_MIN_CYCLE 30                      // Use with 16Khz sampling
//#define FFT_MIN_CYCLE 23                      // minimum time before FFT task is repeated. Use with 20Khz sampling
//#define FFT_MIN_CYCLE 46                      // minimum time before FFT task is repeated. Use with 10Khz sampling
#else
// slightly lower the sampling rate for -C3, to improve stability
//constexpr SRate_t SAMPLE_RATE = 20480;        // 20Khz; Physical sample time -> 25ms
//#define FFT_MIN_CYCLE 23                      // minimum time before FFT task is repeated.
constexpr SRate_t SAMPLE_RATE = 18000;          // 18Khz; Physical sample time -> 28ms
#define FFT_MIN_CYCLE 25                        // minimum time before FFT task is repeated.
// try 16Khz in case your device still lags and responds too slowly.
//constexpr SRate_t SAMPLE_RATE = 16000;        // 16Khz -> Physical sample time -> 32ms
//#define FFT_MIN_CYCLE 30                      // minimum time before FFT task is repeated.
#endif

// How much audio the I2S driver has buffered, in ms. This - not FFT_MIN_CYCLE -
// is the real deadline: if the FFT task runs longer than this, the DMA ring
// refills underneath it and samples are dropped. At 22.05 kHz with 24x128
// descriptors it works out at about 139 ms, so the task has roughly an order of
// magnitude of headroom rather than the 8 ms the old constant implied. Exceeding
// FFT_MIN_CYCLE is not dangerous, it just makes the update rate slip.
//
// Outside the SAMPLE_RATE branch above on purpose: fftBudgetVerdict() uses it,
// and that function is common to every target, so leaving it inside would break
// the -C3 build on an undefined name.
constexpr float I2S_RING_MS = (AR_I2S_DMA_DESCS * BLOCK_SIZE * 1000.0f) / (float)SAMPLE_RATE;

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
constexpr uint16_t samplesFFT = 2048;           // Samples in an FFT batch - This value MUST ALWAYS be a power of 2

// Hop: NEW samples consumed per FFT frame, and the retained overlap is whatever
// is left over (retainSamples), so consecutive windows share retainSamples.
//
// The old samplesFFT_2 conflated this with samplesFFT/2, which made the two
// inseparable: raising the transform length to buy frequency resolution silently
// halved the update rate, and there was no way to express the trade. They answer
// different questions - length sets what the analyser can resolve, hop sets how
// often it looks - so they are separate constants now.
//
// 256 is 11.6 ms at 22.05 kHz, so the DISPLAY is designed to update at 86 Hz
// whatever the transform length is. At N=2048 that is 87.5% overlap, which is a
// lot of redundant work but is the point: the window has to be long to resolve
// the bottom band, and consecutive windows are genuinely near-identical, so the
// display changes by the newest 11.6 ms each frame rather than jumping.
//
// 86 Hz is the design rate, not the achieved one: the task also has to fit its
// own work inside 11.6 ms, and at N=2048 the frame currently takes ~25 ms, so
// vTaskDelayUntil never sleeps and the real rate is ~35 Hz. That is what
// FRAME_PERIOD_MS below is for.
//
// The cost of a long window is attack time, not update rate. A 92.9 ms window
// cannot report a kick drum onset faster than it averages it away - the band
// value ramps over ~8 consecutive frames. That is the correct measurement of a
// 47 Hz band, not a bug, but it does mean the lows will read as a sustained
// envelope. If that turns out to be too soft, the fix is a second, short-window
// path for the bottom bands, not a shorter hop.
constexpr uint16_t hopSamples = 256;
constexpr uint16_t retainSamples = samplesFFT - hopSamples;

// Wall-clock time one frame's worth of audio takes to arrive. This - not
// FFT_MIN_CYCLE, and not I2S_RING_MS - is the CPU budget the FFT task has to
// meet. Exceed it and the task cannot consume the samples it is handed as fast
// as I2S delivers them, so vTaskDelayUntil falls behind, the update rate slips
// below the design rate, and the DMA ring drifts steadily fuller. It does not
// cost audio on its own (there is ~12x of ring left to absorb the drift), which
// is why the orange tier is a warning and not a failure.
//
// At hopSamples=256 and 22.05 kHz this is 11.6 ms. It is deliberately derived
// rather than a tunable: it is a property of the hop and the sample rate, and
// the whole point of splitting hopSamples out of samplesFFT was that these two
// numbers answer different questions and must not be conflated again.
constexpr float FRAME_PERIOD_MS = (hopSamples * 1000.0f) / (float)SAMPLE_RATE;

// The FFT task keeps its Blackman-Harris window in a local array, so the task
// stack has to cover it: at samplesFFT=2048 that array alone is 8 KB. Sized off
// samplesFFT so raising the transform
// length can never silently eat the stack. The task prints its own high-water
// mark at startup, so a genuine overflow is visible.
constexpr uint32_t FFT_TASK_STACK = (samplesFFT * sizeof(float)) * 4;
// the following are observed values, supported by a bit of "educated guessing"
//#define FFT_DOWNSCALE 0.65f                             // 20kHz - downscaling factor for FFT results - "Flat-Top" window @20Khz, old freq channels 
//#define FFT_DOWNSCALE 0.46f                             // downscaling factor for FFT results - for "Flat-Top" window @22Khz, new freq channels
#define FFT_DOWNSCALE 0.40f                             // downscaling factor for FFT results, RMS averaging
#define LOG_256  5.54517744f                            // log(256)

// Global magnitude scale applied to the spectrum before the bands are computed,
// so the result lands near 4096 at full scale. The sub-bin bands are measured on
// the time-domain signal, which happens BEFORE the FFT overwrites that buffer,
// so they carry the same factor by hand. It is a display scale only - it used to
// double as the bridge between the two band paths, which pinned the whole
// pipeline to one transform length. See the note above BAND_F_LOW.
constexpr float FFT_BIN_SCALE = 1.0f / 16.0f;

// dB display mapping (FFTScalingMode 3). Both ends of the mapping are runtime
// values in wled.h - TROYHACKS_DBREF ('d') sets where 0 dB lands and
// TROYHACKS_DBSPAN ('s') sets how many dB the bar covers below it. Neither can be
// a constant: the reference depends on the input level and gain settings, and the
// span depends on how much dynamic range the material actually has. Both are
// dialled in live over serial rather than rebuilt per guess.
//
// The span is the "variance" control and it matters more than it looks. A
// constant-Q band reports power over a width proportional to its centre
// frequency, so for a spectrum of slope 1/f^a the displayed level falls as
// f^(1-a): pink (a=1) reads dead flat, and typical pop/rock (a~1.2) spans only
// about 10 dB across all 16 bands. Laid out over 66 dB that is 16% of the bar
// and reads as a dead line; over 40 dB it is 40% and reads like an RTA.
constexpr float FFT_DB_FLOOR = 1e-7f;

// These are the input and output vectors.  Input vectors receive computed results from FFT.
static float* vReal = nullptr;       // FFT sample inputs / freq output -  these are our raw result bins
static float* vImag = nullptr;       // imaginary parts


static float* pinkFactors = nullptr;                        // "pink noise" correction factors
constexpr float pinkcenter = 23.66;                         // sqrt(560) - center freq for scaling is 560 hz.
constexpr float binWidth = SAMPLE_RATE / (float)samplesFFT; // frequency range of each FFT result bin

// Guard on the trapezoid sample count used to integrate the DFT across a
// sub-bin band. Only bands narrower than 2 bins take this path, and the count
// is 2 points per bin, so it never exceeds 8 in practice - see
// initBandLayout(). This is here rather than beside dftPowerAt() because
// initBandLayout(), which runs earlier in the file, is what uses it.
#define FFT_DFT_MAX_PTS 8

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
// down to DC. 40 Hz is the bottom of the range: it drops sub-bass that an LED
// display cannot usefully show, and in exchange band 0 stops under-reading by
// 10 dB (measured -10.1 dB with a DC-anchored bottom band, -2.3 dB here).
static constexpr float BAND_F_LOW = 40.0f;                  // lowest band edge, Hz
static float bandCentre[NUM_GEQ_CHANNELS];                  // band centre, Hz
static float bandEdgeLo[NUM_GEQ_CHANNELS];                  // band lower edge, Hz
static float bandEdgeHi[NUM_GEQ_CHANNELS];                  // band upper edge, Hz
static int   bandLo[NUM_GEQ_CHANNELS];                      // first bin, inclusive
static int   bandHi[NUM_GEQ_CHANNELS];                      // last bin, inclusive
static bool  bandSubBin[NUM_GEQ_CHANNELS];                  // true if narrower than one bin
static int   bandNpts[NUM_GEQ_CHANNELS];                    // DFT sample points across the band
static float subBinMag[NUM_GEQ_CHANNELS];                   // direct-DFT result, filled before the FFT
static float bandPowerScale = 1.0f;                          // set by initBandLayout()
static bool  bandLayoutReady = false;

// Precomputed DFT twiddles, one row of exp(j*2*pi*f*n/fs) for every integration
// point of every sub-bin band, interleaved (re, im) and sampled at n = 0..N-1.
//
// This table exists because dftPowerAt() used to generate the same twiddles at
// run time, carrying a rotation recurrence in double. On the ESP32-P4 that
// measured 23.10 ms of a 24.73 ms frame - 93% of the whole FFT task - because
// the P4 has a single-precision FPU and every one of those double operations is
// emulated in software. The hardware FFT it was competing with does the same
// work in 260 us.
//
// Recomputing a fixed number of constants 8192 times per frame is not a
// trade-off worth defending: the frequencies are settled the moment
// initBandLayout() returns and never change again, so the whole recurrence is
// loop-invariant. Table it once, at init, and the transform degenerates into
// the float multiply-accumulate the FPU was built for - the same numbers, with
// no precision argument left to make because nothing is being approximated.
//
// At samplesFFT=2048 only band 0 is still sub-bin (16.8 Hz wide against 21.5 Hz
// for two bins), at 4 points, so this is 4 * 2048 * 2 * sizeof(float) = 64 KB.
// Larger N shrinks it: at 512 the sub-bin bands and their point counts all rise,
// but there are more of them, not fewer - the table only grows if the layout
// gains sub-bin bands, and it cannot, because raising samplesFFT is exactly what
// removes them.
static int   dftOffset[NUM_GEQ_CHANNELS];                   // first twiddle row for this band
static int   dftRows = 0;                                   // total twiddle rows allocated
static float* dftTwiddle = nullptr;                         // [dftRows][samplesFFT] interleaved (re, im)

// ---------------------------------------------------------------------------
// Input chain, and the "Auto" pink profile that inverts it
// ---------------------------------------------------------------------------
// These two define the signal path that runs before the FFT. They live at file
// scope, not inside the FFT task, so initDerivedProfile() below can evaluate
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
// The LPF sits just above the geometric centre of the top band, which is what
// keeps ultrasonic content out of band 15 without gutting band 15 itself. That
// one IS tied to the layout, because it exists to protect the top band.
constexpr float DC_BLOCKER_HZ = 20.0f;                       // -3 dB corner, Hz
constexpr float INPUT_LPF_TOP_FRAC = 1.077138f;            // x centre of the top band -> 9963 Hz
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
static float coeffs_lpf[5];                                 // generated in initDerivedProfile()

static_assert(MAX_PINK == PINK_AUTO, "PINK_AUTO must be the last row of fftResultPink[]");

// "Auto" is the only profile that is not in fftResultPink[][]: it is computed at
// runtime as 1/|H_chain| at each band centre, so it is exact for whatever
// SAMPLE_RATE and filter settings this build uses, and re-derives itself if they
// change. Use it for line-in, where the input chain is fully known in software.
// It cannot help a microphone, whose response is not knowable here - that is
// what the hand-measured tables above are for.
static float derivedPink[NUM_GEQ_CHANNELS];

// Create FFT object

#define sqrt(x) sqrtf(x)             // little hack that reduces FFT time by 10-50% on ESP32 (as alternative to FFT_SQRT_APPROXIMATION)
#define sqrt_internal sqrtf          // see https://github.com/kosme/arduinoFFT/pull/83

// Helper functions

// float version of map()
static float mapf(float x, float in_min, float in_max, float out_min, float out_max){
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// compute average of several FFT result bins
// linear average
static float fftAddAvgLin(int from, int to) {
  float result = 0.0f;
  for (int i = from; i <= to; i++) {
    result += vReal[i];
  }
  return result / float(to - from + 1);
}
// Energy in a band: the SUM of bin powers, not their mean.
//
// This is the one that matches a real analyser. A fractional-octave filter
// (IEC 61260) reports the energy that passed through it, so a tone of a given
// amplitude reads the same in a 1-bin band and a 114-bin band. Averaging
// divides by the bin count and makes every wide band read 1/sqrt(N) low - a
// pure measurement artifact, worth 7.8x between band 1 and band 14 here.
static float fftBandEnergy(int from, int to) {
  double result = 0.0;
  for (int i = from; i <= to; i++) {
    result += (double)vReal[i] * (double)vReal[i];
  }
  return sqrtf(result);
}

// RMS average
static float fftAddAvgRMS(int from, int to) {
  double result = 0.0;
  for (int i = from; i <= to; i++) {
    result += vReal[i] * vReal[i];
  }
  return sqrtf(result / float(to - from + 1));
}

static float fftAddAvg(int from, int to) {
  if (from == to) return vReal[from];              // small optimization
  if (averageByRMS) return fftAddAvgRMS(from, to); // use RMS
  else return fftAddAvgLin(from, to);              // use linear average
}

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
  const float fHigh = SAMPLE_RATE * 0.5f;
  const float r = powf(fHigh / BAND_F_LOW, 1.0f / (float)NUM_GEQ_CHANNELS);
  const int   halfBins = samplesFFT >> 1;

  float edge[NUM_GEQ_CHANNELS + 1];
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++)
    bandCentre[k] = BAND_F_LOW * powf(r, (float)k + 0.5f);   // geometric midpoint of edge[k], edge[k+1]
  for (int k = 0; k <= NUM_GEQ_CHANNELS; k++) edge[k] = BAND_F_LOW * powf(r, (float)k);
  // edge[NUM_GEQ_CHANNELS] lands on fHigh exactly, since r^16 == fHigh / BAND_F_LOW.

  // Band power from a direct-DFT integral, expressed in fftBandEnergy()'s units.
  // Derived, not fitted - see the note above BAND_F_LOW for the derivation and
  // the measured agreement.
  bandPowerScale = sqrtf((float)samplesFFT / (float)SAMPLE_RATE);

  // A band narrower than two bins is measured by integrating the DFT across it
  // rather than by adding up bins, because at 43 Hz/bin a bin sum cannot place
  // a tone at the right frequency (and a 1-bin band whose centre is 15 Hz from
  // the bin centre scallops ~1.4 dB, which is audible as a dip in the response).
  //
  // bandNpts sets how finely that integral is sampled: two points per bin
  // across the band, floored at 3. Since only sub-bin bands use it and those
  // are by definition under 2 bins wide, this never exceeds 8 - the cap is a
  // guard, not a working limit.
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    bandEdgeLo[k] = edge[k];
    bandEdgeHi[k] = edge[k + 1];
    const float width = edge[k + 1] - edge[k];
    bandSubBin[k] = width < (2.0f * binWidth);
    bandNpts[k] = (int)ceilf(2.0f * width / binWidth);
    if (bandNpts[k] < 3) bandNpts[k] = 3;
    if (bandNpts[k] > FFT_DFT_MAX_PTS) bandNpts[k] = FFT_DFT_MAX_PTS;
    bandLo[k] = halfBins;
    bandHi[k] = -1;
  }

  // Give every bin to the band whose centre it is logarithmically nearest -
  // geometric midpoints, which keeps all 16 bands at the same Q. Every bin
  // lands in exactly one band, so nothing is dropped or counted twice, and
  // under-resolved bands are free to take no bins at all: they do not steal
  // bins from their neighbours, which is what a forced-contiguity walk did.
  for (int i = 0; i < halfBins; i++) {
    const float bf = (float)i * binWidth;
    int k = 0;
    if (bf > 0.0f) {
      const float lb = logf(bf);
      float best = fabsf(lb - logf(bandCentre[0]));
      for (int j = 1; j < NUM_GEQ_CHANNELS; j++) {
        const float d = fabsf(lb - logf(bandCentre[j]));
        if (d < best) { best = d; k = j; }
      }
    }
    if (i < bandLo[k]) bandLo[k] = i;
    if (i > bandHi[k]) bandHi[k] = i;
  }
  // Any band left without bins is measured directly, whatever its width.
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++)
    if (bandHi[k] < bandLo[k]) bandSubBin[k] = true;

  // The input LPF rides the top of the layout. bandCentre[] holds geometric
  // midpoints, so the last entry is the centre of the top band; sitting a little
  // above it attenuates band 15 without gutting it, and puts the corner below
  // Nyquist so nothing aliases back down into the bands. Deriving it here means
  // it follows SAMPLE_RATE and the band count instead of drifting from them.
  inputLpfHz = INPUT_LPF_TOP_FRAC * bandCentre[NUM_GEQ_CHANNELS - 1];

  // Bake the DFT twiddles now that bandSubBin[] and bandNpts[] are final - both
  // of the loops above can still change them, so this has to come after them and
  // not beside the first one. Every frequency the sub-bin path will ever ask for
  // is fixed from here on.
  //
  // This is the last thing initBandLayout() does, deliberately. Everything
  // above sets up state that has to stand whether or not the allocation
  // succeeds, and bandLayoutReady below must be set either way - leaving it
  // false to signal the failure would make every caller retry, and initBandLayout()
  // is called from the FFT task.
  dftRows = 0;
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    dftOffset[k] = dftRows;
    if (bandSubBin[k]) dftRows += bandNpts[k];
  }
  free(dftTwiddle);                                          // no-op on the first call
  dftTwiddle = nullptr;
  if (dftRows > 0) {
    // SPIRAM-preferred like the other FFT buffers, and it matters more here: this
    // table is written once at init and read-only thereafter, so there is no
    // reason for 64 KB of it to sit in internal RAM next to a 32 KB task stack.
    // On this board that is 64 KB out of 32 MB, and the traffic is one full pass
    // per frame: at the ~5 ms frames this change should produce, that is about
    // 1.3 MB/s, a few percent of even the slow end of a 16-bit PSRAM bus. It is
    // also read every frame, so it stays hot in the data cache.
    // MALLOC_CAP_INTERNAL stays as the fallback - a table that landed there
    // would be faster still, and either way beats a null one.
    const size_t n = (size_t)dftRows * samplesFFT * 2;
    #ifdef ESP32
      dftTwiddle = (float*)heap_caps_calloc_prefer(n, sizeof(float), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
    #else
      dftTwiddle = (float*)calloc(n, sizeof(float));
    #endif
  }
  // Out of memory leaves dftTwiddle null; captureSubBinBands() checks for that
  // and reports the affected bands as zero rather than faulting. A visibly wrong
  // display is the right outcome for a failed allocation - silently measuring
  // something else is not.
  if (dftTwiddle != nullptr) {
    for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
      if (!bandSubBin[k]) continue;
      const float span = bandEdgeHi[k] - bandEdgeLo[k];
      for (int j = 0; j < bandNpts[k]; j++) {
        // Same grid the trapezoid walk in captureSubBinBands() steps over, and
        // the same endpoints it gives half weight to.
        const float fHz = bandEdgeLo[k] + span * (float)j / (float)(bandNpts[k] - 1);
        const double step = 2.0 * M_PI * (double)fHz / (double)SAMPLE_RATE;
        float *tw = dftTwiddle + (size_t)(dftOffset[k] + j) * samplesFFT * 2;
        // Computed in double, stored as float. The storage error is ~6e-8 per
        // sample and the transform sums 2048 of them, so it lands near 4e-6
        // relative - against a band estimate whose own single-frame variance is
        // tens of percent. Precision is not the question in this loop.
        for (int n = 0; n < samplesFFT; n++) {
          tw[2*n]     = (float)(cos(step * n));
          tw[2*n + 1] = (float)(sin(step * n));
        }
      }
    }
  }

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
// This is the whole of what dftPowerAt() was, minus the arithmetic. The
// frequency is no longer a parameter - it is baked into the table by
// initBandLayout() - and with it goes the double-precision rotation recurrence
// that cost 23.10 ms of a 24.73 ms frame. What is left is a float
// multiply-accumulate, which is a native FPU instruction, and there is no drift
// to guard against because no state is carried between samples.
//
// Sign convention is unchanged: the twiddles advance with +step, not -step. The
// magnitude |re + i*im| is identical either way, and keeping the same sign means
// the table reproduces the old function's output rather than its conjugate.
static float dftPowerAt(const float *x, const float *tw) {
  float re = 0.0f, im = 0.0f;
  for (int n = 0; n < samplesFFT; n++) {
    const float v = x[n];
    re += v * tw[2*n];
    im += v * tw[2*n + 1];
  }
  return re * re + im * im;
}

// Band energy for the bands too narrow to read off the FFT, by integrating the
// squared DFT magnitude across the band.
//
// The integrand is sampled at bandNpts points and integrated by the trapezoid
// rule. A single sample at the band centre - which is what this used to do -
// measures a spectral DENSITY, not a band power, so it reads high for exactly
// the bands that are narrow relative to a bin: at samplesFFT=512 band 0 is
// 16.8 Hz wide against a 43.07 Hz bin, and a density over-reads a true band
// power by sqrt(binWidth/width) = 1.6x. Worse, that error moves with
// samplesFFT, which is what broke the pipeline when the length changed.
// Integrating across the band makes the answer independent of the bin grid.
//
// The sample frequencies are not recomputed here - initBandLayout() already laid
// the same points out in the same order when it filled dftTwiddle[], so j
// indexes straight into the table and the two cannot drift apart.
//
// Must run while vReal still holds the filtered, windowed time-domain signal -
// i.e. just before the FFT overwrites it with the spectrum.
static void captureSubBinBands() {
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    if (!bandSubBin[k]) { subBinMag[k] = 0.0f; continue; }
    if (dftTwiddle == nullptr) { subBinMag[k] = 0.0f; continue; }  // see initBandLayout()
    const int np = bandNpts[k];
    const float span = bandEdgeHi[k] - bandEdgeLo[k];
    const double inv = (double)(np - 1);
    double integral = 0.0;
    for (int j = 0; j < np; j++) {
      // trapezoid: half weight on the two endpoints
      const double w = (j == 0 || j == np - 1) ? 0.5 : 1.0;
      const float *tw = dftTwiddle + (size_t)(dftOffset[k] + j) * samplesFFT * 2;
      integral += w * dftPowerAt(vReal, tw);
    }
    integral *= span / inv;                                    // Hz per sample step
    // bandPowerScale puts this in the same units as fftBandEnergy(); both then
    // carry FFT_BIN_SCALE as the shared display scale.
    subBinMag[k] = FFT_BIN_SCALE * bandPowerScale * (float)sqrt(integral);
  }
}

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

// Fill the "Auto" profile: 1/|H_chain| for the cascade this build runs ahead of
// the FFT. Flat input in, flat bars out - which is the whole point of a
// calibration profile.
//
// For bands read off the FFT the correction is averaged over the band's own
// bins rather than sampled at its centre, because |H| varies across a wide band
// (band 15 is 114 bins / 4.9 kHz) and a centre sample leaves the top of the
// spectrum several dB out. Bands measured by direct DFT are narrower than a bin
// and |H| is effectively constant across them, so the centre value is exact.
static void initDerivedProfile() {
  // Layout first: the LPF corner is derived from it.
  if (!bandLayoutReady) initBandLayout();
  dsps_biquad_gen_lpf_f32(coeffs_lpf, inputLpfHz / SAMPLE_RATE, INPUT_FILTER_Q);
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    float h;
    if (bandSubBin[k]) {
      h = 1.0f;
      if ((useInputFilter > 0) && (useInputFilter < 99)) h *= dcBlockerMagAt(bandCentre[k], DC_BLOCKER_R);
      if (TROYHACKS_LPF) h *= biquadMagAt(coeffs_lpf, bandCentre[k]);
    } else {
      double sum = 0.0;
      const int n = bandHi[k] - bandLo[k] + 1;
      for (int i = bandLo[k]; i <= bandHi[k]; i++) {
        float hi = 1.0f;
        if ((useInputFilter > 0) && (useInputFilter < 99)) hi *= dcBlockerMagAt((float)i * binWidth, DC_BLOCKER_R);
        if (TROYHACKS_LPF) hi *= biquadMagAt(coeffs_lpf, (float)i * binWidth);
        sum += (double)hi * (double)hi;
      }
      h = sqrtf((float)(sum / n));
    }
    // Clamp: bands below the DC blocker's corner sit where 1/H blows up, and
    // amplifying that far would just lift the noise floor instead of correcting.
    derivedPink[k] = (h > 0.02f) ? (1.0f / h) : 50.0f;
  }
}

// Psychoacoustic band combining: 16 constant-Q bands, log-spaced from 20 Hz to
// Nyquist, reporting the energy in each. See initBandLayout() for the layout and
// fftBandEnergy() for why it is a sum and not a mean.
static void computeBandsPsychoacoustic(float* fftCalc, float wc) {
  if (!bandLayoutReady) initBandLayout();
  for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
    fftCalc[k] = bandSubBin[k]
      ? wc * subBinMag[k]                    // narrower than a bin - measured directly
      : wc * fftBandEnergy(bandLo[k], bandHi[k]);
  }
}

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

  if (vReal) free(vReal); // should not happen
  if (vImag) free(vImag); // should not happen
  #ifdef ESP32
  if ((vReal = (float*)heap_caps_calloc_prefer(sizeof(float), samplesFFT, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL)) == nullptr) return false; // calloc or die
  if ((vImag = (float*)heap_caps_calloc_prefer(sizeof(float), samplesFFT, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL)) == nullptr) return false;
  #else
  if ((vReal = (float*) calloc(sizeof(float), samplesFFT)) == nullptr) return false; // calloc or die
  if ((vImag = (float*) calloc(sizeof(float), samplesFFT)) == nullptr) return false;
  #endif
  if (pinkFactors) free(pinkFactors);
  if ((pinkFactors = (float*)heap_caps_calloc_prefer(sizeof(float), samplesFFT, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL)) == nullptr) return false;

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
static void runDCBlocker(uint_fast16_t numSamples, float *sampleBuffer) {
  const float filterR = DC_BLOCKER_R;  // -3 dB at DC_BLOCKER_HZ, an octave below the bottom band
  static float xm1 = 0.0f;
  static SR_HIRES_TYPE ym1 = 0.0f;

  for (unsigned i=0; i < numSamples; i++) {
    float value = sampleBuffer[i];
    SR_HIRES_TYPE filtered = (SR_HIRES_TYPE)(value-xm1) + filterR*ym1;
    xm1 = value;
    ym1 = filtered;
    sampleBuffer[i] = filtered;
  }
}

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
  const TickType_t xFrequency = FFT_MIN_CYCLE * portTICK_PERIOD_MS;  
  const TickType_t xFrequencyDouble = FFT_MIN_CYCLE * portTICK_PERIOD_MS * 2;  
  static bool isFirstRun = false;

  static float* oldSamples = nullptr; // previous 50% of samples
  static bool haveOldSamples = false; // for sliding window FFT
  bool usingOldSamples = false;
  if (!oldSamples) oldSamples = (float*) calloc(sizeof(float), retainSamples); // allocate on first run
  if (!oldSamples) { disableSoundProcessing = true; return; }                 // no memory -> die

  bool success = true;
  if ((vReal == nullptr) || (vImag == nullptr)) success = alocateFFTBuffers(); // allocate sample buffers on first run
  if (success == false) { disableSoundProcessing = true; return; }             // no memory -> die

  // create FFT object - we have to do if after allocating buffers
#if defined(FFT_LIB_REV) && FFT_LIB_REV > 0x19
  // arduinoFFT 2.x has a slightly different API
  static ArduinoFFT<float> FFT = ArduinoFFT<float>( vReal, vImag, samplesFFT, SAMPLE_RATE, true);
#else
  // recommended version optimized by @softhack007 (API version 1.9)
  #if defined(WLED_ENABLE_HUB75MATRIX) && defined(CONFIG_IDF_TARGET_ESP32)
    static float* windowWeighingFactors = nullptr;
    if (!windowWeighingFactors) windowWeighingFactors = (float*) calloc(sizeof(float), samplesFFT); // cache for FFT windowing factors - use heap
  #else
    static float windowWeighingFactors[samplesFFT] = {0.0f};                                        // cache for FFT windowing factors - use global RAM
  #endif
#endif

  // pre-compute pink noise scaling table
  for(uint_fast16_t binInd = 0; binInd < samplesFFT; binInd++) {
    float binFreq = binInd * binWidth + binWidth/2.0f;
    if (binFreq > (SAMPLE_RATE * 0.42f))
      binFreq = (SAMPLE_RATE * 0.42f) - 0.25 * (binFreq - (SAMPLE_RATE * 0.42f)); // suppress noise and aliasing 
    pinkFactors[binInd] = sqrtf(binFreq) / pinkcenter;
  }
  // NB: no blanket suppression of bin 0 - band 0 now spans bins 0+1, so
  // real 0-43 Hz content is wanted there. Residual DC is handled in the FFT
  // task by clamping bin 0 to the level of bin 1.

  esp_err_t myerr = dsps_fft4r_init_fc32(NULL, samplesFFT >> 1);
  if (myerr  != ESP_OK) {
      USER_PRINTF("Not possible to initialize FFT. Error = %i", myerr);
      return;
  }
  __attribute__((aligned(16))) float window[samplesFFT];
  dsps_wind_blackman_harris_f32(window, samplesFFT);

  // These two are generated rather than hand-pasted so they track SAMPLE_RATE.
  // At 22050 Hz / Q 0.734 it reproduces the old earlevel.com block equations
  // exactly, so the tuning is unchanged. This is a noise filter, not a
  // correction - the "Auto" pink profile inverts its response exactly, so no
  // accuracy is lost.

  // lowpass, inputLpfHz, INPUT_FILTER_Q - file scope so the derived pink
  // profile can evaluate the very same filter (see initDerivedProfile)
  initDerivedProfile();
  float w_lpf[5] = {0, 0};

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
    delay(1);           // DO NOT DELETE THIS LINE! It is needed to give the IDLE(0) task enough time and to keep the watchdog happy.
                        // taskYIELD(), yield(), vTaskDelay() and esp_task_wdt_feed() didn't seem to work.

    // Don't run FFT computing code if we're in Receive mode or in realtime mode
    if (disableSoundProcessing || (audioSyncEnabled == AUDIOSYNC_REC)) {
      isFirstRun = false;
      haveOldSamples = false;
      vTaskDelayUntil( &xLastWakeTime, xFrequency);        // release CPU, and let I2S fill its buffers
      continue;
    }

    // timing
    uint64_t start = esp_timer_get_time();
    bool haveDoneFFT = false; // indicates if second measurement (FFT time) is valid
    static uint64_t lastCycleStart = 0;
    static uint64_t lastLastTime = 0;
    if ((lastCycleStart > 0) && (lastCycleStart < start)) { // filter out overflows
      uint64_t taskTimeInMillis = ((start - lastCycleStart) +5ULL) / 10ULL; // "+5" to ensure proper rounding
      fftTaskCycle = (((taskTimeInMillis + lastLastTime)/2) *4 + fftTaskCycle*6)/10.0; // smart smooth
      lastLastTime = taskTimeInMillis;
    }
    lastCycleStart = start;

    // get a fresh batch of samples from I2S
    memset(vReal, 0, sizeof(float) * samplesFFT); // start clean

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
    // It is excluded from the CPU budget for that reason; see fftBudgetVerdict().
    if (start < esp_timer_get_time()) { // filter out overflows
      uint64_t sampleTimeInMillis = (esp_timer_get_time() - start +5ULL) / 10ULL; // "+5" to ensure proper rounding
      sampleTime = (sampleTimeInMillis*3 + sampleTime*7)/10.0; // smooth
    }
    start = esp_timer_get_time(); // start measuring filter time

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
      switch(useInputFilter) {
        case 1: runMicFilter(samplesFFT, vReal); break;                   // PDM microphone bandpass
        case 2: runDCBlocker(samplesFFT, vReal); break;                   // generic Low-Cut + DC blocker (~40hz) - default for line-in
      }
    }

    // timing measurement
    if (start < esp_timer_get_time()) { // filter out overflows
      uint64_t filterTimeInMillis = (esp_timer_get_time() - start +5ULL) / 10ULL; // "+5" to ensure proper rounding
      filterTime = (filterTimeInMillis*3 + filterTime*7)/10.0; // smooth
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
    float dWindow = 0.0f, dBandDft = 0.0f, dFft = 0.0f, dPost = 0.0f, dFinal = 0.0f;
    auto probe = [&tMark](float &out) {
      const uint64_t now = esp_timer_get_time();
      out = (float)((now - tMark + 5ULL) / 10ULL); // "+5" to ensure proper rounding
      tMark = now;
    };

    // set imaginary parts to 0
    memset(vImag, 0, sizeof(float) * samplesFFT);

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
      vReal[i] *= window[i]; // FFT windowing for ESP-DSP
    }
    newZeroCrossingCount = (newZeroCrossingCount*2)/3; // reduce value so it typically stays below 256
    zeroCrossingCount = newZeroCrossingCount; // update only once, to avoid that effects pick up an intermediate value
    probe(dWindow);

    // release highest sample to volume reactive effects early - not strictly necessary here - could also be done at the end of the function
    // early release allows the filters (getSample() and agcAvg()) to work with fresh values - we will have matching gain and noise gate values when we want to process the FFT results.
    micDataReal = maxSample;
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

    if (fabsf(volumeSmth) > 0.25f) { // noise gate open
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

          if (TROYHACKS_LPF) {
            dsps_biquad_f32(vReal, vImag, samplesFFT, coeffs_lpf, w_lpf); // you can't dump this back into itself, needs a destination
            memcpy(vReal, vImag, samplesFFT); // dump it back
          }

          // Bands narrower than one FFT bin cannot be recovered from the
          // spectrum - measure them here, while vReal still holds the filtered
          // time-domain signal. Must run before the FFT overwrites it.
          captureSubBinBands();
          probe(dBandDft);

          dsps_fft4r_fc32(vReal,samplesFFT >> 1);
          probe(dFft);
          dsps_bit_rev4r_fc32(vReal,samplesFFT >> 1);
          dsps_cplx2real_fc32(vReal,samplesFFT >> 1);

          // dsps_cplx2real_fc32() does NOT return magnitudes. It leaves the
          // spectrum as INTERLEAVED real/imaginary pairs:
          //   vReal[2k]   = Re(bin k)
          //   vReal[2k+1] = Im(bin k)
          // so vReal[1] is Im(DC) - mathematically 0 - and reading the array
          // as if it held magnitudes makes band 0 dead. Fold each pair into a
          // true magnitude in place (forward is safe: we write index i while
          // reading 2i and 2i+1, both >= i).
          const int halfN = samplesFFT >> 1;
          for (int i = 0; i < halfN; i++) {
            float re = vReal[2*i];
            float im = vReal[2*i+1];
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
              FFT_MajorPeak = i * (SAMPLE_RATE / samplesFFT);
            }
          }

        // scale FFT results
        for(uint_fast16_t binInd = 0; binInd < samplesFFT; binInd++)
          vReal[binInd] *= pinkFactors[binInd];

        FFT_Magnitude *= wc;  // apply correction factor

        if (FFT_MajorPeak < (SAMPLE_RATE /  samplesFFT)) {FFT_MajorPeak = 1.0f; FFT_Magnitude = 0;}                  // too low - use zero
        if (FFT_MajorPeak > (0.42f * SAMPLE_RATE)) {FFT_MajorPeak = last_majorpeak; FFT_Magnitude = last_magnitude;} // too high - keep last peak

        // undo scaling - we want unmodified values for FFTResult[] computations
        for(uint_fast16_t binInd = 0; binInd < samplesFFT; binInd++)
          vReal[binInd] *= 1.0f/pinkFactors[binInd];
        //fix peak magnitude
        if ((FFT_MajorPeak > (binWidth/1.25f)) && (FFT_MajorPeak < (SAMPLE_RATE/2.2f)) && (FFT_Magnitude > 4.0f)) {
          unsigned peakBin = constrain((int)((FFT_MajorPeak + binWidth/2.0f) / binWidth), 0, samplesFFT -1);
          FFT_Magnitude *= fmaxf(1.0f/pinkFactors[peakBin], 1.0f);
        }
        probe(dPost);

        FFT_MajorPeak = constrain(FFT_MajorPeak, 1.0f, 11025.0f);   // restrict value to range expected by effects
        FFT_MajPeakSmth = FFT_MajPeakSmth + 0.42 * (FFT_MajorPeak - FFT_MajPeakSmth);   // I like this "swooping peak" look

      } else { // skip second run --> clear fft results, keep peaks
        memset(vReal, 0, sizeof(float) * samplesFFT);
        // The sub-bin bands were captured before the FFT, so they need clearing
        // too or they would report a stale frame the spectrum has just zeroed.
        memset(subBinMag, 0, sizeof(subBinMag));
      }

      haveDoneFFT = true;

    } else { // noise gate closed - only clear results as FFT was skipped. MIC samples are still valid when we do this.
      memset(vReal, 0, sizeof(float) * samplesFFT);
      FFT_MajorPeak = 1;
      FFT_Magnitude = 0.001;
    }

    if ((skipSecondFFT == false) || (isFirstRun == true)) {
      for (int i = 0; i < samplesFFT; i++) {
        float t = fabsf(vReal[i]);                      // just to be sure - values in fft bins should be positive any way
        vReal[i] = t * FFT_BIN_SCALE;                   // Reduce magnitude. Want end result to be scaled linear and ~4096 max.
      } // for()

      // mapping of FFT result bins to frequency channels
      //if (fabsf(sampleAvg) > 0.25f) { // noise gate open
      if (fabsf(volumeSmth) > 0.25f) { // noise gate open
        //WLEDMM: different distributions
        if (freqDist == 2) {
          // Psychoacoustic logarithmic band combining (1/1 octave per
          // channel by default). Adapts to any SAMPLE_RATE / samplesFFT.
          // For PDM mics (useInputFilter == 1) skip sub-bass: start at
          // 100 Hz instead of 30 Hz to avoid bin 0 (DC) and low-freq
          // noise. Built-in damping handles the rest.
          computeBandsPsychoacoustic(fftCalc, wc);
        } else if (freqDist == 0) {
          /* new mapping, optimized for 22050 Hz by softhack007 --- update: removed overlap */
                                                         // bins frequency  range
          if (useInputFilter==1) {
            // skip frequencies below 100hz
            fftCalc[ 0] = wc * 0.8f * fftAddAvg(3,3);
            fftCalc[ 1] = wc * 0.9f * fftAddAvg(4,4);
            fftCalc[ 2] = wc * fftAddAvg(5,5);
            fftCalc[ 3] = wc * fftAddAvg(6,6);
            // don't use the last bins from 206 to 255.
            fftCalc[15] = wc * fftAddAvg(165,205) * 0.75f;   // 40 7106 - 8828 high             -- with some damping
          } else {
            fftCalc[ 0] = wc * fftAddAvg(1,1);               // 1    43 - 86   sub-bass
            fftCalc[ 1] = wc * fftAddAvg(2,2);               // 1    86 - 129  bass
            fftCalc[ 2] = wc * fftAddAvg(3,4);               // 2   129 - 216  bass
            fftCalc[ 3] = wc * fftAddAvg(5,6);               // 2   216 - 301  bass + midrange
            // don't use the last bins from 216 to 255. They are usually contaminated by aliasing (aka noise)
            fftCalc[15] = wc * fftAddAvg(165,215) * 0.70f;   // 50 7106 - 9259 high             -- with some damping
          }
          fftCalc[ 4] = wc * fftAddAvg(7,9);                 // 3   301 - 430  midrange
          fftCalc[ 5] = wc * fftAddAvg(10,12);               // 3   430 - 560  midrange
          fftCalc[ 6] = wc * fftAddAvg(13,18);               // 5   560 - 818  midrange
          fftCalc[ 7] = wc * fftAddAvg(19,25);               // 7   818 - 1120 midrange -- 1Khz should always be the center !
          fftCalc[ 8] = wc * fftAddAvg(26,32);               // 7  1120 - 1421 midrange
          fftCalc[ 9] = wc * fftAddAvg(33,43);               // 9  1421 - 1895 midrange
          fftCalc[10] = wc * fftAddAvg(44,55);               // 12 1895 - 2412 midrange + high mid
          fftCalc[11] = wc * fftAddAvg(56,69);               // 14 2412 - 3015 high mid
          fftCalc[12] = wc * fftAddAvg(70,85);               // 16 3015 - 3704 high mid
          fftCalc[13] = wc * fftAddAvg(86,103);              // 18 3704 - 4479 high mid
          fftCalc[14] = wc * fftAddAvg(104,164) * 0.88f;     // 61 4479 - 7106 high mid + high  -- with slight damping
        } else { // freqDist == 1: WLEDMM Rightshift
          if (useInputFilter==1) {
            // skip frequencies below 100hz
            fftCalc[ 0] = wc * 0.8f * fftAddAvg(1,1);
            fftCalc[ 1] = wc * 0.9f * fftAddAvg(2,2);
            fftCalc[ 2] = wc * fftAddAvg(3,3);
            fftCalc[ 3] = wc * fftAddAvg(4,4);
            // don't use the last bins from 206 to 255.
            fftCalc[15] = wc * fftAddAvg(165,205) * 0.75f;   // 40 7106 - 8828 high             -- with some damping
          } else {
            fftCalc[ 0] = wc * fftAddAvg(1,1);               // 1    43 - 86   sub-bass
            fftCalc[ 1] = wc * fftAddAvg(2,2);               // 1    86 - 129  bass
            fftCalc[ 2] = wc * fftAddAvg(3,3);               // 2   129 - 216  bass
            fftCalc[ 3] = wc * fftAddAvg(4,4);               // 2   216 - 301  bass + midrange
            // don't use the last bins from 216 to 255. They are usually contaminated by aliasing (aka noise)
            fftCalc[15] = wc * fftAddAvg(165,215) * 0.70f;   // 50 7106 - 9259 high             -- with some damping
          }
          fftCalc[ 4] = wc * fftAddAvg(5,6);                 // 3   301 - 430  midrange
          fftCalc[ 5] = wc * fftAddAvg(7,8);                 // 3   430 - 560  midrange
          fftCalc[ 6] = wc * fftAddAvg(9,10);                // 5   560 - 818  midrange
          fftCalc[ 7] = wc * fftAddAvg(11,13);               // 7   818 - 1120 midrange -- 1Khz should always be the center !
          fftCalc[ 8] = wc * fftAddAvg(14,18);               // 7  1120 - 1421 midrange
          fftCalc[ 9] = wc * fftAddAvg(19,25);               // 9  1421 - 1895 midrange
          fftCalc[10] = wc * fftAddAvg(26,36);               // 12 1895 - 2412 midrange + high mid
          fftCalc[11] = wc * fftAddAvg(37,45);               // 14 2412 - 3015 high mid
          fftCalc[12] = wc * fftAddAvg(46,66);               // 16 3015 - 3704 high mid
          fftCalc[13] = wc * fftAddAvg(67,97);               // 18 3704 - 4479 high mid
          fftCalc[14] = wc * fftAddAvg(98,164) * 0.88f;      // 61 4479 - 7106 high mid + high  -- with slight damping
        }
    } else {  // noise gate closed - just decay old values
      isFirstRun = false;
      for (int i=0; i < NUM_GEQ_CHANNELS; i++) {
        fftCalc[i] *= 0.85f;  // decay to zero
        if (fftCalc[i] < 4.0f) fftCalc[i] = 0.0f;
    } }

      memcpy(lastFftCalc, fftCalc, sizeof(lastFftCalc)); // make a backup of last "good" channels

      // Per-band slow-decay AGC (useAutoLevel = 1):
      // Each band tracks its own peak (with slow decay) and the per-band
      // auto-gain is reduced whenever the band exceeds the target peak.
      // This makes the loudest band fill the display regardless of how
      // 1/f-shaped or quiet the input is. Pure 1/f sources (pink noise)
      // end up roughly flat in the display because each band's gain
      // settles independently. No user action required.
      if (useAutoLevel) {
        const float BAND_TARGET_PEAK = 4096.0f;        // the user-noted "max" magnitude
        const float BAND_DECAY       = 0.9999f;         // slow decay of the peak tracker
        const float GAIN_DECAY       = 0.99995f;        // slower decay of the auto-gain
        if (!bandAutoGainInited) {
          for (int k = 0; k < NUM_GEQ_CHANNELS; k++) bandAutoGain[k] = 1.0f;
          bandAutoGainInited = true;
        }
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
          // Track the peak of (raw_fft * current_gain) using slow decay
          float rawPeak = fftCalc[k] * bandAutoGain[k];
          static float bandPeak[NUM_GEQ_CHANNELS];
          bandPeak[k] = (rawPeak > bandPeak[k])
                          ? rawPeak
                          : (bandPeak[k] * BAND_DECAY);
          if (bandPeak[k] > BAND_TARGET_PEAK && bandPeak[k] > 0.001f) {
            // Reduce this band's gain so the peak sits at the target
            bandAutoGain[k] = BAND_TARGET_PEAK / bandPeak[k];
          } else {
            // Slow recovery toward unity gain when band is below target
            bandAutoGain[k] = bandAutoGain[k] * GAIN_DECAY + (1.0f - GAIN_DECAY);
            if (bandAutoGain[k] > 1.0f) bandAutoGain[k] = 1.0f;  // never boost beyond unity
          }
          fftCalc[k] *= bandAutoGain[k];
        }
      }

      // #4 Noise floor subtraction: track a slow minimum per band, subtract
      // it so quiet content isn't masked by the mic's noise floor. Helps
      // when each band has a different residual noise level.
      if (useNoiseFloorSub) {
        static float bandFloor[NUM_GEQ_CHANNELS] = {0.0f};
        static bool  bandFloorInited = false;
        const float FLOOR_DECAY = 0.9999f;
        const float FLOOR_SMOOTH = 0.001f;
        if (!bandFloorInited) {
          for (int k = 0; k < NUM_GEQ_CHANNELS; k++) bandFloor[k] = 0.0f;
          bandFloorInited = true;
        }
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
          // Track the slow minimum, then push the floor up slowly so
          // transient dips don't get locked in.
          bandFloor[k] = (fftCalc[k] < bandFloor[k])
                          ? fftCalc[k]
                          : (bandFloor[k] * FLOOR_DECAY + fftCalc[k] * FLOOR_SMOOTH);
          float v = fftCalc[k] - bandFloor[k];
          if (v < 0.0f) v = 0.0f;
          fftCalc[k] = v;
        }
      }

      // #3 Histogram-based display gain: pick a single global gain that
      // makes the loudest visible band hit ~85% of the display, regardless
      // of how loud the source is. Periodically updated.
      if (useHistogramNorm && !useAutoLevel) {
        // Skip if AGC is on — AGC already does per-band equalization.
        static float histMax = 1.0f;
        const float HIST_DECAY = 0.999f;
        const float HIST_TARGET = 0.85f * 4096.0f;
        float curMax = 0.0f;
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++)
          if (fftCalc[k] > curMax) curMax = fftCalc[k];
        if (curMax > 0.001f) {
          if (curMax > HIST_TARGET) {
            histMax = HIST_TARGET / curMax;
          } else {
            histMax = histMax * HIST_DECAY + (1.0f - HIST_DECAY);
            if (histMax > 1.0f) histMax = 1.0f;
          }
          for (int k = 0; k < NUM_GEQ_CHANNELS; k++)
            fftCalc[k] *= histMax;
        }
      }

      // #5 Per-band log compression (soft-knee).
      // Apply log(1 + x) to soften peaks without boosting the floor.
      // Map [0, Xmax] -> [0, Xmax] via log(1+x)/log(1+Xmax) so peaks get
      // compressed but small signals pass through nearly linearly.
      // For k=4 / Xmax=4096: x=50 -> 1230, x=200 -> 2115, x=1000 -> 3123.
      if (useBandCompress) {
        const float Xmax = 4096.0f;
        const float logXmax = logf(1.0f + Xmax);
        for (int k = 0; k < NUM_GEQ_CHANNELS; k++) {
          if (fftCalc[k] > 0.0f) {
            // Use Xmax per-band so the curve normalises at the band's
            // current peak. This compresses within-band transients while
            // preserving per-band equalisation from the AGC step.
            float bandMax = fftCalc[k];
            for (int j = 0; j < 8; j++) {
              if (bandMax > 0.0f) break;
              bandMax = 0.0f;
            }
            // Apply log(1+x)/log(1+bandMax) — noop if bandMax <= 1
            if (bandMax > 1.0f) {
              float logBandMax = logf(1.0f + bandMax);
              fftCalc[k] = (logf(1.0f + fftCalc[k]) / logBandMax) * bandMax;
            }
          }
        }
      }

      // "Dance music fixer" — soft lift on band 0 from band 1. Compensates
      // for mics with poor sub-bass response (e.g. the ESP32-P4 EV board's
      // onboard mic) by adding a fraction of band 1 to band 0 when band 1
      // is non-zero. Band 0 keeps its own shape; bands stay distinct.
      if (useBand0Lifter && NUM_GEQ_CHANNELS >= 2 && fftCalc[1] > 0.0f) {
        const float α = 0.15f;  // 15% coupling — adds without matching
        fftCalc[0] += α * fftCalc[1];
      }

    } else { // if second run skipped
      memcpy(fftCalc, lastFftCalc, sizeof(fftCalc)); // restore last "good" channels
    }

    // post-processing of frequency channels (pink noise adjustment, AGC, smoothing, scaling)
    if (pinkIndex > MAX_PINK) pinkIndex = MAX_PINK;

    postProcessFFTResults((fabsf(volumeSmth) > 0.25f)? true : false, NUM_GEQ_CHANNELS, usingOldSamples);    // this function modifies fftCalc, fftAvg and fftResult
    probe(dFinal);

    // timing
    static uint64_t lastLastFFT = 0;
    if (haveDoneFFT && (start < esp_timer_get_time())) { // filter out overflows
      uint64_t fftTimeInMillis = ((esp_timer_get_time() - start) +5ULL) / 10ULL; // "+5" to ensure proper rounding
      fftTime  = (((fftTimeInMillis + lastLastFFT)/2) *3 + fftTime*7)/10.0; // smart smooth
      lastLastFFT = fftTimeInMillis;
      // Stage split, same smoothing as fftTime. A frame that skipped the FFT
      // (noise gate closed, or skipSecondFFT) leaves the un-probed stages at
      // zero, which is honest - that work genuinely did not happen - and the
      // smoothing keeps a few skipped frames from dominating the average.
      tWindow = (dWindow *3 + tWindow *7)/10.0;
      tBandDft= (dBandDft*3 + tBandDft*7)/10.0;
      tFft    = (dFft    *3 + tFft    *7)/10.0;
      tPost   = (dPost   *3 + tPost   *7)/10.0;
      tFinal  = (dFinal  *3 + tFinal  *7)/10.0;
    }

    // run peak detection
    autoResetPeak();
    detectSamplePeak();

    haveNewFFTResult = true;
    
    #if !defined(I2S_GRAB_ADC1_COMPLETELY)    
    if ((audioSource == nullptr) || (audioSource->getType() != AudioSource::Type_I2SAdc))  // the "delay trick" does not help for analog ADC
    #endif
    {
      if (!usingOldSamples) {
        vTaskDelayUntil( &xLastWakeTime, xFrequencyDouble); // we need a double wait when no old data was used
      } else if ((skipSecondFFT == false) || (fabsf(volumeSmth) < 0.25f)) {
        vTaskDelayUntil( &xLastWakeTime, xFrequency);        // release CPU, and let I2S fill its buffers
      } else if (isFirstRun == true) {
        vTaskDelayUntil( &xLastWakeTime, xFrequencyDouble);  // release CPU after performing FFT in "skip second run" mode
      }
    }
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

static void postProcessFFTResults(bool noiseGateOpen, int numberOfChannels, bool i2sFastpath) // post-processing and post-amp of GEQ channels
{
    for (int i=0; i < numberOfChannels; i++) {

      if (noiseGateOpen) { // noise gate open
        
        // Some attempt to auto-calibrate mics
        if (TROYHACKS_PINKY) {
          float calced = fftCalc[i] * FFT_DOWNSCALE * (soundAgc ? multAgc : ((float)sampleGain/40.0f * (float)inputLevel/128.0f + 1.0f/16.0f));
          fftBinAverage[i] = fftBinAverage[i] * 0.99 + (0.01 * calced);
          // if (calced > fftBinAverage[i]) fftBinAverage[i] = calced;
        }
        // Adjustment for frequency curves.
        if (fftBinAverage[0] != 0 && !TROYHACKS_PINKY) {
          fftCalc[i] *= fftBinAverage[i];
        } else {
          // if we aren't calibrat[ing/ed], use the menu choice
          fftCalc[i] *= (pinkIndex == PINK_AUTO) ? derivedPink[i] : fftResultPink[pinkIndex][i];
        }
        // End auto calibration

        if (FFTScalingMode > 0) fftCalc[i] *= FFT_DOWNSCALE;  // adjustment related to FFT windowing function
        // Manual linear adjustment of gain using sampleGain adjustment for different input types.
        fftCalc[i] *= soundAgc ? multAgc : ((float)sampleGain/40.0f * (float)inputLevel/128.0f + 1.0f/16.0f); //apply gain, with inputLevel adjustment
        if(fftCalc[i] < 0) fftCalc[i] = 0;
      }

      float speed = 1.0f;  // filter correction for sampling speed ->  1.0 in normal mode (43hz)
      if (i2sFastpath) speed = 0.6931471805599453094f * 1.1f;  //  ->  ln(2) from math, *1.1 from my gut feeling ;-) in fast mode (86hz)

      if(limiterOn == true) {
        // Limiter ON -> smooth results
        if(fftCalc[i] > fftAvg[i]) {  // rise fast
          fftAvg[i] += speed * 0.78f * (fftCalc[i] - fftAvg[i]);  // will need approx 1-2 cycles (50ms) for converging against fftCalc[i]
        } else {                       // fall slow
          if (decayTime < 150)       fftAvg[i] += speed * 0.50f * (fftCalc[i] - fftAvg[i]); 
          else if (decayTime < 250)  fftAvg[i] += speed * 0.40f * (fftCalc[i] - fftAvg[i]); 
          else if (decayTime < 500)  fftAvg[i] += speed * 0.33f * (fftCalc[i] - fftAvg[i]); 
          else if (decayTime < 1000) fftAvg[i] += speed * 0.22f * (fftCalc[i] - fftAvg[i]);  // approx  5 cycles (225ms) for falling to zero
          else if (decayTime < 2000) fftAvg[i] += speed * 0.17f * (fftCalc[i] - fftAvg[i]);  // default - approx  9 cycles (225ms) for falling to zero
          else if (decayTime < 3000) fftAvg[i] += speed * 0.14f * (fftCalc[i] - fftAvg[i]);  // approx 14 cycles (350ms) for falling to zero
          else if (decayTime < 4000) fftAvg[i] += speed * 0.10f * (fftCalc[i] - fftAvg[i]);
          else fftAvg[i] += speed * 0.05f * (fftCalc[i] - fftAvg[i]);
        }
      } else {
        // Limiter OFF
        if (i2sFastpath) { 
          // fast mode -> average last two results
          float tmp = fftCalc[i];
          fftCalc[i] = 0.7f * tmp + 0.3f * fftAvg[i];
          fftAvg[i] = tmp; // store current sample for next run
        } else {
          // normal mode -> no adjustments
          fftAvg[i] = fftCalc[i]; // keep filters up-to-date
        }
      }

      // constrain internal vars - just to be sure
      fftCalc[i] = constrain(fftCalc[i], 0.0f, 1023.0f);
      fftAvg[i] = constrain(fftAvg[i], 0.0f, 1023.0f);

      float currentResult = limiterOn ? fftAvg[i] : fftCalc[i]; // continue with filtered result (limiter on) or unfiltered result (limiter off)

      switch (FFTScalingMode) {
        case 1:
            // Logarithmic scaling
            currentResult *= 0.42;                      // 42 is the answer ;-)
            currentResult -= 8.0;                       // this skips the lowest row, giving some room for peaks
            if (currentResult > 1.0) currentResult = logf(currentResult); // log to base "e", which is the fastest log() function
            else currentResult = 0.0;                   // special handling, because log(1) = 0; log(0) = undefined
            currentResult *= 0.85f + (float(i)/18.0f);  // extra up-scaling for high frequencies
            currentResult = mapf(currentResult, 0, LOG_256, 0, 255); // map [log(1) ... log(255)] to [0 ... 255]
        break;
        case 2:
            // Linear scaling
            currentResult *= 0.30f;                     // needs a bit more damping, get stay below 255
            currentResult -= 2.0;                       // giving a bit more room for peaks
            if (currentResult < 1.0f) currentResult = 0.0f;
            currentResult *= 0.85f + (float(i)/1.8f);   // extra up-scaling for high frequencies
        break;
        case 3:
            // Loudness (dB) scaling - the fractional-octave analyser convention.
            //
            // A linear magnitude is a poor display quantity. The usable range
            // here is roughly 6..690, a factor of 115, and the old square-root
            // curve squeezed that into 16:1 - only 24 dB across the entire bar,
            // with a per-band ramp (0.85 + i/4.5) quietly restoring some of it.
            // Mapping 20*log10() over a 66 dB window is what an RTA does and
            // what the ear responds to.
            //
            // TROYHACKS_DBREF sets where 0 dB lands, i.e. how much headroom is
            // left above normal material. Too high and every band sits tall with
            // all its movement crushed into the top few pixels; too low and the
            // display never fills. Press 'd' on serial to walk it in 1 dB steps.
            //
            // There is deliberately no per-band tilt: the ramp was compensating
            // for the mean-over-bins estimator that has been replaced, so leaving
            // it in would now be correcting an artifact that no longer exists.
            // If you later want a "smiley" curve, this is the right place for it
            // - the display layer, where it is visible and tunable, rather than
            // baked into the signal path where it silently fights the
            // calibration profile.
            currentResult *= TROYHACKS_DBREF;
            if (currentResult > FFT_DB_FLOOR) {
              currentResult = 20.0f * log10f(currentResult);
              currentResult = mapf(currentResult, -TROYHACKS_DBSPAN, 0.0f, 0.0f, 255.0f);
            } else {
              currentResult = 0.0f;
            }
            if (currentResult < 0.0f) currentResult = 0.0f;
        break;

        case 0:
        default:
            // no scaling - leave freq bins as-is
            currentResult -= 2; // just a bit more room for peaks
        break;
      }

      // Now, let's dump it all into fftResult. Need to do this, otherwise other routines might grab fftResult values prematurely.
      if (soundAgc > 0) {  // apply extra "GEQ Gain" if set by user
        float post_gain = (float)inputLevel/128.0f;
        if (post_gain < 1.0f) post_gain = ((post_gain -1.0f) * 0.8f) +1.0f;
        currentResult *= post_gain;
      }
      fftResult[i] = max(min((int)(currentResult+0.5f), 255), 0);  // +0.5 for proper rounding
    }
}
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
  if ((sampleAvg > 1) && (maxVol > 0) && (binNum > 4) && (vReal[binNum] > maxVol) && ((millis() - timeOfPeak) > 100)) {
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
// Budget verdict for the FFT task, shared by every info-page timing row so they
// cannot disagree. Three limits live here, and conflating them is what made
// FFT_MIN_CYCLE useless as a warning for years:
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
// stageMs is the individual stage being reported; totalMs is the total CPU WORK
// the task owes, which is fftTime + filterTime. It deliberately excludes
// sampleTime. That number looks like a stage but is mostly the task sitting
// Blocked on the DMA waiting for the next 256 samples to arrive - wait time,
// not compute, and time in which it burns no CPU at all.
//
// Getting this wrong is not academic. With sampleTime counted, a correctly
// paced task reads 6.75 ms of an 11.61 ms frame and trips the half-budget
// warning, while the task list shows it at 34% CPU, not 58%. The two disagreed
// because one of them was counting sleep. A warning that fires on a healthy
// system is how you learn to ignore warnings.
//
// Both arguments arrive in TIMING_UNIT_US, matching fftTime/filterTime/
// sampleTime, so the *100.0 factors below convert to milliseconds.
static const char* fftBudgetVerdict(double stageMs, double totalMs) {
  if (stageMs >= (double)I2S_RING_MS * 100.0 || totalMs >= (double)I2S_RING_MS * 100.0)
    return "<b style=\"color:red;\">! over DMA ring - audio will drop</b>";
  if (totalMs >= (double)FRAME_PERIOD_MS * 100.0)
    return "<b style=\"color:orange;\">~ update rate slipping</b>";
  if (totalMs >= (double)FRAME_PERIOD_MS * 50.0)
    return "<i>~ half the frame budget</i>";
  return " OK";
}

// Verdict for the two rows that measure PACING rather than work: the I2S cycle
// and the sample read. A correctly paced task's cycle does not come in under
// FRAME_PERIOD_MS - it sits AT it, because the task spends the remainder of the
// frame asleep and only wakes when the next audio is there. Testing it with
// `>= FRAME_PERIOD_MS` would report perfect pacing as overdue.
//
// So the only question for these rows is whether the cycle has run well PAST
// one frame, which means frames are being missed and audio is being consumed
// faster than the DMA delivers it. 1.5x leaves room for jitter without being
// reachable by a healthy task.
static const char* pacingVerdict(double cycleMs) {
  if (cycleMs >= (double)I2S_RING_MS * 100.0)
    return "<b style=\"color:red;\">! over DMA ring - audio will drop</b>";
  if (cycleMs >= (double)FRAME_PERIOD_MS * 150.0)
    return "<b style=\"color:orange;\">~ frames being missed</b>";
  return " OK";
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
    // used for AGC
    int      last_soundAgc = -1;   // used to detect AGC mode change (for resetting AGC internal error buffers)
    double   control_integrated = 0.0;   // persistent across calls to agcAvg(); "integrator control" = accumulated error

    // variables used by getSample() and agcAvg()
    double   sampleMax = 0.0;     // Max sample over a few seconds. Needed for AGC controller.
    double   micLev = 0.0;        // Used to convert returned value to have '0' as minimum. A leveller
    float    expAdjF = 0.0f;      // Used for exponential filter.
    float    sampleReal = 0.0f;	  // "sampleRaw" as float, to provide bits that are lost otherwise (before amplification by sampleGain or inputLevel). Needed for AGC.
    int16_t  sampleRaw = 0;       // Current sample. Must only be updated ONCE!!! (amplified mic value by sampleGain and inputLevel)
    int16_t  rawSampleAgc = 0;    // not smoothed AGC sample
#endif

    // variables used in effects
    int16_t  volumeRaw = 0;       // either sampleRaw or rawSampleAgc depending on soundAgc
    float syncVolumeSmth = 0;
    float my_magnitude =0.0f;     // FFT_Magnitude, scaled by multAgc
    float soundPressure = 0;      // Sound Pressure estimation, based on microphone raw readings. 0 ->5db, 255 ->105db

    // used to feed "Info" Page
    unsigned long last_UDPTime = 0;    // time of last valid UDP sound sync datapacket
    int receivedFormat = 0;            // last received UDP sound sync format - 0=none, 1=v1 (0.13.x), 2=v2 (0.14.x)
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
      PLOT_PRINT("micReal:");     PLOT_PRINT(micDataReal + 256.0f); PLOT_PRINT("\t");  // +256 to move above other lines
      PLOT_PRINT("DC_Level:");    PLOT_PRINT(micLev + 256.0f);      PLOT_PRINT("\t");  // +256 to move above other lines
      // //PLOT_PRINT("filtmicMin:");     PLOT_PRINT(0.5f * micReal_min2);  PLOT_PRINT("\t"); // scaled down to 50%
      // //PLOT_PRINT("filtmicMax:");     PLOT_PRINT(0.5f * micReal_max2);  PLOT_PRINT("\t"); // scaled down to 50%
      //PLOT_PRINT("sampleAgc:");   PLOT_PRINT(sampleAgc);   PLOT_PRINT("\t");
      //PLOT_PRINT("sampleAvg:");   PLOT_PRINT(sampleAvg);   PLOT_PRINT("\t");
      //PLOT_PRINT("sampleReal:");  PLOT_PRINT(sampleReal);  PLOT_PRINT("\t");
      //PLOT_PRINT("sample:");      PLOT_PRINT(sample);      PLOT_PRINT("\t");
      //PLOT_PRINT("sampleMax:");   PLOT_PRINT(sampleMax);   PLOT_PRINT("\t");
      //PLOT_PRINT("multAgc:");     PLOT_PRINT(multAgc, 4);  PLOT_PRINT("\t");
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

    /*
    * A "PI controller" multiplier to automatically adjust sound sensitivity.
    * 
    * A few tricks are implemented so that sampleAgc does't only utilize 0% and 100%:
    * 0. don't amplify anything below squelch (but keep previous gain)
    * 1. gain input = maximum signal observed in the last 5-10 seconds
    * 2. we use two setpoints, one at ~60%, and one at ~80% of the maximum signal
    * 3. the amplification depends on signal level:
    *    a) normal zone - very slow adjustment
    *    b) emergency zone (<10% or >90%) - very fast adjustment
    */
    void agcAvg(unsigned long the_time)
    {
      const int AGC_preset = (soundAgc > 0)? (soundAgc-1): 0; // make sure the _compiler_ knows this value will not change while we are inside the function

      float lastMultAgc = multAgc;      // last multiplier used
      float multAgcTemp = multAgc;      // new multiplier
      float tmpAgc = sampleReal * multAgc;        // what-if amplified signal

      float control_error;                        // "control error" input for PI control

      if (last_soundAgc != soundAgc)
        control_integrated = 0.0;                // new preset - reset integrator

      // For PI controller, we need to have a constant "frequency"
      // so let's make sure that the control loop is not running at insane speed
      static unsigned long last_time = 0;
      unsigned long time_now = millis();
      if ((the_time > 0) && (the_time < time_now)) time_now = the_time;  // allow caller to override my clock

      if (time_now - last_time > 2)  {
        last_time = time_now;

        if((fabsf(sampleReal) < 2.0f) || (sampleMax < 1.0f)) {
          // MIC signal is "squelched" - deliver silence
          tmpAgc = 0;
          // we need to "spin down" the intgrated error buffer
          if (fabs(control_integrated) < 0.01)  control_integrated  = 0.0;
          else                                  control_integrated *= 0.91;
        } else {
          // compute new setpoint
          if (tmpAgc <= agcTarget0Up[AGC_preset])
            multAgcTemp = agcTarget0[AGC_preset] / sampleMax;   // Make the multiplier so that sampleMax * multiplier = first setpoint
          else
            multAgcTemp = agcTarget1[AGC_preset] / sampleMax;   // Make the multiplier so that sampleMax * multiplier = second setpoint
        }
        // limit amplification
        if (multAgcTemp > 32.0f)      multAgcTemp = 32.0f;
        if (multAgcTemp < 1.0f/64.0f) multAgcTemp = 1.0f/64.0f;

        // compute error terms
        control_error = multAgcTemp - lastMultAgc;
        
        if (((multAgcTemp > 0.085f) && (multAgcTemp < 6.5f))    //integrator anti-windup by clamping
            && (multAgc*sampleMax < agcZoneStop[AGC_preset]))   //integrator ceiling (>140% of max)
          control_integrated += control_error * 0.002 * 0.25;   // 2ms = integration time; 0.25 for damping
        else
          control_integrated *= 0.9;                            // spin down that integrator beast

        // apply PI Control 
        tmpAgc = sampleReal * lastMultAgc;                      // check "zone" of the signal using previous gain
        if ((tmpAgc > agcZoneHigh[AGC_preset]) || (tmpAgc < soundSquelch + agcZoneLow[AGC_preset])) {  // upper/lower emergency zone
          multAgcTemp = lastMultAgc + agcFollowFast[AGC_preset] * agcControlKp[AGC_preset] * control_error;
          multAgcTemp += agcFollowFast[AGC_preset] * agcControlKi[AGC_preset] * control_integrated;
        } else {                                                                         // "normal zone"
          multAgcTemp = lastMultAgc + agcFollowSlow[AGC_preset] * agcControlKp[AGC_preset] * control_error;
          multAgcTemp += agcFollowSlow[AGC_preset] * agcControlKi[AGC_preset] * control_integrated;
        }

        // limit amplification again - PI controller sometimes "overshoots"
        //multAgcTemp = constrain(multAgcTemp, 0.015625f, 32.0f); // 1/64 < multAgcTemp < 32
        if (multAgcTemp > 32.0f)      multAgcTemp = 32.0f;
        if (multAgcTemp < 1.0f/64.0f) multAgcTemp = 1.0f/64.0f;
      }

      // NOW finally amplify the signal
      tmpAgc = sampleReal * multAgcTemp;                  // apply gain to signal
      if (fabsf(sampleReal) < 2.0f) tmpAgc = 0.0f;        // apply squelch threshold
      //tmpAgc = constrain(tmpAgc, 0, 255);
      if (tmpAgc > 255) tmpAgc = 255.0f;                  // limit to 8bit
      if (tmpAgc < 1)   tmpAgc = 0.0f;                    // just to be sure

      // update global vars ONCE - multAgc, sampleAGC, rawSampleAgc
      multAgc = multAgcTemp;
      if (micQuality > 0) {
        if (micQuality > 1) {
          rawSampleAgc = 0.95f * tmpAgc + 0.05f * (float)rawSampleAgc; // raw path
          sampleAgc += 0.95f * (tmpAgc - sampleAgc); // smooth path
        } else {
          rawSampleAgc = 0.70f * tmpAgc + 0.30f * (float)rawSampleAgc; // min filtering path
          sampleAgc += 0.70f * (tmpAgc - sampleAgc);
        }
      } else {
#if defined(WLEDMM_FASTPATH)
      rawSampleAgc = 0.65f * tmpAgc + 0.35f * (float)rawSampleAgc;
#else
      rawSampleAgc = 0.8f * tmpAgc + 0.2f * (float)rawSampleAgc;
#endif
      // update smoothed AGC sample
      if (fabsf(tmpAgc) < 1.0f) 
        sampleAgc =  0.5f * tmpAgc + 0.5f * sampleAgc;    // fast path to zero
      else
        sampleAgc += agcSampleSmooth[AGC_preset] * (tmpAgc - sampleAgc); // smooth path
      }
      sampleAgc = fabsf(sampleAgc);                                      // // make sure we have a positive value
      last_soundAgc = soundAgc;
    } // agcAvg()

    // post-processing and filtering of MIC sample (micDataReal) from FFTcode()
    void getSample()
    {
      float    sampleAdj;           // Gain adjusted sample value
      float    tmpSample;           // An interim sample variable used for calculations.
      const float weighting = 0.18f; // Exponential filter weighting. Will be adjustable in a future release.
      const float weighting2 = 0.073f; // Exponential filter weighting, for rising signal (a bit more robust against spikes)
      const int   AGC_preset = (soundAgc > 0)? (soundAgc-1): 0; // make sure the _compiler_ knows this value will not change while we are inside the function
      static bool isFrozen = false;
      static bool haveSilence = true;
      static unsigned long lastSoundTime = 0; // for delaying un-freeze
      static unsigned long startuptime = 0;   // "fast freeze" mode: do not interfere during first 12 seconds (filter startup time) 

      if (startuptime == 0) startuptime = millis();   // fast freeze mode - remember filter startup time
      if ((micLevelMethod < 1) || !isFrozen) {        // following the input level, UNLESS mic Level was frozen
        micLev += (micDataReal-micLev) / 12288.0f;
      }

      if(micDataReal < (micLev-0.24)) {                    // MicLev above input signal:
        micLev = ((micLev * 31.0f) + micDataReal) / 32.0f; // always align MicLev to lowest input signal
        if (!haveSilence) isFrozen = true;                 // freeze mode: freeze micLevel so it cannot rise again
      }

      // Using an exponential filter to smooth out the signal. We'll add controls for this in a future release.
      float micInNoDC = fabsf(micDataReal - micLev);

      if ((micInNoDC > expAdjF) && (expAdjF > soundSquelch))               // MicIn rising, and above squelch threshold?
        expAdjF = (weighting2 * micInNoDC + (1.0f-weighting2) * expAdjF);  // rise slower
      else
        expAdjF = (weighting * micInNoDC + (1.0f-weighting) * expAdjF);    // fall faster

      expAdjF = fabsf(expAdjF);                         // Now (!) take the absolute value

      if ((micLevelMethod == 2) && !haveSilence && (expAdjF >= (1.5f * float(soundSquelch)))) 
        isFrozen = true;    // fast freeze mode: freeze micLevel once the volume rises 50% above squelch

      // simple noise gate
      if ((expAdjF <= soundSquelch) || ((soundSquelch == 0) && (expAdjF < 0.25f))) {
        expAdjF = 0.0f;
        micInNoDC = 0.0f;
      }

      if (expAdjF <= 0.5f) 
        haveSilence = true;
      else {
        lastSoundTime = millis();
        haveSilence = false;
      }

      // un-freeze micLev
      if  (micLevelMethod == 0) isFrozen = false;
      if ((micLevelMethod == 1) && isFrozen && haveSilence && ((millis() - lastSoundTime) > 4000)) isFrozen = false;  // normal freeze: 4 seconds silence needed
      if ((micLevelMethod == 2) && isFrozen && haveSilence && ((millis() - lastSoundTime) > 6000)) isFrozen = false;  // fast freeze: 6 seconds silence needed
      if ((micLevelMethod == 2) && (millis() - startuptime < 12000)) isFrozen = false;   // fast freeze: no freeze in first 12 seconds (filter startup phase)

      tmpSample = expAdjF;

      // Adjust the gain. with inputLevel adjustment.
      if (micQuality > 0) {
        sampleAdj = micInNoDC * sampleGain / 40.0f * inputLevel/128.0f + micInNoDC / 16.0f; // ... using unfiltered sample
        sampleReal = micInNoDC;
      } else {
        sampleAdj = tmpSample * sampleGain / 40.0f * inputLevel/128.0f + tmpSample / 16.0f; // ... using pre-filtered sample
        sampleReal = tmpSample;
      }

      sampleAdj = fmax(fmin(sampleAdj, 255.0f), 0.0f);        // Question: why are we limiting the value to 8 bits ???
      sampleRaw = (int16_t)sampleAdj;                   // ONLY update sample ONCE!!!!

      // keep "peak" sample, but decay value if current sample is below peak
      if ((sampleMax < sampleReal) && (sampleReal > 0.5f)) {
        sampleMax = sampleMax + 0.5f * (sampleReal - sampleMax);  // new peak - with some filtering
#if 1
        // another simple way to detect samplePeak - cannot detect beats, but reacts on peak volume
        if (((binNum < 12) || ((maxVol < 1))) && (millis() - timeOfPeak > 80) && (sampleAvg > 1)) {
          samplePeak    = true;
          timeOfPeak    = millis();
          udpSamplePeak = true;
        }
#endif
      } else {
        if ((multAgc*sampleMax > agcZoneStop[AGC_preset]) && (soundAgc > 0))
          sampleMax += 0.5f * (sampleReal - sampleMax);        // over AGC Zone - get back quickly
        else
          sampleMax *= agcSampleDecay[AGC_preset];             // signal to zero --> 5-8sec
      }
      if (sampleMax < 0.5f) sampleMax = 0.0f;

      if (micQuality > 0) {
        if (micQuality > 1) sampleAvg += 0.95f * (sampleAdj - sampleAvg);
        else sampleAvg += 0.70f * (sampleAdj - sampleAvg);
      } else {
#if defined(WLEDMM_FASTPATH)
      sampleAvg = ((sampleAvg * 11.0f) + sampleAdj) / 12.0f;   // make reactions a bit more "crisp" in fastpath mode 
#else
      sampleAvg = ((sampleAvg * 15.0f) + sampleAdj) / 16.0f;   // Smooth it out over the last 16 samples.
#endif
      }
      sampleAvg = fabsf(sampleAvg);                            // make sure we have a positive value
    } // getSample()


    // current sensitivity, based on AGC gain (multAgc)
    float getSensitivity()
    {
      // start with AGC gain factor
      float tmpSound = multAgc;
      // experimental: this gives you a calculated "real gain"
      // if ((sampleAvg> 1.0) && (sampleReal > 0.05)) tmpSound = (float)sampleRaw / sampleReal;    // calculate gain from sampleReal 
      // else tmpSound = ((float)sampleGain/40.0f * (float)inputLevel/128.0f) + 1.0f/16.0f;        // silence --> use values from user settings

      if (soundAgc == 0)
        tmpSound = ((float)sampleGain/40.0f * (float)inputLevel/128.0f) + 1.0f/16.0f;   // AGC off -> use non-AGC gain from presets
      else
        tmpSound /= (float)sampleGain/40.0f + 1.0f/16.0f;                               // AGC ON -> scale value so 1 = middle value

      // scale to 0..255. Actually I'm not absolutely happy with this, but it works
      if (tmpSound > 1.0) tmpSound = sqrtf(tmpSound);
      if (tmpSound > 1.25) tmpSound = ((tmpSound-1.25f)/3.42f) +1.25f;
      // we have a value now that should be between 0 and 4 (representing gain 1/16 ... 16.0)
      return fminf(fmaxf(128.0*tmpSound -6.0f, 0), 255.0);        // return scaled non-inverted value // "-6" to ignore values below 1/24
    }

    // estimate sound pressure, based on some assumptions : 
    //   * sample max = 32676 -> Acoustic overload point  --> 105db ==> 255
    //   * sample < squelch   -> just above hearing level -->   5db ==> 0
    // see https://en.wikipedia.org/wiki/Sound_pressure#Examples_of_sound_pressure
    // use with I2S digital microphones. Expect stupid values for analog in, and with Line-In !!
    float estimatePressure() {
      // some constants
      constexpr float logMinSample = 0.8329091229351f; // ln(2.3)
      constexpr float sampleMin = 2.3f;
      constexpr float logMaxSample = 10.1895683436f; // ln(32767 - 6144)
      constexpr float sampleMax = 32767.0f - 6144.0f;

      // take the max sample from last I2S batch.
      float micSampleMax = fabsf(sampleReal);      // from getSample() - nice results, however a bit distorted by MicLev processing
      //float micSampleMax = fabsf(micDataReal);   // from FFTCode() - better source, but more flickering
      if (dmType == 0) micSampleMax *= 2.0f;       // correction for ADC analog
      //if (dmType == 4) micSampleMax *= 16.0f;      // correction for I2S Line-In
      if (dmType == 5) micSampleMax *= 2.0f;       // correction for PDM
      if (dmType == 4) {               // I2S Line-In. This is a dirty trick to make sound pressure look interesting for line-in (which doesn't have "sound pressure" as its not a microphone)
        micSampleMax /= 11.0f;         // reduce to max 128
        micSampleMax *= micSampleMax;  // blow up --> max 16000
      }
      // make sure we are in expected ranges
      if(micSampleMax <= sampleMin) return 0.0f;
      if(micSampleMax >= sampleMax) return 255.0f;

      // apply logarithmic scaling
      float scaledvalue = logf(micSampleMax);
      scaledvalue = (scaledvalue - logMinSample) / (logMaxSample - logMinSample); // 0...1
      return fminf(fmaxf(256.0*scaledvalue, 0), 255.0);        // scaled value
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

      if (gotNewSample) { // take new FFT samples as target values
        for(unsigned i=0; i < NUM_GEQ_CHANNELS; i++) {
          fftCalc[i] = fftResult[i];
          fftResult[i] = fftAvg[i];
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
        fftResult[i] = fftAvg[i];
      }
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

    void transmitAudioData() {
      if (!udpSyncConnected) return; // We reuse the listener socket for sending
      static uint8_t frameCounter = 0;

      audioSyncPacket transmitData;
      memset(reinterpret_cast<void*>(&transmitData), 0, sizeof(transmitData));

      strncpy_P(transmitData.header, PSTR(UDP_SYNC_HEADER), 6);
      transmitData.sampleRaw = (soundAgc) ? rawSampleAgc : sampleRaw;
      transmitData.sampleSmth = (soundAgc) ? sampleAgc : sampleAvg;
      transmitData.samplePeak = udpSamplePeak ? 1 : 0;
      udpSamplePeak = false;
      transmitData.frameCounter = frameCounter;
      transmitData.zeroCrossingCount = zeroCrossingCount;

      for (int i = 0; i < NUM_GEQ_CHANNELS; i++) {
        transmitData.fftResult[i] = fftResult[i];
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
      IPAddress multicastGroup(239, 0, 0, 1);
      fftUdp.writeTo(reinterpret_cast<uint8_t*>(&transmitData), sizeof(transmitData), multicastGroup, audioSyncPort);

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
      sampleRaw    = volumeRaw;
      sampleAvg    = volumeSmth;
      rawSampleAgc = volumeRaw;
      sampleAgc    = volumeSmth;
      multAgc      = 1.0f;
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
      for (int i = 0; i < NUM_GEQ_CHANNELS; i++) fftResult[i] = receivedPacket.fftResult[i];
      my_magnitude  = fmaxf(receivedPacket.FFT_Magnitude, 0.0f);
      FFT_Magnitude = my_magnitude;
      FFT_MajorPeak = constrain(receivedPacket.FFT_MajorPeak, 1.0f, 11025.0f);  // restrict value to range expected by effects
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
      sampleRaw    = fmaxf(receivedPacket->sampleRaw, 0.0f);
      sampleAvg    = fmaxf(receivedPacket->sampleAvg, 0.0f);;
      sampleAgc    = volumeSmth;
      rawSampleAgc = volumeRaw;
      multAgc      = 1.0f;   
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
      for (int i = 0; i < NUM_GEQ_CHANNELS; i++) fftResult[i] = receivedPacket->fftResult[i];
      my_magnitude  = fmaxf(receivedPacket->FFT_Magnitude, 0.0);
      FFT_Magnitude = my_magnitude;
      FFT_MajorPeak = constrain(receivedPacket->FFT_MajorPeak, 1.0, 11025.0);  // restrict value to range expected by effects
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
        um_data->u_size = 12;
        um_data->u_type = new um_types_t[um_data->u_size];
        um_data->u_data = new void*[um_data->u_size];
        um_data->u_data[0] = &volumeSmth;      //*used (New)
        um_data->u_type[0] = UMT_FLOAT;
        um_data->u_data[1] = &volumeRaw;       // used (New)
        um_data->u_type[1] = UMT_UINT16;
        um_data->u_data[2] = fftResult;        //*used (Blurz, DJ Light, Noisemove, GEQ_base, 2D Funky Plank, Akemi)
        um_data->u_type[2] = UMT_BYTE_ARR;
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
        // ADC over I2S is only possible on "classic" ESP32
        case 0:
        default:
          DEBUGSR_PRINTLN(F("AR: Analog Microphone (left channel only)."));
          useInputFilter = 1;  // PDM bandpass filter seems to work well for analog, too
          audioSource = new I2SAdcSource(SAMPLE_RATE, BLOCK_SIZE);
          delay(100);
          if (audioSource) audioSource->initialize(audioPin);
          break;
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
        if (soundAgc > AGC_NUM_PRESETS) soundAgc = 0; // make sure that AGC preset is valid (to avoid array bounds violation)

        unsigned long t_now = millis();      // remember current time
        int userloopDelay = int(t_now - lastUMRun);
        if (lastUMRun == 0) userloopDelay=0; // startup - don't have valid data from last run.

        #if defined(SR_DEBUG)
          // complain when audio userloop has been delayed for long time. Currently we need userloop running between 500 and 1500 times per second.
          // softhack007 disabled temporarily - avoid serial console spam with MANY LEDs and low FPS
          //if ((userloopDelay > /*23*/ 65) && !disableSoundProcessing && (audioSyncEnabled == AUDIOSYNC_NONE)) {
            //DEBUG_PRINTF("[AR userLoop] hiccup detected -> was inactive for last %d millis!\n", userloopDelay);
          //}
        #endif

        // run filters, and repeat in case of loop delays (hick-up compensation)
        if (userloopDelay <2) userloopDelay = 0;      // minor glitch, no problem
        if (userloopDelay >200) userloopDelay = 200;  // limit number of filter re-runs  
        do {
          getSample();                        // run microphone sampling filters
          agcAvg(t_now - userloopDelay);      // Calculated the PI adjusted value as sampleAvg
          userloopDelay -= 2;                 // advance "simulated time" by 2ms
        } while (userloopDelay > 0);
        lastUMRun = t_now;                    // update time keeping

        // update samples for effects (raw, smooth) 
        volumeSmth = (soundAgc) ? sampleAgc   : sampleAvg;
        volumeRaw  = (soundAgc) ? rawSampleAgc: sampleRaw;
        // update FFTMagnitude, taking into account AGC amplification
        my_magnitude = FFT_Magnitude; // / 16.0f, 8.0f, 4.0f done in effects
        if (soundAgc) my_magnitude *= multAgc;
        if (volumeSmth < 1 ) my_magnitude = 0.001f;  // noise gate closed - mute

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
#ifdef ARDUINO_ARCH_ESP32
        multAgc = 1;
#endif
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
        maxSample5sec = (0.15 * maxSample5sec) + 0.85 *((soundAgc) ? sampleAgc : sampleAvg); // reset, and start with some smoothing
        if (sampleAvg < 1) maxSample5sec = 0; // noise gate 
      } else {
         if ((sampleAvg >= 1)) maxSample5sec = fmaxf(maxSample5sec, (soundAgc) ? rawSampleAgc : sampleRaw); // follow maximum volume
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
      micDataReal = 0.0f;
      volumeRaw = 0; volumeSmth = 0;
      sampleAgc = 0; sampleAvg = 0;
      sampleRaw = 0; rawSampleAgc = 0;
      my_magnitude = 0; FFT_Magnitude = 0; FFT_MajorPeak = 1;
      multAgc = 1;
      memset(fftCalc, 0, sizeof(fftCalc));
      memset(fftAvg, 0, sizeof(fftAvg));
      memset(fftResult, 0, sizeof(fftResult));
      for (int i = (init ? 0 : 1); i < NUM_GEQ_CHANNELS; i += 2) fftResult[i] = 16;
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
              FFTcode, "FFT", FFT_TASK_STACK, NULL, FFTTASK_PRIORITY, &FFT_Task, 1
              #else 
              FFTcode, "FFT", 8192, NULL, FFTTASK_PRIORITY, & FFT_Task, -1
              #endif
            );
        }
      }
      micDataReal = 0.0f;
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
     * addToJsonInfo() can be used to add custom entries to the /json/info part of the JSON API.
     * Creating an "u" object allows you to add custom key/value pairs to the Info section of the WLED web UI.
     * Below it is shown how this could be used for e.g. a light sensor
     */
    void addToJsonInfo(JsonObject& root)
    {
      char myStringBuffer[16]; // buffer for snprintf() - not used yet on 8266
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

        // Input Level Slider
        if (disableSoundProcessing == false) {                                 // only show slider when audio processing is running
          if (soundAgc > 0) {
            infoArr = user.createNestedArray(F("GEQ Input Level"));           // if AGC is on, this slider only affects fftResult[] frequencies
            // show slider value as a number
            float post_gain = (float)inputLevel/128.0f;
            if (post_gain < 1.0f) post_gain = ((post_gain -1.0f) * 0.8f) +1.0f;
            post_gain = roundf(post_gain * 100.0f);
            snprintf_P(myStringBuffer, 15, PSTR("%3.0f %%"), post_gain);
            infoArr.add(myStringBuffer);
          } else {
            infoArr = user.createNestedArray(F("Audio Input Level"));
          }
          uiDomString = F("<div class=\"slider\"><div class=\"sliderwrap il\"><input class=\"noslide\" onchange=\"requestJson({");
          uiDomString += FPSTR(_name);
          uiDomString += F(":{");
          uiDomString += FPSTR(_inputLvl);
          uiDomString += F(":parseInt(this.value)}});\" oninput=\"updateTrail(this);\" max=255 min=0 type=\"range\" value=");
          uiDomString += inputLevel;
          uiDomString += F(" /><div class=\"sliderdisplay\"></div></div></div>"); //<output class=\"sliderbubble\"></output>
          infoArr.add(uiDomString);
        } 

        // The following can be used for troubleshooting user errors and is so not enclosed in #ifdef WLED_DEBUG
        // current Audio input
        infoArr = user.createNestedArray(F("Audio Source"));
        if ((audioSyncEnabled == AUDIOSYNC_REC) || (!audioSyncIDLE && (audioSyncEnabled == AUDIOSYNC_REC_PLUS))){
          // UDP sound sync - receive mode
          infoArr.add(F("UDP sound sync"));
          if (udpSyncConnected) {
            if (millis() - last_UDPTime < AUDIOSYNC_IDLE_MS)
              infoArr.add(F(" - receiving"));
            else
              infoArr.add(F(" - idle"));
          } else {
            infoArr.add(F(" - no connection"));
          }
#ifndef ARDUINO_ARCH_ESP32  // substitute for 8266
        } else {
          infoArr.add(F("sound sync Off"));
        }
#else  // ESP32 only
        } else {
          // Analog or I2S digital input
          if (audioSource && (audioSource->isInitialized())) {
            // audio source successfully configured
            if (audioSource->getType() == AudioSource::Type_I2SAdc) {
              infoArr.add(F("ADC analog"));
            } else {
              if (dmType != 51)
                infoArr.add(F("I2S digital"));
              else
                infoArr.add(F("legacy I2S PDM"));
            }
            // input level or "silence"
            if (maxSample5sec > 1.0) {
              float my_usage = 100.0f * (maxSample5sec / 255.0f);
              snprintf_P(myStringBuffer, 15, PSTR(" - peak %3d%%"), int(my_usage));
              infoArr.add(myStringBuffer);
            } else {
              infoArr.add(F(" - quiet"));
            }
          } else {
            // error during audio source setup
            infoArr.add(F("not initialized"));
            if (dmType < 254) infoArr.add(F(" - check pin settings"));
          }
        }

        // Active codec name (matches the "Audio Source" dropdown in the
        // usermod settings page). This is the verbatim label string used
        // for the currently-selected dmType, so the info pane stays in sync
        // with whatever the user picked.
        {
          infoArr = user.createNestedArray(F("Active codec"));
          const char* codecName = "";
          switch (dmType) {
            case 0:   codecName = PSTR("Generic Analog"); break;
            case 1:   codecName = PSTR("Generic I2S"); break;
            case 2:   codecName = PSTR("ES7243"); break;
            case 3:   codecName = PSTR("SPH0654"); break;
            case 4:   codecName = PSTR("Generic I2S with MCLK"); break;
            case 5:   codecName = PSTR("Generic I2S PDM"); break;
            case 51:  codecName = PSTR("Legacy I2S PDM"); break;
            case 6:   codecName = PSTR("ES8388"); break;
            case 7:   codecName = PSTR("WM8978"); break;
            case 8:   codecName = PSTR("AC101"); break;
            case 9:   codecName = PSTR("ES8311"); break;
            case 10:  codecName = PSTR("ES8311 (esp_codec_dev)"); break;
            case 11:  codecName = PSTR("ES8388 (esp_codec_dev)"); break;
            case 12:  codecName = PSTR("ES8374 (esp_codec_dev)"); break;
            case 13:  codecName = PSTR("ZL38063 (esp_codec_dev)"); break;
            case 14:  codecName = PSTR("ES8389 (esp_codec_dev)"); break;
            case 15:  codecName = PSTR("ES7210 (esp_codec_dev)"); break;
            case 16:  codecName = PSTR("ES7243 (esp_codec_dev)"); break;
            case 17:  codecName = PSTR("ES7243E (esp_codec_dev)"); break;
            case 18:  codecName = PSTR("CJC8910 (esp_codec_dev)"); break;
            case 254: codecName = PSTR("None - network receive only"); break;
            default:  codecName = PSTR("Unknown"); break;
          }
          infoArr.add(codecName);
        }

        // Sound processing (FFT and input filters)
        infoArr = user.createNestedArray(F("Sound Processing"));
        if (audioSource && (disableSoundProcessing == false)) {
          infoArr.add(F("running"));
        } else {
          infoArr.add(F("suspended"));
        }

        // AGC or manual Gain
        if ((soundAgc == 0) && (disableSoundProcessing == false) && !(audioSyncEnabled == AUDIOSYNC_REC)) {
          infoArr = user.createNestedArray(F("Manual Gain"));
          float myGain = ((float)sampleGain/40.0f * (float)inputLevel/128.0f) + 1.0f/16.0f;     // non-AGC gain from presets
          infoArr.add(roundf(myGain*100.0f) / 100.0f);
          infoArr.add("x");
        }
        if ((soundAgc > 0) && (disableSoundProcessing == false) && !(audioSyncEnabled == AUDIOSYNC_REC)) {
          infoArr = user.createNestedArray(F("AGC Gain"));
          char agcBuf[12];
          snprintf(agcBuf, sizeof(agcBuf), "%.2f", multAgc);
          infoArr.add(agcBuf);
          infoArr.add("x");
        }
#endif
        // UDP Sound Sync status
        infoArr = user.createNestedArray(F("UDP Sound Sync"));
        if (audioSyncEnabled) {
          if (audioSyncEnabled & AUDIOSYNC_SEND) {
            infoArr.add(F("send mode"));
            if ((udpSyncConnected) && (millis() - lastTime < AUDIOSYNC_IDLE_MS)) infoArr.add(F(" v2+"));
          } else if (audioSyncEnabled == AUDIOSYNC_REC) {
              infoArr.add(F("receive mode"));
          } else if (audioSyncEnabled == AUDIOSYNC_REC_PLUS) {
              infoArr.add(F("receive+local mode"));
          }
        } else
          infoArr.add("off");
        if (audioSyncEnabled && !udpSyncConnected) infoArr.add(" <i>(unconnected)</i>");
        if (audioSyncEnabled && udpSyncConnected && (millis() - last_UDPTime < AUDIOSYNC_IDLE_MS)) {
            if (receivedFormat == 1) infoArr.add(F(" v1"));
            if (receivedFormat == 2) infoArr.add(F(" v2"));
            if (receivedFormat == 3) {
              if (audioSyncSequence) infoArr.add(F(" v2+")); // Sequence checking enabled
              else infoArr.add(F(" v2"));
            }
        }

        #if 1 || (defined(WLED_DEBUG) || defined(SR_DEBUG) || defined(SR_STATS))
        #ifdef ARDUINO_ARCH_ESP32
        // Auto-scale timing values to µs / ms / s based on magnitude. Most
        // values land in the ms range; very small ones (filter, idle) use µs
        // and very large ones (over budget) use s.
        //
        // This takes a RAW stored value in TIMING_UNIT_US, NOT microseconds,
        // and does the conversion itself. That is deliberate: the previous
        // version took microseconds, so every row was handed a 10 µs-unit
        // number and divided it by 1000 - reporting a 24.9 ms frame as
        // "2.49 ms" and making a saturated core look comfortable. Taking the
        // stored value makes that class of bug impossible to reintroduce at a
        // call site.
        char timeBuf[16];
        auto fmtTime = [](char *buf, size_t buflen, float stored) {
          const float us = stored * TIMING_UNIT_US;
          if (us < 1000.0f)         snprintf(buf, buflen, "%.1f µs", us);
          else if (us < 1000000.0f)  snprintf(buf, buflen, "%.2f ms", us / 1000.0f);
          else                      snprintf(buf, buflen, "%.2f s",  us / 1000000.0f);
        };

        infoArr = user.createNestedArray(F("I2S cycle time"));
        fmtTime(timeBuf, sizeof(timeBuf), fftTaskCycle);
        infoArr.add(timeBuf);
        infoArr.add(pacingVerdict(fftTaskCycle));

        // Mostly the task waiting on the DMA, not working. See pacingVerdict().
        infoArr = user.createNestedArray(F("Sampling time"));
        fmtTime(timeBuf, sizeof(timeBuf), sampleTime);
        infoArr.add(timeBuf);
        infoArr.add(pacingVerdict(sampleTime));

        infoArr = user.createNestedArray(F("FFT time (ESP-DSP)"));

        fmtTime(timeBuf, sizeof(timeBuf), fftTime);
        infoArr.add(timeBuf);
        infoArr.add(fftBudgetVerdict(fftTime, fftTime + filterTime));

        // Breakdown of the row above. "FFT time (ESP-DSP)" is a misleading name
        // for what it measures - it is the whole frame from windowing through
        // postProcessFFTResults, and the transform is only one of five stages in
        // it. Until that is understood, treat the split as the real diagnosis
        // and the total as the sum of it.
        infoArr = user.createNestedArray(F("  windowing"));
        fmtTime(timeBuf, sizeof(timeBuf), tWindow);
        infoArr.add(timeBuf);
        infoArr.add(fftBudgetVerdict(tWindow, fftTime + filterTime));

        infoArr = user.createNestedArray(F("  sub-bin DFT"));
        fmtTime(timeBuf, sizeof(timeBuf), tBandDft);
        infoArr.add(timeBuf);
        infoArr.add(fftBudgetVerdict(tBandDft, fftTime + filterTime));

        infoArr = user.createNestedArray(F("  ESP-DSP FFT"));
        fmtTime(timeBuf, sizeof(timeBuf), tFft);
        infoArr.add(timeBuf);
        infoArr.add(fftBudgetVerdict(tFft, fftTime + filterTime));

        infoArr = user.createNestedArray(F("  bitrev + mag"));
        fmtTime(timeBuf, sizeof(timeBuf), tPost);
        infoArr.add(timeBuf);
        infoArr.add(fftBudgetVerdict(tPost, fftTime + filterTime));

        infoArr = user.createNestedArray(F("  bands + post"));
        fmtTime(timeBuf, sizeof(timeBuf), tFinal);
        infoArr.add(timeBuf);
        infoArr.add(fftBudgetVerdict(tFinal, fftTime + filterTime));

        infoArr = user.createNestedArray(F("Filtering time"));
        fmtTime(timeBuf, sizeof(timeBuf), filterTime);
        infoArr.add(timeBuf);

        // Total CPU work: filtering + FFT. This is the number that has to fit
        // inside the frame period - the sampling wait is not a term in it,
        // because waiting for audio is not something the task can hurry up.
        infoArr.add(fftBudgetVerdict(filterTime, fftTime + filterTime));

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
        DEBUGSR_PRINTF("AR   sub-bin DFT : %5.2f ms\n", roundf(tBandDft)/100.0f);
        DEBUGSR_PRINTF("AR   ESP-DSP FFT : %5.2f ms\n", roundf(tFft)/100.0f);
        DEBUGSR_PRINTF("AR   bitrev +mag : %5.2f ms\n", roundf(tPost)/100.0f);
        DEBUGSR_PRINTF("AR   bands +post : %5.2f ms\n", roundf(tFinal)/100.0f);
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
      dmic[F("rightSlot")] = i2sUseRightSlot;
      dmic[F("master")]  = i2sMaster;

      JsonObject cfg = top.createNestedObject("config");
      cfg[F("squelch")] = soundSquelch;
      cfg[F("gain")] = sampleGain;
      cfg[F("AGC")] = soundAgc;

      //WLEDMM: experimental settings
      JsonObject poweruser = top.createNestedObject("experiments");
      poweruser[F("micLev")] = micLevelMethod;
      poweruser[F("Mic_Quality")] = micQuality;
      poweruser[F("freqDist")] = freqDist;
      //poweruser[F("freqRMS")] = averageByRMS;
      poweruser[F("FFT_Window")] = fftWindow;
      poweruser[F("I2S_FastPath")] = doSlidingFFT;
      poweruser[F("AutoLevel")] = useAutoLevel;
      poweruser[F("BandCompress")] = useBandCompress;
      poweruser[F("NoiseFloorSub")] = useNoiseFloorSub;
      poweruser[F("HistogramNorm")] = useHistogramNorm;
      poweruser[F("Band0Lifter")] = useBand0Lifter;

      JsonObject freqScale = top.createNestedObject("frequency");
      freqScale[F("scale")] = FFTScalingMode;
      freqScale[F("profile")] = pinkIndex; //WLEDMM
      // Both ends of the dB display mapping (FFTScalingMode 3). Kept here rather than
      // as fixed constants because they describe the input level and how much range the
      // bar should cover, not the filter chain - so they are per-install.
      freqScale[F("dBref")]  = TROYHACKS_DBREF;
      freqScale[F("dBspan")] = TROYHACKS_DBSPAN;
#endif
      JsonObject dynLim = top.createNestedObject("dynamics");
      dynLim[F("limiter")] = limiterOn;
      dynLim[F("rise")] = attackTime;
      dynLim[F("fall")] = decayTime;

      JsonObject sync = top.createNestedObject("sync");
      sync[F("port")] = audioSyncPort;
      sync[F("mode")] = audioSyncEnabled;
      sync[F("check_sequence")] = audioSyncSequence;
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

      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
#ifdef ARDUINO_ARCH_ESP32
    #if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32S3)
      configComplete &= getJsonValue(top[FPSTR(_analogmic)]["pin"], audioPin);
    #else
      audioPin = -1; // MCU does not support analog mic
    #endif

      configComplete &= getJsonValue(top[FPSTR(_digitalmic)]["type"],   dmType);
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

      configComplete &= getJsonValue(top[FPSTR(_digitalmic)]["pin"][0], i2ssdPin);
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)]["pin"][1], i2swsPin);
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)]["pin"][2], i2sckPin);
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)]["pin"][3], mclkPin);
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)]["pin"][4], sdaPin);
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)]["pin"][5], sclPin);

      uint8_t bitsRead = i2sBitsPerSample;
      bool rightSlotRead = i2sUseRightSlot;
      bool masterRead = i2sMaster;
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)][F("bits")],      bitsRead);
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)][F("rightSlot")], rightSlotRead);
      configComplete &= getJsonValue(top[FPSTR(_digitalmic)][F("master")],    masterRead);
      // Sanitize bit-width: only 16/24/32 are valid.
      if (bitsRead == 16 || bitsRead == 24 || bitsRead == 32) i2sBitsPerSample = bitsRead;
      i2sUseRightSlot = rightSlotRead;
      i2sMaster = masterRead;

      configComplete &= getJsonValue(top["config"][F("squelch")], soundSquelch);
      configComplete &= getJsonValue(top["config"][F("gain")],    sampleGain);
      configComplete &= getJsonValue(top["config"][F("AGC")],     soundAgc);

      //WLEDMM: experimental settings
      configComplete &= getJsonValue(top["experiments"][F("micLev")], micLevelMethod);
      configComplete &= getJsonValue(top["experiments"][F("Mic_Quality")], micQuality);
      configComplete &= getJsonValue(top["experiments"][F("freqDist")], freqDist);
      //configComplete &= getJsonValue(top["experiments"][F("freqRMS")],  averageByRMS);
      configComplete &= getJsonValue(top["experiments"][F("FFT_Window")], fftWindow);

      configComplete &= getJsonValue(top["experiments"][F("I2S_FastPath")], doSlidingFFT);
      configComplete &= getJsonValue(top["experiments"][F("AutoLevel")], useAutoLevel);
      configComplete &= getJsonValue(top["experiments"][F("BandCompress")], useBandCompress);
      configComplete &= getJsonValue(top["experiments"][F("NoiseFloorSub")], useNoiseFloorSub);
      configComplete &= getJsonValue(top["experiments"][F("HistogramNorm")], useHistogramNorm);
      configComplete &= getJsonValue(top["experiments"][F("Band0Lifter")], useBand0Lifter);


      configComplete &= getJsonValue(top["frequency"][F("scale")], FFTScalingMode);
      configComplete &= getJsonValue(top["frequency"][F("profile")], pinkIndex);  //WLEDMM
      // dB display mapping. Clamp on read: these are also walked by the 'd'/'D' and
      // 's'/'S' serial commands, and a value typed into the settings page should not be
      // able to produce a mapping that makes every band read full or empty.
      float dbRefRead = TROYHACKS_DBREF, dbSpanRead = TROYHACKS_DBSPAN;
      configComplete &= getJsonValue(top["frequency"][F("dBref")],  dbRefRead);
      configComplete &= getJsonValue(top["frequency"][F("dBspan")], dbSpanRead);
      if (dbRefRead  > 1e-7f && dbRefRead  < 1.0f)   TROYHACKS_DBREF  = dbRefRead;
      if (dbSpanRead >= 12.0f  && dbSpanRead <= 96.0f) TROYHACKS_DBSPAN = dbSpanRead;
#endif
      configComplete &= getJsonValue(top["dynamics"][F("limiter")], limiterOn);
      configComplete &= getJsonValue(top["dynamics"][F("rise")],  attackTime);
      configComplete &= getJsonValue(top["dynamics"][F("fall")],  decayTime);

      configComplete &= getJsonValue(top["sync"][F("port")], audioSyncPort);
      configComplete &= getJsonValue(top["sync"][F("mode")], audioSyncEnabled);
      configComplete &= getJsonValue(top["sync"][F("check_sequence")], audioSyncSequence);

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
      #ifdef SR_SQUELCH
        oappend(SET_F("addInfo(ux+':config:squelch',1,'<i>&#9100; ")); oappendi(SR_SQUELCH); oappend("</i>');");  // 0 is field type, 1 is actual field
      #endif
      #ifdef SR_GAIN
        oappend(SET_F("addInfo(ux+':config:gain',1,'<i>&#9100; ")); oappendi(SR_GAIN); oappend("</i>');");  // 0 is field type, 1 is actual field
      #endif

      oappend(SET_F("dd=addDropdown(ux,'config:AGC');"));
      oappend(SET_F("addOption(dd,'Off',0);"));
      oappend(SET_F("addOption(dd,'Normal',1);"));
      oappend(SET_F("addOption(dd,'Vivid',2);"));
      oappend(SET_F("addOption(dd,'Lazy',3);"));

      //WLEDMM: experimental settings
      oappend(SET_F("xx='experiments';")); // shortcut
      oappend(SET_F("dd=addDropdown(ux,xx+':micLev');"));
      oappend(SET_F("addOption(dd,'Floating  (⎌)',0);"));
      oappend(SET_F("addOption(dd,'Freeze',1);"));
      oappend(SET_F("addOption(dd,'Fast Freeze',2);"));
      oappend(SET_F("addInfo(ux+':'+xx+':micLev',1,'☾');"));

      oappend(SET_F("dd=addDropdown(ux,xx+':Mic_Quality');"));
      oappend(SET_F("addOption(dd,'average (standard)',0);"));
      oappend(SET_F("addOption(dd,'low noise',1);"));
      oappend(SET_F("addOption(dd,'perfect',2);"));

      oappend(SET_F("dd=addDropdown(ux,xx+':freqDist');"));
      oappend(SET_F("addOption(dd,'Normal',0);"));
      oappend(SET_F("addOption(dd,'RightShift',1);"));
      oappend(SET_F("addOption(dd,'Psychoacoustic  (⎌)',2);"));
      oappend(SET_F("addInfo(ux+':'+xx+':freqDist',1,'☾');"));

      //oappend(SET_F("dd=addDropdown(ux,xx+':freqRMS');"));
      //oappend(SET_F("addOption(dd,'Off  (⎌)',0);"));
      //oappend(SET_F("addOption(dd,'On',1);"));
      //oappend(SET_F("addInfo(ux+':experiments:freqRMS',1,'☾');"));

      oappend(SET_F("dd=addDropdown(ux,xx+':FFT_Window');"));
      oappend(SET_F("addOption(dd,'Blackman-Harris (MM standard)',0);"));
      oappend(SET_F("addOption(dd,'Hann (balanced)',1);"));
      oappend(SET_F("addOption(dd,'Nuttall (more accurate)',2);"));
      oappend(SET_F("addOption(dd,'Blackman',5);"));
      oappend(SET_F("addOption(dd,'Hamming',3);"));
      oappend(SET_F("addOption(dd,'Flat-Top (AC WLED, inaccurate)',4);"));

      oappend(SET_F("dd=addDropdown(ux,xx+':I2S_FastPath');"));
      oappend(SET_F("addOption(dd,'Off',0);"));
      oappend(SET_F("addOption(dd,'On  (⎌)',1);"));
      oappend(SET_F("addInfo(ux+':'+xx+':I2S_FastPath',1,'☾');"));

      oappend(SET_F("dd=addDropdown(ux,xx+':AutoLevel');"));
      oappend(SET_F("addOption(dd,'Off',0);"));
      oappend(SET_F("addOption(dd,'AGC: per-band  (⎌)',1);"));
      oappend(SET_F("addInfo(ux+':'+xx+':AutoLevel',1,'🐺');"));

      oappend(SET_F("dd=addDropdown(ux,xx+':BandCompress');"));
      oappend(SET_F("addOption(dd,'Off  (⎌)',0);"));
      oappend(SET_F("addOption(dd,'Per-band log: soften peaks',1);"));
      oappend(SET_F("addInfo(ux+':'+xx+':BandCompress',1,'🐺');"));

      oappend(SET_F("dd=addDropdown(ux,xx+':NoiseFloorSub');"));
      oappend(SET_F("addOption(dd,'Off  (⎌)',0);"));
      oappend(SET_F("addOption(dd,'Subtract per-band floor',1);"));
      oappend(SET_F("addInfo(ux+':'+xx+':NoiseFloorSub',1,'🐺');"));

      oappend(SET_F("dd=addDropdown(ux,xx+':HistogramNorm');"));
      oappend(SET_F("addOption(dd,'Off  (⎌)',0);"));
      oappend(SET_F("addOption(dd,'Global gain via loudest band',1);"));
      oappend(SET_F("addInfo(ux+':'+xx+':HistogramNorm',1,'🐺');"));

      oappend(SET_F("dd=addDropdown(ux,xx+':Band0Lifter');"));
      oappend(SET_F("addOption(dd,'Off  (⎌)',0);"));
      oappend(SET_F("addOption(dd,'Lift band 0 from band 1',1);"));
      oappend(SET_F("addInfo(ux+':'+xx+':Band0Lifter',1,'🐺');"));

      oappend(SET_F("dd=addDropdown(ux,'dynamics:limiter');"));
      oappend(SET_F("addOption(dd,'Off',0);"));
      oappend(SET_F("addOption(dd,'On',1);"));
      oappend(SET_F("addInfo(ux+':dynamics:limiter',0,' On ');"));  // 0 is field type, 1 is actual field
      oappend(SET_F("addInfo(ux+':dynamics:rise',1,'ms <i>(&#x266A; effects only)</i>');"));
      oappend(SET_F("addInfo(ux+':dynamics:fall',1,'ms <i>(&#x266A; effects only)</i>');"));

      oappend(SET_F("dd=addDropdown(ux,'frequency:scale');"));
      oappend(SET_F("addOption(dd,'None',0);"));
      oappend(SET_F("addOption(dd,'Linear (Amplitude)',2);"));
      oappend(SET_F("addOption(dd,'Square Root (Energy)',3);"));
      oappend(SET_F("addOption(dd,'Logarithmic (Loudness)',1);"));

      //WLEDMM add defaults
      oappend(SET_F("dd=addDropdown(ux,'frequency:profile');"));
      #if SR_FREQ_PROF==0
        oappend(SET_F("addOption(dd,'Generic Microphone (⎌)',0);"));
      #else
        oappend(SET_F("addOption(dd,'Generic Microphone',0);"));
      #endif
      #if SR_FREQ_PROF==1
        oappend(SET_F("addOption(dd,'Generic Line-In (⎌)',1);"));
      #else
        oappend(SET_F("addOption(dd,'Generic Line-In',1);"));
      #endif
      #if SR_FREQ_PROF==PINK_AUTO
        oappend(SET_F("addOption(dd,'Auto - derived (⎌)',12);"));
      #else
        oappend(SET_F("addOption(dd,'Auto - derived',12);"));
      #endif
      #if SR_FREQ_PROF==5
        oappend(SET_F("addOption(dd,'ICS-43434 (⎌)',5);"));
      #else
        oappend(SET_F("addOption(dd,'ICS-43434',5);"));
      #endif
      #if SR_FREQ_PROF==6
        oappend(SET_F("addOption(dd,'ICS-43434 - big speakers (⎌)',6);"));
      #else
        oappend(SET_F("addOption(dd,'ICS-43434 - big speakers',6);"));
      #endif
      #if SR_FREQ_PROF==7
        oappend(SET_F("addOption(dd,'SPM1423 (⎌)',7);"));
      #else
        oappend(SET_F("addOption(dd,'SPM1423',7);"));
      #endif
      #if SR_FREQ_PROF==2
        oappend(SET_F("addOption(dd,'IMNP441 (⎌)',2);"));
      #else
        oappend(SET_F("addOption(dd,'IMNP441',2);"));
      #endif
      #if SR_FREQ_PROF==3
        oappend(SET_F("addOption(dd,'IMNP441 - big speakers (⎌)',3);"));
      #else
        oappend(SET_F("addOption(dd,'IMNP441 - big speakers',3);"));
      #endif
      #if SR_FREQ_PROF==4
        oappend(SET_F("addOption(dd,'IMNP441 - small speakers (⎌)',4);"));
      #else
        oappend(SET_F("addOption(dd,'IMNP441 - small speakers',4);"));
      #endif
      #if SR_FREQ_PROF==10
        oappend(SET_F("addOption(dd,'flat - no adjustments (⎌)',10);"));
      #else
        oappend(SET_F("addOption(dd,'flat - no adjustments',10);"));
      #endif
      #if SR_FREQ_PROF==8
        oappend(SET_F("addOption(dd,'userdefined #1 (⎌)',8);"));
      #else
        oappend(SET_F("addOption(dd,'userdefined #1',8);"));
      #endif
      #if SR_FREQ_PROF==9
        oappend(SET_F("addOption(dd,'userdefined #2 (⎌)',9);"));
      #else
        oappend(SET_F("addOption(dd,'userdefined #2',9);"));
      #endif
      oappend(SET_F("addInfo(ux+':frequency:profile',1,'☾');"));
      oappend(SET_F("addInfo(ux+':frequency:dBref',1,'<i>bar reads full at this band level; lower it if the top bands are pinned</i>');"));
      oappend(SET_F("addInfo(ux+':frequency:dBspan',1,'<i>how many dB the bar covers - smaller = more movement (☾)</i>');"));
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
