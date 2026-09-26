/*
  Flow Toys usermod — self-contained IMU-driven visualizations for hoops, poi, staff.

  Contains its own MPU6050 driver (no USERMOD_MPU6050_IMU needed).
  Configure the interrupt pin via WLED's "MPU6050 INT GPIO" setting or -D MPU6050_INT_GPIO=6

  Effects:
    - mode_GravityBubble  (bubble drifts to gravity low-point — hoop/staff)
    - mode_FlowTrail      (bright dot + fading trail following gravity)
    - mode_GyroRing       (ring expands with tilt magnitude)
    - mode_POVChecker      (2-phase checkerboard POV driven by X-axis tilt)
    - mode_YPRColor       (AccelRGB — solid fill, each axis deviation → R/G/B)

  Architecture based on concepts from willmmiles/wled-motion_reactive and
  lost-hope/particle-tilt-effect.
*/

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "wled.h"

#include <I2Cdev.h>
#include <MPU6050_6Axis_MotionApps20.h>

// ================================================================
// ===          MPU6050 Integrated Driver (no external dep)      ===
// ================================================================

class FlowIMU {
public:
  Quaternion  qat    = {0,0,0,0};
  VectorInt16 aa     = {0,0,0};
  VectorInt16 gy     = {0,0,0};
  VectorFloat gravity = {0,0,0};
  float      ypr[3] = {0,0,0};

  bool dmpReady = false;

  // Accumulated gyro-z rotation (radians) for spin angle
  float spinAccum = 0.0f;

  // Adaptive peak for color normalization (rad/s)
  float colorPeak = 1.0f;

  // ---- public methods ----

  // Tilt angles from DMP gravity vector (sensor-fused, cleaner than raw accel).
  // Returns radians: positive = tilted that direction.
  float tiltRoll()  const { return atan2f(gravity.x, gravity.z); }
  float tiltPitch() const { return atan2f(gravity.y, gravity.z); }

  // Smoothed spin rate (rad/s). Uses exponential smoothing on raw gyro z.
  float spinRate() const { return _smoothedSpinRate; }

  // Accumulated spin angle in radians (integrated gyro z)
  float spinAngle() const { return spinAccum; }

  // Normalized spin [0..1] against adaptive peak, with time-based decay.
  // Call once per frame from the effect that uses it most.
  float updateColorNorm(float dt) {
    float spd = fabsf(_smoothedSpinRate);

    // Time-based peak growth: instantly match any new maximum
    if (spd > colorPeak) {
      colorPeak = spd;
    }

    // Time-based exponential decay: halve over ~30s
    // factor = 0.5^(dt / 30s)
    float decay = powf(0.5f, dt / 30.0f);
    colorPeak *= decay;
    if (colorPeak < 0.3f) colorPeak = 0.3f;  // floor so it never goes to zero

    return fminf(spd / fmaxf(colorPeak, 0.1f), 1.0f);
  }

  void setup() {
    USER_PRINTLN(F("FlowIMU: setup"));

    if (!pinManager.joinWire()) {
      USER_PRINTLN(F("FlowIMU: I2C join failed"));
      return;
    }
    Wire.setClock(400000);

    mpu.initialize();
    delay(100);

    USER_PRINTLN(mpu.testConnection() ? F("FlowIMU: MPU6050 connected") : F("FlowIMU: MPU6050 FAILED"));

    // Claim interrupt pin — stabilizes MPU6050 I2C state
    int8_t intPin = MPU6050_INT_GPIO;
    if (intPin >= 0) {
      if (!pinManager.allocatePin(intPin, false, PinOwner::UM_IMU)) {
        USER_PRINTF("FlowIMU: could not allocate INT GPIO %d\n", intPin);
      } else {
        USER_PRINTF("FlowIMU: INT GPIO %d reserved\n", intPin);
      }
    }
    delay(100);

    devStatus = mpu.dmpInitialize();
    delay(100);

    if (devStatus == 0) {
      mpu.CalibrateAccel(6);
      mpu.CalibrateGyro(6);
      mpu.setDMPEnabled(true);

      dmpReady = true;
      packetSize = mpu.dmpGetFIFOPacketSize();
      USER_PRINTLN(F("FlowIMU: DMP ready"));
    } else {
      USER_PRINTF("FlowIMU: DMP init failed, code=%d\n", devStatus);
    }
  }

  void loop() {
    if (!dmpReady) return;

    // Be nice — skip if LEDs are being programmed
    if (strip.isUpdating()) return;

    if (mpu.dmpGetCurrentFIFOPacket(fifoBuffer)) {
      mpu.dmpGetQuaternion(&qat, fifoBuffer);
      mpu.dmpGetGravity(&gravity, &qat);
      mpu.dmpGetYawPitchRoll(ypr, &qat, &gravity);
      mpu.dmpGetAccel(&aa, fifoBuffer);
      mpu.dmpGetGyro(&gy, fifoBuffer);

      // Freeze gyro drift on first packet
      if (!_gyroDriftSet) {
        _gyroDriftZ = gy.z;
        _gyroDriftSet = true;
      }

      // Smoothed spin rate — exponential filter (matches wled-motion_reactive pattern)
      // gyro_z LSB = 16.4 LSB/°/s; convert to rad/s
      float rawSpin = float(gy.z - _gyroDriftZ) / 16.4f / 180.0f * (float)M_PI;
      float alpha = max(0.0f, 1.0f - 4.0f * 0.01f);  // 4s time constant
      _smoothedSpinRate = alpha * _smoothedSpinRate + (1.0f - alpha) * rawSpin;

      // Accumulate spin angle
      spinAccum += _smoothedSpinRate * 0.01f;  // ~10ms frame

      // Adaptive peak update — time-based decay
      float spd = fabsf(_smoothedSpinRate);
      if (spd > colorPeak) {
        colorPeak = spd;
      } else {
        // ~30s half-life decay
        colorPeak *= 0.977f;
        if (colorPeak < 0.3f) colorPeak = 0.3f;
      }
    }
  }

private:
  MPU6050 mpu;
  uint8_t devStatus = 0;
  uint16_t packetSize = 0;
  uint8_t fifoBuffer[64];

  int16_t _gyroDriftZ = 0;
  bool _gyroDriftSet = false;
  float _smoothedSpinRate = 0.0f;
};

static FlowIMU g_flowIMU;

// ================================================================
// ===                     Effect Modes                           ===
// ================================================================

// ---------------------------------------------------------------------------
// GravityBubble — glowing dot drifts to the gravity low point.
// For a hoop (2×145), bubble moves with pitch/roll of the hoop plane.
// ---------------------------------------------------------------------------
uint16_t mode_GravityBubble(void) {
  const uint16_t cols = SEGMENT.virtualWidth();
  const uint16_t rows = SEGMENT.virtualHeight();

  uint16_t radius = (cols <= 2) ? 1u : std::max(2u, std::min<uint16_t>(cols, rows) / 6u);

  if (g_flowIMU.dmpReady) {
    float roll  = g_flowIMU.tiltRoll();
    float pitch = g_flowIMU.tiltPitch();

    float halfRows = float(rows) / 2.0f;
    float by = halfRows + pitch * halfRows;  // pitch → vertical movement
    float bx = (cols > 2) ? (float(cols) / 2.0f) + roll * (float(cols) / 2.0f) : 1.0f;

    uint16_t bx16 = uint16_t(bx);
    uint16_t by16 = uint16_t(by);
    if (bx16 >= cols) bx16 = cols - 1;
    if (by16 >= rows) by16 = rows - 1;

    SEGMENT.fadeToBlackBy(40);
    
    uint32_t color = SEGCOLOR(0);
    if (color == 0 || color == 0xFFFFFF) color = WHITE;
    SEGMENT.fillCircle(bx16, by16, radius, color, true);
  }

  return FRAMETIME;
}
static const char _data_FX_MODE_GravityBubble[] PROGMEM =
  "Y~GravityBubble~@;!;!;2;pal=0";

// ---------------------------------------------------------------------------
// POV Checker — simple 2-phase checkerboard driven by X-axis tilt.
// Wave the hoop side-to-side to see the squares appear as vertical bands.
//   Phase 0: columns 0,2,4,... lit
//   Phase 1: columns 1,3,5,... lit
// Each square is 10 pixels tall. Phase swaps at |tiltX| threshold.
// ---------------------------------------------------------------------------
uint16_t mode_POVChecker(void) {
  const uint16_t cols = SEGMENT.virtualWidth();
  const uint16_t rows = SEGMENT.virtualHeight();
  static uint8_t phase = 0;
  static unsigned long lastPhaseChange = 0;

  if (g_flowIMU.dmpReady) {
    // X-axis tilt drives which phase is shown
    float tiltX = g_flowIMU.tiltRoll();  // radians, +ve = tilted right

    // Threshold for phase switch (radians)
    constexpr float THRESHOLD = 0.15f;  // ~8.6°

    unsigned long now = millis();
    if (fabsf(tiltX) > THRESHOLD) {
      uint8_t newPhase = tiltX > 0.0f ? 1 : 0;
      if (newPhase != phase && (now - lastPhaseChange) > 80) {
        phase = newPhase;
        lastPhaseChange = now;
      }
    }

    // Draw checkerboard: 10-pixel tall squares
    // Row bands of 10: even bands = phase 0, odd bands = phase 1
    for (uint16_t y = 0; y < rows; y++) {
      uint8_t band = (y / 10) & 0x01;  // 0 or 1
      bool lightCol = (band == phase);   // phase 0 lights even cols, phase 1 lights odd cols
      for (uint16_t x = 0; x < cols; x++) {
        bool light = lightCol ? ((x & 0x01) == 0) : ((x & 0x01) == 1);
        if (light) {
          SEGMENT.setPixelColorXY(x, y, SEGCOLOR(0));
        } else {
          SEGMENT.setPixelColorXY(x, y, BLACK);
        }
      }
    }
  }

  return FRAMETIME;
}
static const char _data_FX_MODE_POVChecker[] PROGMEM =
  "Y~POV Checker~@;;;2;pal=0";

// ---------------------------------------------------------------------------
// FlowTrail — bright dot with fading afterimage trail.
// ---------------------------------------------------------------------------
uint16_t mode_FlowTrail(void) {
  const uint16_t cols = SEGMENT.virtualWidth();
  const uint16_t rows = SEGMENT.virtualHeight();

  uint16_t radius = (cols <= 2) ? 1u : std::max(1u, std::min<uint16_t>(cols, rows) / 10u);

  SEGMENT.fadeToBlackBy(40);

  if (g_flowIMU.dmpReady) {
    float roll  = g_flowIMU.tiltRoll();
    float pitch = g_flowIMU.tiltPitch();

    float halfRows = float(rows) / 2.0f;
    float by = halfRows + pitch * halfRows;
    float bx = (cols > 2) ? (float(cols) / 2.0f) + roll * (float(cols) / 2.0f) : 1.0f;

    uint16_t bx16 = uint16_t(bx);
    uint16_t by16 = uint16_t(by);
    if (bx16 >= cols) bx16 = cols - 1;
    if (by16 >= rows) by16 = rows - 1;

    uint32_t headColor = SEGCOLOR(0);
    if (headColor == 0 || headColor == 0xFFFFFF) headColor = WHITE;
    SEGMENT.fillCircle(bx16, by16, radius, headColor, true);
    SEGMENT.fillCircle(bx16, by16, radius / 2 + 1, WHITE, true);

    // Opposite comet in blue — mirrored position
    int16_t ox = (cols > 2) ? (int16_t(cols) - 1 - bx16) : 1;
    int16_t oy = (int16_t)rows - 1 - by16;
    if (oy < 0) oy = 0;
    if (oy >= (int16_t)rows) oy = rows - 1;
    if (ox < 0) ox = 0;
    if (ox >= (int16_t)cols) ox = cols - 1;
    SEGMENT.fillCircle((uint16_t)ox, (uint16_t)oy, radius, BLUE, true);
    SEGMENT.fillCircle((uint16_t)ox, (uint16_t)oy, radius / 2 + 1, BLUE, true);
  }

  return FRAMETIME;
}
static const char _data_FX_MODE_FlowTrail[] PROGMEM =
  "Y~FlowTrail~@;!;!;2;pal=0";

// ---------------------------------------------------------------------------
// GyroRing — ring expands with total tilt magnitude.
// ---------------------------------------------------------------------------
uint16_t mode_GyroRing(void) {
  const uint16_t cols = SEGMENT.virtualWidth();
  const uint16_t rows = SEGMENT.virtualHeight();
  const uint16_t cx = cols / 2;
  const uint16_t cy = rows / 2;

  if (g_flowIMU.dmpReady) {
    float roll  = fabsf(g_flowIMU.tiltRoll());
    float pitch = fabsf(g_flowIMU.tiltPitch());
    float tiltMag = (roll + pitch) * 40.0f;

    uint16_t halfDim = std::min<uint16_t>(cols, rows) / 2;
    tiltMag = std::min(tiltMag, float(halfDim) - 1.0f);

    uint16_t innerR = std::max<uint16_t>(1u, uint16_t(tiltMag));
    uint16_t outerR = innerR + std::max<uint16_t>(1u, uint16_t(halfDim / 6));

    uint32_t ringColor = SEGMENT.color_from_palette(SEGENV.call % 256, false, true, 0);

    if (outerR < std::min<uint16_t>(cx, cy)) {
      SEGMENT.fillCircle(cx, cy, outerR, ringColor, true);
      SEGMENT.fillCircle(cx, cy, innerR, BLACK, true);
    }
  }

  return FRAMETIME;
}
static const char _data_FX_MODE_GyroRing[] PROGMEM =
  "Y~GyroRing~@;Speed;!;2;pal=0";

// ---------------------------------------------------------------------------
// YPRColor — solid fill, RGB from accelerometer deviation vs gravity.
// Still = black. Each axis's deviation from 1g maps to R/G/B.
//   gravity.x/y/z is the DMP's unit-gravity vector — subtract it to get
//   the deviation that represents actual motion, not orientation.
// Normalized against 2G full-scale (8192 LSB/g).
// ---------------------------------------------------------------------------
uint16_t mode_YPRColor(void) {
  if (g_flowIMU.dmpReady) {
    // Deviation from gravity for each axis (positive = pulled that way, negative = opposite)
    float dx = (float)g_flowIMU.aa.x - g_flowIMU.gravity.x * 8192.0f;
    float dy = (float)g_flowIMU.aa.y - g_flowIMU.gravity.y * 8192.0f;
    float dz = (float)g_flowIMU.aa.z - g_flowIMU.gravity.z * 8192.0f;

    uint8_t r = (uint8_t)min<uint16_t>((uint16_t)fabsf(dx) * 255u / 8192u, 255u);
    uint8_t g = (uint8_t)min<uint16_t>((uint16_t)fabsf(dy) * 255u / 8192u, 255u);
    uint8_t b = (uint8_t)min<uint16_t>((uint16_t)fabsf(dz) * 255u / 8192u, 255u);

    SEGMENT.fill(RGBW32(r, g, b, 0));
  }
  return FRAMETIME;
}
static const char _data_FX_MODE_YPRColor[] PROGMEM =
  "Y~AccelRGB~@;;;2;pal=0";

// ================================================================
// ===              FlowtoysUsermod (self-contained)              ===
// ================================================================
class FlowtoysUsermod : public Usermod {
public:
  void setup() override {
    g_flowIMU.setup();

    strip.addEffect(255, &mode_GravityBubble, _data_FX_MODE_GravityBubble);
    strip.addEffect(255, &mode_FlowTrail,     _data_FX_MODE_FlowTrail);
    strip.addEffect(255, &mode_GyroRing,      _data_FX_MODE_GyroRing);
    strip.addEffect(255, &mode_POVChecker,    _data_FX_MODE_POVChecker);
    strip.addEffect(255, &mode_YPRColor,      _data_FX_MODE_YPRColor);

    initDone = true;
  }

  void loop() override {
    g_flowIMU.loop();
  }

  uint16_t getId() override {
    return USERMOD_ID_FLOWTOYS;
  }
};
