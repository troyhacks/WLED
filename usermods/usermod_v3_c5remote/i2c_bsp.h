/*
 * i2c_bsp.h - the I2C glue for the QMI8658 driver.
 *
 * The driver that ships with the board's Arduino demo talks to the chip
 * through these four functions. They are here, on top of the Arduino Wire,
 * so the driver itself can stay as it is.
 *
 * NOT a standalone translation unit: included by c5_board.h.
 */

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "esp_err.h"
#include "wled.h"

// The bus: the board's I2C is GPIO27 (SDA) and GPIO26 (SCL).
static inline void I2C_master_Init(void) {
  Wire.begin(C5_I2C_SDA, C5_I2C_SCL);
  Wire.setClock(400000);
}

static inline uint8_t I2C_writr_buff(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (len) Wire.write(buf, len);
  return Wire.endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

static inline uint8_t I2C_read_buff(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return ESP_FAIL;
  if (Wire.requestFrom(addr, len) != len) return ESP_FAIL;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return ESP_OK;
}

static inline uint8_t I2C_master_write_read_device(uint8_t addr, uint8_t *writeBuf, uint8_t writeLen,
                                                   uint8_t *readBuf, uint8_t readLen) {
  Wire.beginTransmission(addr);
  Wire.write(writeBuf, writeLen);
  if (Wire.endTransmission(false) != 0) return ESP_FAIL;
  if (Wire.requestFrom(addr, readLen) != readLen) return ESP_FAIL;
  for (uint8_t i = 0; i < readLen; i++) readBuf[i] = Wire.read();
  return ESP_OK;
}
