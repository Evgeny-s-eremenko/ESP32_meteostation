#pragma once
#include <Wire.h>

class HMC5883L {
public:
  bool begin(uint8_t address = 0x1E) {
    _address = address;
    Wire.beginTransmission(_address);
    if (Wire.endTransmission() != 0) return false;

    // Конфигурация: 8 выборок, 15 Гц, нормальное измерение
    writeRegister(0x00, 0x70);
    // Gain = ±1.3 Ga
    writeRegister(0x01, 0x20);
    // Continuous mode
    writeRegister(0x02, 0x00);

    return true;
  }

  bool readRaw(int16_t &x, int16_t &y, int16_t &z) {
    Wire.beginTransmission(_address);
    Wire.write(0x03); // Data Output X MSB Register
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(_address, (uint8_t)6);
    if (Wire.available() < 6) return false;

    x = (Wire.read() << 8) | Wire.read();
    z = (Wire.read() << 8) | Wire.read(); // HMC: порядок X, Z, Y
    y = (Wire.read() << 8) | Wire.read();

    return true;
  }

  float scale = 0.92f; // µT per LSB for ±1.3 Gauss gain

private:
  uint8_t _address;

  void writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(_address);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
  }
};
