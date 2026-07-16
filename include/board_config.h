#pragma once
#include <Arduino.h>

// ── ESP32-S3-WROOM-1 N16R8 ──────────────────────────────────
// Доступные GPIO: 0-21, 38-42, 45-48
// Заняты flash/PSRAM: 22-37 (22-34 не на пинах модуля, 35-37 = Octal SPI PSRAM)
// USB: 19-20 (оставляем свободными, если нет USB-периферии)
#ifdef ESP32S3

  // SPI → nRF905 (SPI2/FSPI)
  #define NRF905_SPI_SCK    12
  #define NRF905_SPI_MISO   13
  #define NRF_SPI_MOSI      11
  #define NRF905_CE         10
  #define NRF905_TX_EN      9
  #define NRF905_CS         8
  // GPIO46 — strapping pin (boot mode/ROM print), избегаем
  #define NRF905_PWR_UP_PIN 41

  // nRF905 статусные пины (только S3 — подключены в новой плате)
  #define NRF905_DR         4   // Data Ready — прерывание на вход пакета
  #define NRF905_AM         5   // Address Match — прерывание на совпадение адреса

  // I2C (Bus 0) — GPIO 1/2 безопасны (не strapping pins на S3)
  #define I2C_SDA           1
  #define I2C_SCL           2

  // UART2 → Nextion
  #define RX2               16
  #define TX2               17

  // UART1 → MH-Z19 (CO2)
  // GPIO 32/33 не существуют на S3!
  // GPIO 19/20 используются как USB — выбираем 38/39
  #define RX1               38
  #define TX1               39

// ── ESP32 (classic) ────────────────────────────────────────
#else

  // SPI → nRF905
  #define NRF905_SPI_SCK    14
  #define NRF905_SPI_MISO   12
  #define NRF_SPI_MOSI      13
  #define NRF905_CE         27
  #define NRF905_TX_EN      25
  #define NRF905_CS         15
  #define NRF905_PWR_UP_PIN 26

  // I2C (Bus 0)
  #define I2C_SDA           21
  #define I2C_SCL           22

  // UART2 → Nextion
  #define RX2               16
  #define TX2               17

  // UART1 → MH-Z19 (CO2)
  #define RX1               32
  #define TX1               33

#endif
