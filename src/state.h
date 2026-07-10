#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <nvs.h>

// ── Дескрипторы задач ────────────────────────────────────────
extern TaskHandle_t taskNRF905Handle;
extern TaskHandle_t taskCO2ReadHandle;
extern TaskHandle_t processNextionTaskHandle;
extern TaskHandle_t taskBMP280Handle;
extern TaskHandle_t taskSendDataToInfluxDBHandle;
extern TaskHandle_t taskForecasterHandle;
extern TaskHandle_t taskGetTimeHandle;
extern TaskHandle_t taskTVOCReadHandle;
extern TaskHandle_t taskNRF905TxHandle;

// ── Очередь и мьютексы ───────────────────────────────────────
extern QueueHandle_t     nrf905CmdQueue;
extern SemaphoreHandle_t i2cMutex;
extern SemaphoreHandle_t driverMutex;
extern SemaphoreHandle_t dataMutex;
extern portMUX_TYPE      mutexMux;
extern TimerHandle_t     wifiTimer;
extern nvs_handle_t      nrf905NvsHandle;
extern nvs_handle_t      settingsNvsHandle;

// ── Уличные данные (nRF905) ──────────────────────────────────
extern volatile float temperature, humidity, dewPoint;
extern volatile float uvIndex, luxLevel, pm25Level, pm10Level;
extern volatile uint8_t heaterStatus, fanStatus;

// ── Домашние данные (BME280) ─────────────────────────────────
extern volatile float pressure, homeTemp, homeHum, homeDP;
extern volatile float stationPressure;

// ── Качество воздуха ─────────────────────────────────────────
extern volatile int ppm, TVOC, AQI, ECO2;

// ── Прогноз ──────────────────────────────────────────────────
extern volatile float forecast, trend;
extern volatile int   month;

// ── Диагностика ──────────────────────────────────────────────
extern volatile uint32_t i2cResetCount, nRF905ResetCount;

// ── Время ────────────────────────────────────────────────────
extern double sunriseTime, sunsetTime;

// ── Nextion ──────────────────────────────────────────────────
enum NextionPage : uint8_t { PAGE0, PAGE1, PAGE2, PAGE3, PAGE_UNKNOWN };
extern NextionPage currentPage;

// ── WiFi config (через Nextion) ──────────────────────────────
enum WifiCfgState { WIFI_CFG_IDLE, WIFI_CFG_WAIT_SSID, WIFI_CFG_WAIT_PASS };
extern WifiCfgState wifiCfgState;
extern char wifiCfgSSID[33];
extern char wifiCfgPass[65];

// ── Калибровка ───────────────────────────────────────────────
extern volatile float tCorr;
extern volatile float altitude_m;

// ── Сетевые настройки ────────────────────────────────────────
extern char ssid[33], password[65], http_username[33], http_password[65];
extern bool useStaticIP;
extern char staticIP[16], staticGateway[16], staticSubnet[16], staticDNS[16];
extern char ntpServer[64];
extern long gmtOffset_sec;
extern char influxDBHost[64];
extern int  influxDBPort;
extern char influxDBDatabase[33];
extern double latitude, longitude;
extern int tzOffset;

// ── Периферия ────────────────────────────────────────────────
extern HardwareSerial nextion;
extern HardwareSerial mh19;

// ── Утилиты ──────────────────────────────────────────────────
template <typename T>
inline void safeDelete(T *&ptr) { if (ptr) { delete ptr; ptr = nullptr; } }
template <typename T>
inline void safeDeleteArray(T *&ptr) { if (ptr) { delete[] ptr; ptr = nullptr; } }
#define HEAP_CHECK(tag) ESP_LOGI(tag, "Heap: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap())

// ── Переменные WiFi reconnect ────────────────────────────────
extern uint8_t  wifi_attempts;
extern uint32_t last_reconnect_time;
