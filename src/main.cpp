#include <Arduino.h>
#include "config.h"
#include "state.h"
#include "secrets.h"
#include "board_config.h"

// Модули
#include "sensors/bme280.h"
#include "sensors/ens160.h"
#include "sensors/co2.h"
#include "sensors/nrf905.h"
#include "display/nextion.h"
#include "web/server.h"
#include "web/ota.h"
#include "network/wifi.h"
#include "network/influxdb.h"
#include "network/time_sync.h"
#include "forecast/forecast.h"
#include "utils/i2c_recovery.h"
#include "utils/heap_monitor.h"
#include "utils/settings.h"

#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
#include <SparkFun_ENS160.h>
#include <SparkFun_Qwiic_Humidity_AHT20.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <SPI.h>
#include "LittleFS.h"
#include <ArduinoJson.h>
#include <Forecaster.h>
#include <sunset.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

// Forward declaration
RH_NRF905::TransmitPower getTransmitPowerFromString(const String &s);

// ── Определения глобальных переменных (state.h — extern) ─────

// WiFi и HTTP-сервер
char ssid[33]          = SECRET_WIFI_SSID;
char password[65]      = SECRET_WIFI_PASSWORD;
char http_username[33] = SECRET_HTTP_USER;
char http_password[65] = SECRET_HTTP_PASSWORD;

bool   useStaticIP       = SECRET_USE_STATIC_IP;
char   staticIP[16]      = SECRET_STATIC_IP;
char   staticGateway[16] = SECRET_STATIC_GATEWAY;
char   staticSubnet[16]  = SECRET_STATIC_SUBNET;
char   staticDNS[16]     = SECRET_STATIC_DNS;

// NTP
char   ntpServer[64]       = SECRET_NTP_SERVER;
long   gmtOffset_sec       = SECRET_TZ_OFFSET_SEC;
const int daylightOffset_sec = 0;

// InfluxDB
char influxDBHost[64]     = SECRET_INFLUX_HOST;
int  influxDBPort         = SECRET_INFLUX_PORT;
char influxDBDatabase[33] = SECRET_INFLUX_DATABASE;

// Координаты и часовой пояс
double latitude      = SECRET_LATITUDE;
double longitude     = SECRET_LONGITUDE;
int    tzOffset      = SECRET_TZ_OFFSET;

// UART
HardwareSerial nextion(2);
HardwareSerial mh19(1);

// Объекты периферии
Adafruit_BME280  bme;
SparkFun_ENS160  ens160;
AHT20            aht20;
RH_NRF905        driver(NRF905_CE, NRF905_TX_EN, NRF905_CS);
Forecaster       cond;
SunSet           sun;

// Дескрипторы задач FreeRTOS
TaskHandle_t taskNRF905Handle             = NULL;
TaskHandle_t taskCO2ReadHandle            = NULL;
TaskHandle_t processNextionTaskHandle     = NULL;
TaskHandle_t taskBMP280Handle             = NULL;
TaskHandle_t taskSendDataToInfluxDBHandle = NULL;
TaskHandle_t taskForecasterHandle         = NULL;
TaskHandle_t taskGetTimeHandle            = NULL;
TaskHandle_t taskTVOCReadHandle           = NULL;
TaskHandle_t taskNRF905TxHandle           = NULL;

// Очередь команд для nRF905
QueueHandle_t nrf905CmdQueue = NULL;

// Мьютексы
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t driverMutex;
SemaphoreHandle_t dataMutex;
portMUX_TYPE      mutexMux  = portMUX_INITIALIZER_UNLOCKED;
TimerHandle_t     wifiTimer;
nvs_handle_t      nrf905NvsHandle  = 0;
nvs_handle_t      settingsNvsHandle = 0;

// Уличные данные
volatile float temperature = 0.0f;
volatile float humidity    = 0.0f;
volatile float dewPoint    = 0.0f;
volatile float uvIndex     = 0.0f;
volatile float luxLevel    = 0.0f;
volatile float pm25Level   = 0.0f;
volatile float pm10Level   = 0.0f;
volatile uint8_t heaterStatus = 1;
volatile uint8_t fanStatus    = 0;

// Домашние данные
volatile float pressure = 0.0f;
volatile float homeTemp = 0.0f;
volatile float homeHum  = 0.0f;
volatile float homeDP   = 0.0f;
volatile float stationPressure = 0.0f;

// Качество воздуха
volatile int ppm  = 400;
volatile int TVOC = 0;
volatile int AQI  = 1;
volatile int ECO2 = 400;

// Прогноз
volatile float forecast = 0.0f;
volatile float trend    = 0.0f;
volatile int   month    = -1;

// Диагностика
volatile uint32_t i2cResetCount   = 0;
volatile uint32_t nRF905ResetCount = 0;
volatile uint32_t nrf905RxOK        = 0;
volatile uint32_t nrf905RxCorrected = 0;
volatile uint32_t nrf905RxFail      = 0;

// Время
double sunriseTime = 0.0;
double sunsetTime  = 0.0;

// Nextion
NextionPage currentPage = PAGE0;
WifiCfgState wifiCfgState = WIFI_CFG_IDLE;
char wifiCfgSSID[33] = {};
char wifiCfgPass[65] = {};

// Калибровка
volatile float tCorr      = 0.0f;
volatile float altitude_m = 61.0f;

// WiFi reconnect
uint8_t  wifi_attempts       = 0;
uint32_t last_reconnect_time = 0;

// nRF905 DR ISR (ESP32-S3)
#ifdef ESP32S3
volatile bool nrf905DataReady = false;
void IRAM_ATTR nrf905DRISR() {
  nrf905DataReady = true;
}
#endif

// ── Setup ────────────────────────────────────────────────────

void setupFileSystem() {
  if (!LittleFS.begin()) {
    ESP_LOGE("FS", "Ошибка инициализации LittleFS");
  } else {
    ESP_LOGI("FS", "LittleFS инициализирована");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  esp_log_level_set("*", ESP_LOG_VERBOSE);

  // Nextion
  nextion.begin(115200, SERIAL_8N1, RX2, TX2);
  ESP_LOGI("INIT", "Nextion инициализирован");

  // nRF905: включение питания
  pinMode(NRF905_PWR_UP_PIN, OUTPUT);
  digitalWrite(NRF905_PWR_UP_PIN, HIGH);
  SPI.begin(NRF905_SPI_SCK, NRF905_SPI_MISO, NRF_SPI_MOSI);
  ESP_LOGI("INIT", "SPI инициализирован");

  // Файловая система
  setupFileSystem();

  // CO2 UART
  mh19.begin(9600, SERIAL_8N1, RX1, TX1);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  applyWiFiConfig();
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(1000);
  IPAddress ip = WiFi.localIP();
  ESP_LOGI("WIFI", "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  // HTTP-маршруты
  setupWebServer();

  nextionRestart();

  // NVS
  if (nvs_open("nrf905", NVS_READWRITE, &nrf905NvsHandle) != ESP_OK) {
    ESP_LOGE("INIT", "Не удалось открыть NVS для настроек nRF905");
  }

  if (nvs_open("settings", NVS_READWRITE, &settingsNvsHandle) != ESP_OK) {
    ESP_LOGE("INIT", "Не удалось открыть NVS для системных настроек");
  } else {
    settingsLoadAll();
  }

  // nRF905: инициализация и загрузка настроек
  if (!driver.init()) {
    ESP_LOGE("INIT", "nRF905 не инициализирован!");
  } else {
    int ch = 175; bool band = false;
    char pwr[32] = "TransmitPower10dBm";
    if (nrf905LoadSettings(ch, band, pwr, sizeof(pwr))) {
      driver.setChannel(ch, band);
      driver.setRF(getTransmitPowerFromString(pwr));
    } else {
      driver.setChannel(175, false);
      driver.setRF(RH_NRF905::TransmitPower10dBm);
    }
    ESP_LOGI("INIT", "nRF905 готов");
  }

#ifdef ESP32S3
  pinMode(NRF905_DR, INPUT_PULLUP);
  pinMode(NRF905_AM, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(NRF905_DR), nrf905DRISR, RISING);
  ESP_LOGI("INIT", "nRF905 DR/AM прерывания настроены (GPIO %d/%d)", NRF905_DR, NRF905_AM);
#endif

  // I2C датчики
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!bme.begin(0x76)) {
    ESP_LOGE("INIT", "BME280 не найден!");
  } else {
    ESP_LOGI("INIT", "BME280 OK");
  }

  if (!ens160.begin()) {
    ESP_LOGE("INIT", "ENS160 не найден!");
  } else {
    ens160.setOperatingMode(SFE_ENS160_RESET);
    delay(100);
    ens160.setOperatingMode(SFE_ENS160_STANDARD);
    ESP_LOGI("INIT", "ENS160 OK, флаг: %d", ens160.getFlags());
  }

  if (!aht20.begin()) {
    ESP_LOGE("INIT", "AHT20 не найден!");
  } else {
    ESP_LOGI("INIT", "AHT20 OK");
  }

  // NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Forecaster
  cond.begin();
  cond.setH((int)altitude_m);

  // Мьютексы
  i2cMutex = xSemaphoreCreateMutex();
  if (!i2cMutex)    ESP_LOGE("INIT", "Ошибка создания i2cMutex!");
  driverMutex = xSemaphoreCreateMutex();
  if (!driverMutex) ESP_LOGE("INIT", "Ошибка создания driverMutex!");
  dataMutex = xSemaphoreCreateMutex();
  if (!dataMutex) ESP_LOGE("INIT", "Ошибка создания dataMutex!");
  nrf905CmdQueue = xQueueCreate(NRF905_CMD_QUEUE_LEN, NRF905_CMD_LEN);
  if (!nrf905CmdQueue) ESP_LOGE("INIT", "Ошибка создания очереди nRF905 TX!");

  // Задачи FreeRTOS
  xTaskCreate(taskNRF905,             "NRF905 Receiver",  4096, NULL, 5, &taskNRF905Handle);
  xTaskCreate(taskNRF905Tx,           "NRF905 TX",        2048, NULL, 4, &taskNRF905TxHandle);
  xTaskCreate(taskBMP280,             "BMP280 Sensor",    2048, NULL, 4, &taskBMP280Handle);
  xTaskCreate(taskCO2Read,            "CO2 read task",    2048, NULL, 3, &taskCO2ReadHandle);
  xTaskCreate(taskTVOCRead,           "ENS160 read task", 4096, NULL, 2, &taskTVOCReadHandle);
  xTaskCreate(taskGetTime,            "Get NTP Time",     4096, NULL, 2, &taskGetTimeHandle);
  xTaskCreate(taskSendDataToInfluxDB, "InfluxDBTask",     4096, NULL, 6, &taskSendDataToInfluxDBHandle);
  xTaskCreate(taskForecast,           "Forecast task",    2048, NULL, 1, &taskForecasterHandle);
  xTaskCreate(processNextionTask,     "Nextion",          4096, NULL, 3, &processNextionTaskHandle);
  xTaskCreatePinnedToCore(wifi_monitor_task, "WiFiMonitor", 2048, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(heap_monitor_task, "HeapMonitor", 2048, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelete(NULL);
}
