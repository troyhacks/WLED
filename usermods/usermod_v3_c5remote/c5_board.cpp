/*
 * c5_board.cpp - the implementation of the board's hardware.
 *
 * NOT a standalone translation unit: this file is included at the end of
 * c5_board.h, which is included by the usermod's header, which is included
 * by usermods_list.cpp. One translation unit, one copy.
 */

// ---------------------------------------------------------------------------
// The IO expander
// ---------------------------------------------------------------------------

bool C5IoExpander::begin(TwoWire &wire) {
  _wire = &wire;
  _outputs = 0xFFFF;

  // The mode first, then the output register: the vendor's order, and it
  // matters - writing the output register before the mode can leave the
  // pins in a state nobody asked for.
  if (!setMode(C5_IOEXP_MODE)) return false;
  if (!writeWord(C5_IOEXP_REG_OUTPUT, _outputs)) return false;
  return true;
}

bool C5IoExpander::writeWord(uint8_t reg, uint16_t value) {
  if (_wire == nullptr) return false;
  _wire->beginTransmission(C5_IOEXP_ADDR);
  _wire->write(reg);
  _wire->write((uint8_t)(value & 0xFF));
  _wire->write((uint8_t)((value >> 8) & 0xFF));
  return _wire->endTransmission() == 0;
}

bool C5IoExpander::readWord(uint8_t reg, uint16_t &value) {
  if (_wire == nullptr) return false;
  _wire->beginTransmission(C5_IOEXP_ADDR);
  _wire->write(reg);
  if (_wire->endTransmission(false) != 0) return false;
  if (_wire->requestFrom((uint8_t)C5_IOEXP_ADDR, (uint8_t)2) != 2) return false;
  uint8_t lo = _wire->read();
  uint8_t hi = _wire->read();
  value = (uint16_t)lo | ((uint16_t)hi << 8);
  return true;
}

bool C5IoExpander::setMode(uint16_t pinMask) {
  return writeWord(C5_IOEXP_REG_MODE, pinMask);
}

bool C5IoExpander::writeOutput(uint16_t value) {
  if (!writeWord(C5_IOEXP_REG_OUTPUT, value)) return false;
  _outputs = value;
  return true;
}

bool C5IoExpander::setPin(uint8_t pin, bool high) {
  if (pin > 15) return false;
  uint16_t v = _outputs;
  if (high) v |= (uint16_t)(1U << pin);
  else      v &= (uint16_t)~(1U << pin);
  return writeOutput(v);
}

bool C5IoExpander::readInput(uint16_t &value) {
  return readWord(C5_IOEXP_REG_INPUT, value);
}

bool C5IoExpander::getPin(uint8_t pin, bool &high) {
  if (pin > 15) return false;
  uint16_t v = 0;
  if (!readInput(v)) return false;
  high = (v & (uint16_t)(1U << pin)) != 0;
  return true;
}

bool C5IoExpander::setPwm(uint8_t percent) {
  if (_wire == nullptr) return false;
  // The vendor caps this at 97 %, and the duty is 0..255.
  if (percent > 97) percent = 97;
  _wire->beginTransmission(C5_IOEXP_ADDR);
  _wire->write(C5_IOEXP_REG_PWM);
  _wire->write((uint8_t)((percent * 255) / 100));
  return _wire->endTransmission() == 0;
}

uint16_t C5IoExpander::readAdc() {
  uint16_t v = 0;
  readWord(C5_IOEXP_REG_ADC, v);
  return v;
}

uint8_t C5IoExpander::readRtcInt() {
  uint16_t v = 0;
  readWord(C5_IOEXP_REG_RTCINT, v);
  return (uint8_t)(v & 0xFF);
}

// ---------------------------------------------------------------------------
// The IMU (QMI8658)
// ---------------------------------------------------------------------------

bool C5Imu::begin() {
  // The driver talks to the chip through the I2C glue in i2c_bsp.h, which
  // needs the bus up.
  I2C_master_Init();

  // The driver's own init: it checks the chip's ID, sets the ranges and
  // the data rates, and turns the sensors on.
  if (!qmi8658_init()) {
    _present = false;
    USER_PRINTLN("C5: no IMU at 0x6B - the console will not show any motion");
    return false;
  }
  _present = true;
  USER_PRINTLN("C5: the IMU (QMI8658) is there");
  return true;
}

bool C5Imu::read(float acc[3], float gyr[3]) {
  if (!_present) return false;

  // The driver's read leaves the numbers alone when the chip had no fresh
  // set, so the values are checked afterwards: a zero accelerometer with a
  // zero gyroscope is what the chip says when it has nothing to say.
  qmi8658_read_sensor_data(acc, gyr);
  return true;
}
