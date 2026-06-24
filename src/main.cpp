#include <Arduino.h>
#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
#include <SparkFun_ENS160.h>
#include <SparkFun_Qwiic_Humidity_AHT20.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "LittleFS.h"
#include <HTTPClient.h>
#include <time.h>
#include <Forecaster.h>
#include <sunset.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <esp_log.h>
#include <math.h>
#include "secrets.h"

// ─────────────────────────────────────────────────────────────
//  WiFi и HTTP-сервер
// ─────────────────────────────────────────────────────────────

const char *ssid          = SECRET_WIFI_SSID;
const char *password      = SECRET_WIFI_PASSWORD;
const char *http_username = SECRET_HTTP_USER;
const char *http_password = SECRET_HTTP_PASSWORD;

// ─────────────────────────────────────────────────────────────
//  NTP
// ─────────────────────────────────────────────────────────────

const char *ntpServer          = SECRET_NTP_SERVER;
const long   gmtOffset_sec     = SECRET_TZ_OFFSET_SEC;
const int    daylightOffset_sec = 0;

// ─────────────────────────────────────────────────────────────
//  InfluxDB
// ─────────────────────────────────────────────────────────────

const char *influxDBHost     = SECRET_INFLUX_HOST;
const int   influxDBPort     = SECRET_INFLUX_PORT;
const char *influxDBDatabase = SECRET_INFLUX_DATABASE;

// ─────────────────────────────────────────────────────────────
//  Пины nRF905 (SPI)
// ─────────────────────────────────────────────────────────────

#define NRF905_SPI_SCK    14
#define NRF905_SPI_MISO   12
#define NRF_SPI_MOSI      13
#define NRF905_CE         27
#define NRF905_TX_EN      25
#define NRF905_CS         15
#define NRF905_PWR_UP_PIN 26  // Принудительный сброс nRF905

// ─────────────────────────────────────────────────────────────
//  Коды статусов (должны совпадать с передатчиком STM32)
// ─────────────────────────────────────────────────────────────

#define ST_NORMAL  0x01
#define ST_HEATER  0x02
#define ST_COOLING 0x03
#define ST_FAN_OFF 0x00
#define ST_FAN_ON  0x01

// ─────────────────────────────────────────────────────────────
//  Координаты и часовой пояс (для расчёта восхода/заката)
// ─────────────────────────────────────────────────────────────

const double lat      = SECRET_LATITUDE;
const double lon      = SECRET_LONGITUDE;
const int    tzOffset = SECRET_TZ_OFFSET;

// ─────────────────────────────────────────────────────────────
//  UART2 → Nextion | UART1 → MH-Z19 (CO2)
// ─────────────────────────────────────────────────────────────

HardwareSerial nextion(2);
#define RX2 16
#define TX2 17

HardwareSerial mh19(1);
#define RX1 32
#define TX1 33

// ─────────────────────────────────────────────────────────────
//  Объекты периферии
// ─────────────────────────────────────────────────────────────

Adafruit_BME280  bme;                                     // Давление, T, H (внутри)
SparkFun_ENS160  ens160;                                  // TVOC / eCO2
AHT20            aht20;                                   // Компенсация T и H для ENS160
RH_NRF905        driver(NRF905_CE, NRF905_TX_EN, NRF905_CS);
AsyncWebServer   server(80);
AsyncWebSocket   webSocket("/ws");                        // Статус задач
AsyncWebSocket   webSocket1("/ws1");                      // Время восхода/заката
Forecaster       cond;
SunSet           sun;

// Статусы уличного блока, принятые по nRF905
volatile uint8_t heaterStatus = 1;
volatile uint8_t fanStatus    = 0;

// ─────────────────────────────────────────────────────────────
//  Дескрипторы задач FreeRTOS
// ─────────────────────────────────────────────────────────────

TaskHandle_t taskNRF905Handle             = NULL;
TaskHandle_t taskCO2ReadHandle            = NULL;
TaskHandle_t processNextionTaskHandle     = NULL;
TaskHandle_t taskBMP280Handle             = NULL;
TaskHandle_t taskSendDataToInfluxDBHandle = NULL;
TaskHandle_t taskForecasterHandle         = NULL;
TaskHandle_t taskGetTimeHandle            = NULL;
TaskHandle_t taskTVOCReadHandle           = NULL;

// ─────────────────────────────────────────────────────────────
//  Примитивы синхронизации
// ─────────────────────────────────────────────────────────────

SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t driverMutex;
portMUX_TYPE      mutexMux  = portMUX_INITIALIZER_UNLOCKED;
TimerHandle_t     wifiTimer;

// ─────────────────────────────────────────────────────────────
//  Глобальные переменные датчиков
// ─────────────────────────────────────────────────────────────

// Уличные данные (принимаются по nRF905 от STM32-передатчика)
volatile float temperature = 0.0f;
volatile float humidity    = 0.0f;
volatile float dewPoint    = 0.0f;
volatile float uvIndex     = 0.0f;
volatile float luxLevel    = 0.0f;
volatile float pm25Level   = 0.0f;
volatile float pm10Level   = 0.0f;

// Домашние данные (BME280)
volatile float pressure = 0.0f;
volatile float homeTemp = 0.0f;
volatile float homeHum  = 0.0f;
volatile float homeDP   = 0.0f;

// Качество воздуха (ENS160)
volatile int ppm  = 400;
volatile int TVOC = 0;
volatile int AQI  = 1;
volatile int ECO2 = 400;

// Прогноз погоды
float forecast   = 0.0f;
volatile float trend = 0.0f;
int   month      = -1;

// Счётчики аварийных сбросов
volatile uint32_t i2cResetCount   = 0;
volatile uint32_t nRF905ResetCount = 0;

// Время восхода и заката (минуты с полуночи, вычисляются раз в сутки)
double sunriseTime = 0.0;
double sunsetTime  = 0.0;

// ─────────────────────────────────────────────────────────────
//  Nextion: текущая активная страница
// ─────────────────────────────────────────────────────────────

String currentPage = "page0";

// ─────────────────────────────────────────────────────────────
//  Параметры переподключения WiFi
// ─────────────────────────────────────────────────────────────

const uint8_t  MAX_ATTEMPTS_PER_CYCLE = 3;
const uint8_t  MAX_CYCLES             = 3;   // Итого до 9 попыток, затем перезагрузка
const uint32_t SHORT_COOLDOWN         = 30000;   // мс между попытками в цикле
const uint32_t LONG_COOLDOWN          = 300000;  // мс между циклами

uint8_t  wifi_attempts       = 0;
uint32_t last_reconnect_time = 0;

// ─────────────────────────────────────────────────────────────
//  Прототипы функций
// ─────────────────────────────────────────────────────────────

bool  isTaskActive(TaskHandle_t taskHandle);
void  checkMutex();
void  resetI2CBus();
void  resetNRF905();
void  nextionRestart();
void  reconnectWiFi();
void  sendTaskStateUpdate();
float calculateDewPoint(float temp, float hum);
float calculatehomeDP(float temp, float hum);
void  processNextionTask(void *pvParameters);
void  taskForecast(void *pvParameters);
void  taskCO2Read(void *pvParameters);
void  taskGetTime(void *pvParameters);
void  taskNRF905(void *pvParameters);
void  taskBMP280(void *pvParameters);
void  taskTVOCRead(void *pvParameters);
void  taskSendDataToInfluxDB(void *pvParameters);

// ─────────────────────────────────────────────────────────────
//  Вспомогательные вычисления
// ─────────────────────────────────────────────────────────────

// Давление насыщенного пара по Магнусу (для коррекции влажности BME280)
float es(float tempC) {
  return 6.112f * expf((17.67f * tempC) / (tempC + 243.5f));
}

// Точка росы по формуле Магнуса (для уличных данных)
float calculateDewPoint(float temp, float hum) {
  const float a = 17.27f, b = 237.7f;
  float alpha = ((a * temp) / (b + temp)) + logf(hum / 100.0f);
  return (b * alpha) / (a - alpha);
}

// Точка росы для домашнего датчика
float calculatehomeDP(float temp, float hum) {
  const float a = 17.27f, b = 237.7f;
  float alpha = ((a * temp) / (b + temp)) + logf(hum / 100.0f);  // исправлен баг: b+temp, не b+hum
  return (b * alpha) / (a - alpha);
}

// ─────────────────────────────────────────────────────────────
//  Файловая система LittleFS
// ─────────────────────────────────────────────────────────────

void setupFileSystem() {
  if (!LittleFS.begin()) {
    ESP_LOGE("FS", "Ошибка инициализации LittleFS");
  } else {
    ESP_LOGI("FS", "LittleFS инициализирована");
  }
}

// ─────────────────────────────────────────────────────────────
//  HTTP-обработчики
// ─────────────────────────────────────────────────────────────

// Проверка Basic Auth перед доступом к защищённым эндпоинтам
bool isAuthenticated(AsyncWebServerRequest *request) {
  if (!request->authenticate(http_username, http_password)) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

// Отдаёт текущие показания всех датчиков в формате JSON
void handleGraphData(AsyncWebServerRequest *request) {
  StaticJsonDocument<384> doc;

  doc["temperature"] = temperature;
  doc["humidity"]    = humidity;
  doc["dewPoint"]    = dewPoint;
  doc["pressure"]    = pressure;
  doc["homeTemp"]    = homeTemp;
  doc["homeHum"]     = homeHum;
  doc["homeDP"]      = homeDP;
  doc["forecast"]    = forecast;
  doc["trend"]       = trend;
  doc["CO2"]         = ppm;
  doc["TVOC"]        = TVOC;
  doc["LUX"]         = luxLevel;
  doc["PM2.5"]       = pm25Level;
  doc["PM10"]        = pm10Level;
  doc["UV"]          = uvIndex;
  doc["FAN"]         = fanStatus;
  doc["HEAT"]        = heaterStatus;

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}

void handleRoot(AsyncWebServerRequest *request) {
  if (LittleFS.exists("/index.html")) {
    request->send(LittleFS, "/index.html", "text/html");
  } else {
    request->send(404, "text/plain", "index.html not found");
  }
}

void handleAdmin(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(LittleFS, "/admin.html", "text/html");
}

void handleAbout(AsyncWebServerRequest *request) {
  if (LittleFS.exists("/about.html")) {
    request->send(LittleFS, "/about.html", "text/html");
  } else {
    request->send(404, "text/plain", "about.html not found");
  }
}

void handleUpdateForm(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  if (LittleFS.exists("/updateform.html")) {
    request->send(LittleFS, "/updateform.html", "text/html");
  } else {
    request->send(404, "text/plain", "updateform.html not found");
  }
}

// Приём бинарного файла прошивки или файловой системы (OTA)
void handleUpdateUpload(AsyncWebServerRequest *request, String filename,
                        size_t index, uint8_t *data, size_t len, bool final) {
  if (!isAuthenticated(request)) {
    request->send(401, "text/plain", "Unauthorized");
    return;
  }

  if (index == 0) {
    ESP_LOGW("OTA", "Начало обновления: %s", filename.c_str());
    int updateType = (filename.indexOf("littlefs") >= 0) ? U_SPIFFS : U_FLASH;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateType)) {
      Update.printError(Serial);
      request->send(500, "text/plain", "Failed to start update");
      return;
    }
  }

  if (len > 0 && Update.write(data, len) != len) {
    Update.printError(Serial);
    request->send(500, "text/plain", "Write error");
    return;
  }

  if (final) {
    if (!Update.end(true)) {
      Update.printError(Serial);
      request->send(500, "text/plain", "Update failed");
      return;
    }
    ESP_LOGW("OTA", "Обновление завершено: %u байт", index + len);
  }
}

void handleUpdateEnd(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
  if (!Update.hasError()) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
  }
}

void handleRestart(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(200, "text/plain", "ESP32 restarting...");
  ESP_LOGW("SYS", "Перезагрузка по команде из веб-интерфейса");
  nextionRestart();
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  ESP.restart();
}

void handleRestartFromNextion() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  ESP.restart();
}

// Системная информация: uptime, модель чипа, RSSI, RAM, стек задач
void getSystemInfo(char *buffer, size_t len) {
  unsigned long uptime  = millis() / 1000;
  int days    = uptime / 86400;
  int hours   = (uptime % 86400) / 3600;
  int minutes = (uptime % 3600) / 60;
  int seconds = uptime % 60;

  char uptimeStr[20];
  snprintf(uptimeStr, sizeof(uptimeStr), "%dd %02dh %02dm %02ds",
           days, hours, minutes, seconds);

  snprintf(buffer, len,
           "Uptime: %s\nChip model: %s\nChip rev.: %d\n"
           "WiFi RSSI: %d dBm\nIP address: %s\n"
           "Free Heap: %d bytes\n"
           "WebServer free stack: %d bytes\n"
           "InfluxDB free stack: %d bytes\n",
           uptimeStr,
           ESP.getChipModel(), ESP.getChipRevision(),
           WiFi.RSSI(),
           WiFi.localIP().toString().c_str(),
           ESP.getFreeHeap(),
           uxTaskGetStackHighWaterMark(NULL),
           uxTaskGetStackHighWaterMark(taskSendDataToInfluxDBHandle));
}

void handleSysInfo(AsyncWebServerRequest *request) {
  static char info[256];
  getSystemInfo(info, sizeof(info));
  request->send(200, "text/plain", info);
}

// Расшифровка режима ENS160
const char *modeToString(uint8_t mode) {
  switch (mode) {
    case 0x00: return "DEEP_SLEEP";
    case 0x01: return "IDLE";
    case 0x02: return "STANDARD";
    case 0xF0: return "RESET";
    default:   return "UNKNOWN";
  }
}

// Статус датчиков на шине I2C
void getBME280Status(char *buffer, size_t len) {
  size_t offset = 0;
  checkMutex();

  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    offset += snprintf(buffer + offset, len - offset,
                       bme.begin(0x76) ? "BME280: Found\nTemp: %.2f °C\nHumidity: %.2f %%\nPressure: %.2f hPa\n"
                                       : "BME280: Not Found\n",
                       bme.readTemperature(), bme.readHumidity(),
                       bme.readPressure() / 100.0f);

    if (!ens160.begin()) {
      offset += snprintf(buffer + offset, len - offset, "ENS160: Not Found\n");
    } else {
      offset += snprintf(buffer + offset, len - offset,
                         "ENS160: Found\nMode: %s\nStatus: %s\nI2C Resets: %u\n",
                         modeToString(ens160.getOperatingMode()),
                         ens160.getOperationError() ? "Error" : "OK",
                         i2cResetCount);
    }
    xSemaphoreGive(i2cMutex);
  } else {
    snprintf(buffer, len, "i2cMutex занят (задача: %s)\n", pcTaskGetTaskName(NULL));
    ESP_LOGE("MUTEX", "Таймаут i2cMutex в задаче %s", pcTaskGetTaskName(NULL));
    resetI2CBus();
  }
}

void handleBMEInfo(AsyncWebServerRequest *request) {
  static char statusBuffer[256];
  getBME280Status(statusBuffer, sizeof(statusBuffer));
  request->send_P(200, "text/plain", statusBuffer);
}

// Статус nRF905: регистры конфигурации, канал, частота, мощность
void getNRF905Status(char *buffer, size_t bufferSize) {
  char    temp[32];
  uint8_t config[10];
  int     pos = 0;

  uint8_t status_reg = driver.spiBurstReadRegister(RH_NRF905_REG_W_CONFIG, config, 10);
  pos += snprintf(buffer, bufferSize, "Status: 0x%02X\n", status_reg);

  if (status_reg & 0x20) pos += snprintf(buffer + pos, bufferSize - pos, "[DR] Data Ready\n");
  if (status_reg & 0x80) pos += snprintf(buffer + pos, bufferSize - pos, "[AM] Address Match\n");
  pos += snprintf(buffer + pos, bufferSize - pos,
                  (status_reg & 0x40) ? "[CRC_ERR]\n" : "[CRC_OK]\n");

  uint8_t band_bit = config[1] & RH_NRF905_CONFIG_1_HFREQ_PLL;
  float   freq     = 422.4f + (config[0] / 10.0f);
  if (band_bit) freq *= 2.0f;

  const char *pwr_str[] = {"-10 dBm", "-2 dBm", "+6 dBm", "+10 dBm"};
  uint8_t     pwr       = (config[1] & RH_NRF905_CONFIG_1_PA_PWR) >> 2;

  pos += snprintf(buffer + pos, bufferSize - pos,
                  "Channel: %d\nFreq: %.3f MHz\nTX Power: %s\n",
                  config[0], freq, pwr_str[pwr]);

  pos += snprintf(buffer + pos, bufferSize - pos, "RAW Config: ");
  for (int i = 0; i < 10; i++) {
    snprintf(temp, sizeof(temp), "%02X ", config[i]);
    strncat(buffer, temp, bufferSize - strlen(buffer) - 1);
  }
  snprintf(temp, sizeof(temp), "\nnRF905 Resets: %u\n", nRF905ResetCount);
  strncat(buffer, temp, bufferSize - strlen(buffer) - 1);
}

void handlenRFInfo(AsyncWebServerRequest *request) {
  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    static char status[256];
    getNRF905Status(status, sizeof(status));
    request->send_P(200, "text/plain", status);
    xSemaphoreGive(driverMutex);
  }
}

// Применение настроек nRF905 из веб-формы (канал, диапазон, мощность)
RH_NRF905::TransmitPower getTransmitPowerFromString(const String &s) {
  if (s == "TransmitPowerm10dBm") return RH_NRF905::TransmitPowerm10dBm;
  if (s == "TransmitPowerm2dBm")  return RH_NRF905::TransmitPowerm2dBm;
  if (s == "TransmitPower6dBm")   return RH_NRF905::TransmitPower6dBm;
  return RH_NRF905::TransmitPower10dBm;
}

void handleSetNRF905(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;

  if (request->hasParam("channel", true) &&
      request->hasParam("band",    true) &&
      request->hasParam("power",   true)) {
    int    channel  = request->getParam("channel", true)->value().toInt();
    bool   band     = (request->getParam("band",   true)->value() == "true");
    String powerStr = request->getParam("power",   true)->value();

    ESP_LOGI("NRF905", "Новые настройки: канал=%d band=%s мощность=%s",
             channel, band ? "hiband" : "lowband", powerStr.c_str());

    if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
      driver.setChannel(channel, band);
      driver.setRF(getTransmitPowerFromString(powerStr));
      xSemaphoreGive(driverMutex);
    }
    request->send(200, "text/plain", "nRF905 settings applied");
  } else {
    request->send(400, "text/plain", "Invalid parameters");
  }
}

void handleNRFReset(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  resetNRF905();
  request->send(200, "text/plain", "nRF905 reset done.");
}

// ─────────────────────────────────────────────────────────────
//  Управление состоянием задач (JSON + WebSocket)
// ─────────────────────────────────────────────────────────────

// Возвращает true, если задача создана и не завершена/приостановлена
bool isTaskActive(TaskHandle_t taskHandle) {
  if (taskHandle == NULL) return false;
  eTaskState state = eTaskGetState(taskHandle);
  return (state == eRunning || state == eReady || state == eBlocked);
}

// Формирует JSON со статусами всех управляемых задач
void buildTaskStateJson(char *buffer, size_t bufferSize) {
  snprintf(buffer, bufferSize,
           "{\"nRF905\":%s,\"CO2\":%s,\"nextion\":%s,"
           "\"BMP280\":%s,\"InfluxDB\":%s,\"Forecaster\":%s,"
           "\"NTP\":%s,\"TVOC\":%s}",
           isTaskActive(taskNRF905Handle)             ? "true" : "false",
           isTaskActive(taskCO2ReadHandle)            ? "true" : "false",
           isTaskActive(processNextionTaskHandle)     ? "true" : "false",
           isTaskActive(taskBMP280Handle)             ? "true" : "false",
           isTaskActive(taskSendDataToInfluxDBHandle) ? "true" : "false",
           isTaskActive(taskForecasterHandle)         ? "true" : "false",
           isTaskActive(taskGetTimeHandle)            ? "true" : "false",
           isTaskActive(taskTVOCReadHandle)           ? "true" : "false");
}

void handleGetTasksState(AsyncWebServerRequest *request) {
  char jsonBuffer[256];
  buildTaskStateJson(jsonBuffer, sizeof(jsonBuffer));
  request->send(200, "application/json", jsonBuffer);
}

// Рассылает статус задач всем подключённым WebSocket-клиентам (/ws)
void sendTaskStateUpdate() {
  char jsonBuffer[256];
  buildTaskStateJson(jsonBuffer, sizeof(jsonBuffer));
  webSocket.textAll(jsonBuffer);
}

// Отправляет текущее время и данные восхода/заката по /ws1
void sendTimeData() {
  if (webSocket1.count() == 0) return;

  StaticJsonDocument<128> json;
  json["nowTime"]     = time(nullptr);
  json["sunriseTime"] = sunriseTime * 60;
  json["sunsetTime"]  = sunsetTime  * 60;

  static char jsonBuffer[160];
  size_t len = serializeJson(json, jsonBuffer, sizeof(jsonBuffer));
  if (len >= sizeof(jsonBuffer) - 1) {
    ESP_LOGE("WS", "JSON-буфер времени переполнен!");
    return;
  }
  webSocket1.textAll(jsonBuffer, len);
}

// ─────────────────────────────────────────────────────────────
//  WebSocket-обработчики
// ─────────────────────────────────────────────────────────────

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    ESP_LOGI("WS", "Клиент #%u подключён (%s)", client->id(),
             client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    ESP_LOGI("WS", "Клиент #%u отключён", client->id());
  }
}

void onWsEvent1(AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    ESP_LOGI("WS1", "Клиент подключён к /ws1");
  } else if (type == WS_EVT_DATA) {
    if (strncmp((char *)data, "getTime", len) == 0) {
      sendTimeData();
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  Nextion: вспомогательные функции
// ─────────────────────────────────────────────────────────────

// Завершает команду тремя байтами 0xFF (обязательно по протоколу Nextion)
void nextionFin() {
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void nextionWakeUP()  { nextion.print("sleep=0"); nextionFin(); }
void nextionSleep()   { nextion.print("sleep=1"); nextionFin(); }
void nextionRestart() { nextion.print("rest");    nextionFin(); }

// Синхронизирует визуальное состояние кнопки-тумблера на Nextion
void syncButtonState(int buttonId, TaskHandle_t taskHandle) {
  nextion.printf("bt%d.val=%d", buttonId, isTaskActive(taskHandle) ? 1 : 0);
  nextionFin();
}

// ─────────────────────────────────────────────────────────────
//  Мьютекс I2C: ленивое создание с защитой критической секцией
// ─────────────────────────────────────────────────────────────

void checkMutex() {
  taskENTER_CRITICAL(&mutexMux);
  if (i2cMutex == NULL) {
    i2cMutex = xSemaphoreCreateMutex();
    if (i2cMutex == NULL) {
      ESP_LOGE("MUTEX", "Не удалось создать i2cMutex! Задача: %s", pcTaskGetTaskName(NULL));
    } else {
      ESP_LOGI("MUTEX", "i2cMutex создан в задаче: %s", pcTaskGetTaskName(NULL));
    }
  }
  taskEXIT_CRITICAL(&mutexMux);
}

// ─────────────────────────────────────────────────────────────
//  Аварийный сброс шины I2C (при зависании датчика)
// ─────────────────────────────────────────────────────────────

void resetI2CBus() {
  i2cResetCount++;
  vSemaphoreDelete(i2cMutex);
  i2cMutex = NULL;
  ESP_LOGE("SYS", "Сброс шины I2C (сброс #%u)...", i2cResetCount);

  // Генерируем 10 тактовых импульсов на SCL для освобождения застрявшего устройства
  pinMode(22, OUTPUT);
  pinMode(21, INPUT_PULLUP);
  for (int i = 0; i < 10; i++) {
    digitalWrite(22, HIGH); delayMicroseconds(5);
    digitalWrite(22, LOW);  delayMicroseconds(5);
  }
  digitalWrite(22, HIGH);
  vTaskDelay(pdMS_TO_TICKS(10));

  Wire.end();
  Wire.begin(21, 22);
  checkMutex();
  ESP_LOGW("SYS", "Шина I2C сброшена, мьютекс пересоздан");

  ens160.setOperatingMode(SFE_ENS160_RESET);
  if (!bme.begin(0x76))  ESP_LOGE("SYS", "BME280 не найден после сброса!");
  if (!ens160.begin())   ESP_LOGE("SYS", "ENS160 не найден после сброса!");
  if (!aht20.begin())    ESP_LOGE("SYS", "AHT20 не найден после сброса!");
  vTaskDelay(pdMS_TO_TICKS(100));
}

// ─────────────────────────────────────────────────────────────
//  Сброс и переинициализация nRF905
// ─────────────────────────────────────────────────────────────

void resetNRF905() {
  ESP_LOGW("NRF905", "Сброс nRF905...");
  digitalWrite(NRF905_PWR_UP_PIN, LOW);
  vTaskDelay(pdMS_TO_TICKS(200));
  digitalWrite(NRF905_PWR_UP_PIN, HIGH);
  nRF905ResetCount++;
  vTaskDelay(pdMS_TO_TICKS(100));

  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    if (driver.init()) {
      driver.setChannel(175, false);
      driver.setRF(RH_NRF905::TransmitPowerm10dBm);
      ESP_LOGW("NRF905", "nRF905 переинициализирован (сброс #%u)", nRF905ResetCount);
    } else {
      ESP_LOGE("NRF905", "Ошибка переинициализации nRF905!");
    }
    xSemaphoreGive(driverMutex);
  }
}

// ─────────────────────────────────────────────────────────────
//  Отправка данных в InfluxDB (Line Protocol)
// ─────────────────────────────────────────────────────────────

void sendDataToInfluxDB() {
  WiFiClient wifiClient;
  HTTPClient http;

  char influxDBLine[512];

  // Копируем volatile-переменные статусов в локальные (атомарно)
  uint8_t curHeater = heaterStatus;
  uint8_t curFan    = fanStatus;

  // При активном нагреве уличные T/H недостоверны — не отправляем
  bool sendClimate = (curHeater != ST_HEATER && curHeater != ST_COOLING);

  int         offset     = snprintf(influxDBLine, sizeof(influxDBLine), "weather,location=home ");
  int         baseOffset = offset;
  const char *sep        = "";

  // Уличный климат
  if (sendClimate) {
    if (temperature != 0.0f) {
      offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                         "%stemperature=%.2f", sep, temperature);
      sep = ",";
    }
    if (humidity != 0.0f) {
      offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                         "%shumidity=%.2f", sep, humidity);
      sep = ",";
    }
    if (!isnan(dewPoint) && dewPoint != 0.0f) {
      offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                         "%sdewPoint=%.2f", sep, dewPoint);
      sep = ",";
    }
  }

  // Атмосферное давление
  if (pressure != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%spressure=%.2f", sep, pressure);
    sep = ",";
  }

  // Прогноз и тренд
  if (forecast >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sforecast=%.2f", sep, forecast);
    sep = ",";
  }
  if (trend >= -30.0f && trend <= 30.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%strend=%.2f", sep, trend);
    sep = ",";
  }

  // Домашний климат
  if (homeTemp != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%shomeTemp=%.2f", sep, homeTemp);
    sep = ",";
  }
  if (homeHum != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%shomeHum=%.2f", sep, homeHum);
    sep = ",";
  }
  if (!isnan(homeDP) && homeDP != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%shomeDP=%.2f", sep, homeDP);
    sep = ",";
  }

  // Качество воздуха (ENS160 + MH-Z19)
  // Примечание: InfluxDB уже хранит эти поля как float, поэтому %.0f без суффикса 'i'
  if (ppm != 0) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sCO2=%.0f", sep, (float)ppm);
    sep = ",";
  }
  if (TVOC != -1) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sTVOC=%.0f", sep, (float)TVOC);
    sep = ",";
  }
  if (AQI != -1) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sAQI=%.0f", sep, (float)AQI);
    sep = ",";
  }
  if (ECO2 != -1) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sECO2=%.0f", sep, (float)ECO2);
    sep = ",";
  }

  // Пыль (PM)
  if (pm25Level >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%spm25Level=%.2f", sep, pm25Level);
    sep = ",";
  }
  if (pm10Level >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%spm10Level=%.2f", sep, pm10Level);
    sep = ",";
  }

  // Свет и УФ
  if (luxLevel >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sluxLevel=%.2f", sep, luxLevel);
    sep = ",";
  }
  if (uvIndex >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%suvIndex=%.2f", sep, uvIndex);
    sep = ",";
  }

  // Статусы нагревателя и вентилятора уличного блока
  if (curHeater >= 1 && curHeater <= 3) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sheaterStatus=%.0f", sep, (float)curHeater);
    sep = ",";
  }
  if (curFan == ST_FAN_OFF || curFan == ST_FAN_ON) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sfanStatus=%.0f", sep, (float)curFan);
    sep = ",";
  }

  // Не отправляем пустой measurement
  if (offset == baseOffset) return;

  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d/write?db=%s",
           influxDBHost, influxDBPort, influxDBDatabase);

  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(influxDBLine);

  if (code > 0) {
    ESP_LOGI("InfluxDB", "HTTP %d", code);
    if (code != 204) {
      ESP_LOGD("InfluxDB", "Ответ: %s", http.getString().c_str());
    }
  } else {
    ESP_LOGE("InfluxDB", "Ошибка: %s", http.errorToString(code).c_str());
  }
  http.end();
}

// ─────────────────────────────────────────────────────────────
//  Переключатели задач (вызываются из веб-интерфейса и Nextion)
// ─────────────────────────────────────────────────────────────

// Макрос для стандартного тела переключателя: suspend ↔ resume/create
#define TASK_SWITCH_BODY(handle, createFn, name, stackSize, prio)   \
  if (isTaskActive(handle)) {                                         \
    vTaskSuspend(handle);                                             \
    ESP_LOGD("SYS", "%s: остановлена", name);                        \
  } else {                                                            \
    if (handle == NULL) {                                             \
      xTaskCreate(createFn, name, stackSize, NULL, prio, &handle);   \
      ESP_LOGD("SYS", "%s: создана", name);                          \
    } else {                                                          \
      vTaskResume(handle);                                            \
      ESP_LOGD("SYS", "%s: возобновлена", name);                     \
    }                                                                 \
  }                                                                   \
  sendTaskStateUpdate();

void switchTaskNRF905() {
  TASK_SWITCH_BODY(taskNRF905Handle, taskNRF905, "NRF905 Receiver", 4096, 5)
}

void switchTaskBMP280() {
  TASK_SWITCH_BODY(taskBMP280Handle, taskBMP280, "BMP280 Sensor", 2048, 4)
}

void switchTaskForecaster() {
  TASK_SWITCH_BODY(taskForecasterHandle, taskForecast, "Forecast task", 2048, 1)
}

void switchTaskNTP() {
  TASK_SWITCH_BODY(taskGetTimeHandle, taskGetTime, "Get NTP Time", 4096, 2)
}

void switchTaskInfluxDB() {
  TASK_SWITCH_BODY(taskSendDataToInfluxDBHandle, taskSendDataToInfluxDB, "InfluxDBTask", 4096, 6)
}

// CO2 требует отдельной инициализации UART
void switchTaskCO2Read() {
  if (isTaskActive(taskCO2ReadHandle)) {
    mh19.end();
    vTaskSuspend(taskCO2ReadHandle);
    ESP_LOGD("SYS", "CO2 Task: остановлена");
  } else {
    mh19.begin(9600, SERIAL_8N1, RX1, TX1);
    if (taskCO2ReadHandle == NULL) {
      xTaskCreate(taskCO2Read, "CO2 read task", 2048, NULL, 3, &taskCO2ReadHandle);
      ESP_LOGD("SYS", "CO2 Task: создана");
    } else {
      vTaskResume(taskCO2ReadHandle);
      ESP_LOGD("SYS", "CO2 Task: возобновлена");
    }
  }
  sendTaskStateUpdate();
}

// Nextion требует отправки команд sleep/wakeup
void switchTaskNextion() {
  if (isTaskActive(processNextionTaskHandle)) {
    nextionSleep();
    vTaskDelay(pdMS_TO_TICKS(200));
    vTaskSuspend(processNextionTaskHandle);
    ESP_LOGD("SYS", "Nextion Task: остановлена");
  } else {
    if (processNextionTaskHandle == NULL) {
      xTaskCreate(processNextionTask, "Nextion", 4096, NULL, 3, &processNextionTaskHandle);
      ESP_LOGD("SYS", "Nextion Task: создана");
    } else {
      vTaskResume(processNextionTaskHandle);
      nextionWakeUP();
      ESP_LOGD("SYS", "Nextion Task: возобновлена");
    }
  }
  sendTaskStateUpdate();
}

// TVOC требует управления режимом ENS160
void switchTaskTVOCRead() {
  if (isTaskActive(taskTVOCReadHandle)) {
    checkMutex();
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      ens160.setOperatingMode(SFE_ENS160_DEEP_SLEEP);
      xSemaphoreGive(i2cMutex);
    }
    vTaskSuspend(taskTVOCReadHandle);
    ESP_LOGD("SYS", "TVOC Task: остановлена");
  } else {
    if (taskTVOCReadHandle == NULL) {
      if (!ens160.begin()) ESP_LOGE("SYS", "ENS160 не найден!");
      if (!aht20.begin())  ESP_LOGE("SYS", "AHT20 не найден!");
      xTaskCreate(taskTVOCRead, "ENS160 read task", 4096, NULL, 2, &taskTVOCReadHandle);
      ESP_LOGD("SYS", "TVOC Task: создана");
    } else {
      checkMutex();
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        ens160.setOperatingMode(SFE_ENS160_RESET);
        vTaskDelay(pdMS_TO_TICKS(100));
        ens160.setOperatingMode(SFE_ENS160_STANDARD);
        xSemaphoreGive(i2cMutex);
      }
      vTaskResume(taskTVOCReadHandle);
      ESP_LOGD("SYS", "TVOC Task: возобновлена");
    }
  }
  sendTaskStateUpdate();
}

// Обработчик POST /toggleTask — переключает задачу по имени
void handleTaskControl(AsyncWebServerRequest *request) {
  if (!request->hasParam("task", true)) {
    request->send(400, "text/plain", "Missing task parameter");
    return;
  }

  String task = request->getParam("task", true)->value();
  ESP_LOGD("SYS", "Запрос переключения задачи: %s", task.c_str());

  if      (task == "nRF905")    switchTaskNRF905();
  else if (task == "CO2")       switchTaskCO2Read();
  else if (task == "nextion")   switchTaskNextion();
  else if (task == "BMP280")    switchTaskBMP280();
  else if (task == "InfluxDB")  switchTaskInfluxDB();
  else if (task == "Forecaster") switchTaskForecaster();
  else if (task == "NTP")       switchTaskNTP();
  else if (task == "TVOC")      switchTaskTVOCRead();
  else {
    request->send(400, "text/plain", "Unknown task");
    return;
  }

  sendTaskStateUpdate();
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

// ─────────────────────────────────────────────────────────────
//  Задачи FreeRTOS
// ─────────────────────────────────────────────────────────────

// Отправка пакета данных в InfluxDB раз в 60 секунд
void taskSendDataToInfluxDB(void *pvParameters) {
  while (true) {
    sendDataToInfluxDB();
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}

// Приём пакетов от уличного блока STM32 по nRF905
// Протокол: 18 байт (burst_id + статусы + данные + CRC XOR)
void taskNRF905(void *pvParameters) {
  unsigned long lastReceived        = millis();
  const uint8_t EXPECTED_LEN        = 18;   // burst_id(1) + данные(16) + CRC(1)
  uint8_t       last_burst_id       = 0xFF; // 0xFF ≠ первый burst_id=0 → гарантированный приём

  while (true) {
    if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
      if (driver.available()) {
        uint8_t buf[RH_NRF905_MAX_MESSAGE_LEN];
        uint8_t len = sizeof(buf);

        if (driver.recv(buf, &len)) {
          // 1. Проверка длины
          if (len != EXPECTED_LEN) {
            ESP_LOGW("NRF905", "Неверная длина пакета: ожидалось %d, получено %d",
                     EXPECTED_LEN, len);
            xSemaphoreGive(driverMutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
          }

          // 2. Проверка CRC (XOR байт 0..len-2)
          if (buf[len - 1] != ({
                uint8_t cs = 0;
                for (uint8_t i = 0; i < len - 1; i++) cs ^= buf[i];
                cs;
              })) {
            // Используем inline-вычисление CRC
          }
          uint8_t crc_calc = 0;
          for (uint8_t i = 0; i < len - 1; i++) crc_calc ^= buf[i];
          if (buf[len - 1] != crc_calc) {
            ESP_LOGW("NRF905", "Ошибка CRC");
            xSemaphoreGive(driverMutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
          }

          // 3. Дедупликация Burst Transmission
          uint8_t burst_id = buf[0];
          if (burst_id == last_burst_id) {
            ESP_LOGD("NRF905", "Дубликат burst (id=%u) — пропущен", burst_id);
            xSemaphoreGive(driverMutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
          }
          last_burst_id = burst_id;

          // 4. Парсинг данных (начиная с байта 1, после burst_id)
          const uint8_t *p = buf + 1;
          heaterStatus = *p++;
          fanStatus    = *p++;

          int16_t  rawT   = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));  p += 2;
          uint16_t rawH   = (uint16_t)p[0] | ((uint16_t)p[1] << 8);              p += 2;
          uint16_t rawUV  = (uint16_t)p[0] | ((uint16_t)p[1] << 8);              p += 2;
          uint32_t rawLux = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                          | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);     p += 4;
          uint16_t rawPM25 = (uint16_t)p[0] | ((uint16_t)p[1] << 8);            p += 2;
          uint16_t rawPM10 = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

          temperature = rawT    / 100.0f;
          humidity    = rawH    / 100.0f;
          uvIndex     = rawUV   / 100.0f;
          luxLevel    = rawLux  / 100.0f;
          pm25Level   = rawPM25 / 10.0f;
          pm10Level   = rawPM10 / 10.0f;
          dewPoint    = calculateDewPoint(temperature, humidity);

          lastReceived = millis();
          ESP_LOGI("NRF905",
                   "OK [burst=%u] HEAT=%u FAN=%u T=%.2f H=%.2f "
                   "UV=%.2f LUX=%.1f PM2.5=%.1f PM10=%.1f",
                   burst_id, heaterStatus, fanStatus,
                   temperature, humidity, uvIndex,
                   luxLevel, pm25Level, pm10Level);
        }
      }
      xSemaphoreGive(driverMutex);
    }

    // Аппаратный сброс nRF905 при отсутствии данных более 10 минут
    if (millis() - lastReceived >= 600000UL) {
      ESP_LOGE("NRF905", "Нет данных >10 мин, выполняю сброс...");
      resetNRF905();
      lastReceived = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// Чтение BME280 (давление, домашняя T и H) раз в 5 секунд
void taskBMP280(void *pvParameters) {
  while (true) {
    checkMutex();
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      float rawTemp = bme.readTemperature();
      float rawHum  = bme.readHumidity();
      pressure      = bme.readPressure() / 100.0f;

      // Коррекция влажности относительно реальной температуры размещения датчика
      homeTemp = rawTemp;  // при необходимости вычесть смещение (напр. -2.0f)
      float eRaw  = es(rawTemp);
      float eCorr = es(homeTemp);
      homeHum = rawHum * (eRaw / eCorr);
      if (homeHum > 100.0f) homeHum = 100.0f;

      xSemaphoreGive(i2cMutex);
      homeDP = calculatehomeDP(homeTemp, homeHum);
    } else {
      ESP_LOGE("MUTEX", "Таймаут i2cMutex (задача: %s, держатель: %s)",
               pcTaskGetTaskName(NULL),
               xSemaphoreGetMutexHolder(i2cMutex)
                   ? pcTaskGetTaskName(xSemaphoreGetMutexHolder(i2cMutex))
                   : "none");
      resetI2CBus();
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// Синхронизация времени через NTP, расчёт восхода/заката (раз в минуту)
void taskGetTime(void *pvParameters) {
  static int currentDay = 32;
  struct tm  timeinfo;

  for (;;) {
    if (getLocalTime(&timeinfo)) {
      if (currentDay != timeinfo.tm_mday) {
        sun.setCurrentDate(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        sun.setPosition(lat, lon, tzOffset);
        sunriseTime = sun.calcSunrise();
        sunsetTime  = sun.calcSunset();
        currentDay  = timeinfo.tm_mday;
      }
      month = timeinfo.tm_mon + 1;
    } else {
      ESP_LOGE("NTP", "Не удалось получить время по NTP");
    }
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}

// Прогноз погоды по давлению (алгоритм Замбретти, раз в 30 минут)
void taskForecast(void *pvParameters) {
  for (;;) {
    int m = month;  // локальная копия, чтобы не конкурировать с taskGetTime
    if (m != -1) {
      cond.setMonth(m);
    }

    float p_hpa = pressure;
    if (!isnan(p_hpa) && p_hpa > 0.0f) {
      cond.addP((long)(p_hpa * 100.0f), temperature);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    forecast = cond.getCast();
    trend    = cond.getTrend() / 100.0f;
    ESP_LOGD("FORECAST", "Прогноз: %.1f, тренд: %.2f hPa/3ч", forecast, trend);

    vTaskDelay(pdMS_TO_TICKS(30UL * 60UL * 1000UL));
  }
}

// Отправка данных на дисплей Nextion для страницы 0 (уличные данные)
void sendPage0Data() {
  String cmd;
  cmd = "t0.txt=\"" + String(humidity)    + "%\"";    nextion.print(cmd); nextionFin();
  cmd = "t1.txt=\"" + String(temperature) + "\xc2\xb0\x43\""; nextion.print(cmd); nextionFin();
  cmd = "t3.txt=\"" + String(dewPoint)    + "\xc2\xb0\x43\""; nextion.print(cmd); nextionFin();
  cmd = "t4.txt=\"" + String(pm25Level)   + " ug/m3\""; nextion.print(cmd); nextionFin();
  cmd = "t2.txt=\"" + String(pressure)    + " hPa\"";  nextion.print(cmd); nextionFin();

  int pic;
  if      (forecast < 2.0f)  pic = 5;
  else if (forecast < 4.5f)  pic = 3;
  else if (forecast < 7.0f)  pic = 2;
  else                        pic = 4;
  cmd = "p0.pic=" + String(pic);
  nextion.print(cmd); nextionFin();
}

// Отправка данных на дисплей Nextion для страницы 1 (домашние данные)
void sendPage1Data() {
  String cmd;
  cmd = "t0.txt=\"" + String(homeHum)  + "%\"";    nextion.print(cmd); nextionFin();
  cmd = "t1.txt=\"" + String(homeTemp) + "\xc2\xb0\x43\""; nextion.print(cmd); nextionFin();
  cmd = "t3.txt=\"" + String(homeDP)   + "\xc2\xb0\x43\""; nextion.print(cmd); nextionFin();
  cmd = "t2.txt=\"" + String(pressure) + " hPa\""; nextion.print(cmd); nextionFin();
  cmd = "t4.txt=\"" + String(ppm)      + " ppm\""; nextion.print(cmd); nextionFin();
  cmd = "t5.txt=\"" + String(TVOC)     + " ppb\""; nextion.print(cmd); nextionFin();
}

// Отправка состояния кнопок-тумблеров на страницу 2 (таск-менеджер)
void sendPage2Data() {
  syncButtonState(1, taskNRF905Handle);
  syncButtonState(2, taskCO2ReadHandle);
  syncButtonState(3, processNextionTaskHandle);
  syncButtonState(4, taskBMP280Handle);
  syncButtonState(5, taskSendDataToInfluxDBHandle);
  syncButtonState(6, taskForecasterHandle);
  syncButtonState(7, taskGetTimeHandle);
}

// Разбор бинарного сообщения от Nextion (события кнопок и смены страниц)
void processNextionMessageBinary(const uint8_t *msg, size_t len) {
  if (len < 5) return;
  if (!(msg[len-1] == 0xFF && msg[len-2] == 0xFF && msg[len-3] == 0xFF)) return;

  if (msg[0] == 0x66) {
    // Событие смены страницы
    switch (msg[1]) {
      case 0x00: currentPage = "page0"; break;
      case 0x01: currentPage = "page1"; break;
      case 0x02: currentPage = "page2"; break;
      default:   currentPage = "unknown"; break;
    }
    ESP_LOGV("NEXTION", "Страница: %s", currentPage.c_str());
  } else if (msg[0] == 0x65) {
    // Событие от компонента (compID)
    switch (msg[2]) {
      case 0x03: handleRestartFromNextion(); break;
      case 0x04: resetNRF905();             break;
      case 0x05: ESP.restart();             break;
      case 0x07: switchTaskNRF905();    syncButtonState(1, taskNRF905Handle);             break;
      case 0x08: switchTaskCO2Read();   syncButtonState(2, taskCO2ReadHandle);            break;
      case 0x09: switchTaskNextion();   syncButtonState(3, processNextionTaskHandle);     break;
      case 0x0A: switchTaskBMP280();    syncButtonState(4, taskBMP280Handle);             break;
      case 0x0B: switchTaskInfluxDB();  syncButtonState(5, taskSendDataToInfluxDBHandle); break;
      case 0x0C: switchTaskForecaster();syncButtonState(6, taskForecasterHandle);         break;
      case 0x0D: switchTaskNTP();       syncButtonState(7, taskGetTimeHandle);            break;
    }
  }
}

// Задача Nextion: чтение команд с дисплея и периодическая отправка данных
void processNextionTask(void *parameter) {
  const size_t bufSize = 32;
  uint8_t      buffer[bufSize];
  size_t       bufIndex     = 0;
  unsigned long lastUpdate  = millis();

  for (;;) {
    while (nextion.available()) {
      uint8_t b = nextion.read();
      if (bufIndex < bufSize) buffer[bufIndex++] = b;

      // Пакет завершён тремя байтами 0xFF
      if (bufIndex >= 3 &&
          buffer[bufIndex-1] == 0xFF &&
          buffer[bufIndex-2] == 0xFF &&
          buffer[bufIndex-3] == 0xFF) {
        processNextionMessageBinary(buffer, bufIndex);
        bufIndex = 0;
      }
      if (bufIndex >= bufSize) bufIndex = 0;
    }

    // Обновление данных на дисплее раз в секунду
    if (millis() - lastUpdate > 1000) {
      if      (currentPage == "page0") sendPage0Data();
      else if (currentPage == "page1") sendPage1Data();
      else if (currentPage == "page2") sendPage2Data();
      lastUpdate = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// Чтение CO2 с датчика MH-Z19 по UART (раз в 5 секунд)
void taskCO2Read(void *pvParameters) {
  const byte cmd[9]   = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte       response[9];
  const int  MAX_ERRORS = 3;
  int        errorCount = 0;
  mh19.setTimeout(1000);

  while (true) {
    mh19.write(cmd, 9);
    size_t bytesRead = mh19.readBytes(response, 9);

    if (bytesRead == 9) {
      byte checksum = 0;
      for (int i = 1; i < 8; i++) checksum += response[i];
      checksum = 0xFF - checksum + 1;

      if (response[8] == checksum) {
        ppm = (256 * response[2]) + response[3];
        ESP_LOGV("CO2", "%d ppm", ppm);
        errorCount = 0;
      } else {
        errorCount++;
        ESP_LOGE("CO2", "Ошибка CRC (%d/%d)", errorCount, MAX_ERRORS);
      }
    } else {
      errorCount++;
      ESP_LOGE("CO2", "Нет ответа (%d/%d)", errorCount, MAX_ERRORS);
    }

    if (errorCount >= MAX_ERRORS) {
      ESP_LOGE("SYS", "MH-Z19 не отвечает — задача удалена");
      mh19.end();
      taskCO2ReadHandle = NULL;
      sendTaskStateUpdate();
      vTaskDelete(NULL);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// Чтение TVOC/eCO2 с ENS160, компенсация T/H с AHT20 (раз в 3 секунды)
void taskTVOCRead(void *pvParameters) {
  unsigned long lastCompensation = millis();
  float         rH = 0.0f, tempAHT = 0.0f;
  const int     MAX_ERRORS     = 3;
  int           ens160Errors   = 0;
  int           aht21Errors    = 0;

  while (true) {
    checkMutex();
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      // ENS160
      if (ens160.begin()) {
        if (ens160.checkDataStatus()) {
          AQI  = ens160.getAQI();
          TVOC = ens160.getTVOC();
          ECO2 = ens160.getECO2();
          ESP_LOGV("TVOC", "AQI=%d TVOC=%d ppb eCO2=%d ppm", AQI, TVOC, ECO2);
        }
        ens160Errors = 0;
      } else {
        ens160Errors++;
        ESP_LOGE("TVOC", "ENS160 недоступен (%d/%d)", ens160Errors, MAX_ERRORS);
      }

      // AHT20 (компенсационный датчик)
      if (aht20.begin()) {
        tempAHT   = aht20.getTemperature();
        rH        = aht20.getHumidity();
        aht21Errors = 0;
      } else {
        aht21Errors++;
        ESP_LOGE("TVOC", "AHT20 недоступен (%d/%d)", aht21Errors, MAX_ERRORS);
      }

      xSemaphoreGive(i2cMutex);
    } else {
      ESP_LOGE("MUTEX", "Таймаут i2cMutex (задача: %s)", pcTaskGetTaskName(NULL));
      resetI2CBus();
    }

    if (ens160Errors >= MAX_ERRORS || aht21Errors >= MAX_ERRORS) {
      ESP_LOGE("SYS", "ENS160/AHT20 не отвечают — задача удалена");
      taskTVOCReadHandle = NULL;
      sendTaskStateUpdate();
      vTaskDelete(NULL);
    }

    // Передача данных компенсации в ENS160 раз в 2 минуты
    if (millis() - lastCompensation >= 120000UL) {
      checkMutex();
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        ens160.setTempCompensationCelsius(tempAHT);
        ens160.setRHCompensationFloat(rH);
        ESP_LOGI("TVOC", "Компенсация: T=%.2f°C RH=%.2f%%", tempAHT, rH);
        xSemaphoreGive(i2cMutex);
        lastCompensation = millis();
      } else {
        resetI2CBus();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

// ─────────────────────────────────────────────────────────────
//  WiFi: мониторинг и автоматическое переподключение
// ─────────────────────────────────────────────────────────────

void reconnectWiFi() {
  uint32_t cooldown = (wifi_attempts > 0 && (wifi_attempts % MAX_ATTEMPTS_PER_CYCLE) == 0)
                          ? LONG_COOLDOWN
                          : SHORT_COOLDOWN;

  if (last_reconnect_time != 0 && (millis() - last_reconnect_time < cooldown)) return;

  wifi_attempts++;
  last_reconnect_time = millis();
  ESP_LOGW("WIFI", "Попытка переподключения %d/%d",
           wifi_attempts, MAX_ATTEMPTS_PER_CYCLE * MAX_CYCLES);

  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.config(IPAddress(192,168,1,230),
              IPAddress(192,168,1,254),
              IPAddress(255,255,255,0),
              IPAddress(192,168,1,254));
  esp_wifi_set_ps(WIFI_PS_NONE);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_attempts = 0;
    ESP_LOGI("WIFI", "Переподключение успешно. RSSI: %d", WiFi.RSSI());
  }
}

void wifi_timer_callback(TimerHandle_t xTimer) {
  if (WiFi.status() == WL_CONNECTED) return;

  if (wifi_attempts < MAX_ATTEMPTS_PER_CYCLE * MAX_CYCLES) {
    reconnectWiFi();
    uint32_t nextInterval = (wifi_attempts > 0 && (wifi_attempts % MAX_ATTEMPTS_PER_CYCLE) == 0)
                                ? LONG_COOLDOWN : SHORT_COOLDOWN;
    xTimerChangePeriod(wifiTimer, pdMS_TO_TICKS(nextInterval), 0);
    xTimerStart(wifiTimer, 0);
  } else {
    ESP_LOGE("WIFI", "Исчерпаны %d попыток подключения. Перезагрузка...", wifi_attempts);
    ESP.restart();
  }
}

void wifi_monitor_task(void *pvParams) {
  wifiTimer = xTimerCreate("WiFiTimer", pdMS_TO_TICKS(SHORT_COOLDOWN),
                            pdFALSE, 0, wifi_timer_callback);
  if (wifiTimer == NULL) {
    ESP_LOGE("WIFI", "Не удалось создать таймер WiFi!");
    vTaskDelete(NULL);
    return;
  }

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      ESP_LOGW("WIFI", "Отключение WiFi!");
      last_reconnect_time = 0;
      UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
      if (!xTimerIsTimerActive(wifiTimer)) {
        xTimerStartFromISR(wifiTimer, &xHigherPriorityTaskWoken);
      }
      taskEXIT_CRITICAL_FROM_ISR(saved);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      xTimerStopFromISR(wifiTimer, &xHigherPriorityTaskWoken);
      wifi_attempts = 0;
      ESP_LOGI("WIFI", "IP получен: %s", WiFi.localIP().toString().c_str());
    }

    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
  });

  while (true) {
    if (WiFi.status() != WL_CONNECTED && !xTimerIsTimerActive(wifiTimer)) {
      xTimerStart(wifiTimer, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

// ─────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────

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
  WiFi.config(IPAddress(192,168,1,230),
              IPAddress(192,168,1,254),
              IPAddress(255,255,255,0),
              IPAddress(192,168,1,254));
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(1000);
  ESP_LOGI("WIFI", "IP: %s", WiFi.localIP().toString().c_str());

  // HTTP-маршруты
  server.serveStatic("/", LittleFS, "/");
  server.on("/",          HTTP_ANY, [](AsyncWebServerRequest *r){ handleRoot(r); });
  server.on("/admin",     HTTP_POST, [](AsyncWebServerRequest *r){ handleAdmin(r); });
  server.on("/about",     HTTP_POST, [](AsyncWebServerRequest *r){ handleAbout(r); });
  server.on("/updateform",HTTP_POST, [](AsyncWebServerRequest *r){ handleUpdateForm(r); });
  server.on("/update",    HTTP_POST, handleUpdateEnd, handleUpdateUpload);
  server.onFileUpload(handleUpdateUpload);
  server.on("/restart",       HTTP_POST, handleRestart);
  server.on("/graph-data",    HTTP_GET,  [](AsyncWebServerRequest *r){ handleGraphData(r); });
  server.on("/getTasksState", HTTP_GET,  [](AsyncWebServerRequest *r){ handleGetTasksState(r); });
  server.on("/toggleTask",    HTTP_ANY,  handleTaskControl);
  server.on("/sysinfo",       HTTP_GET,  handleSysInfo);
  server.on("/bmeinfo",       HTTP_GET,  handleBMEInfo);
  server.on("/nrf905Status",  HTTP_GET,  handlenRFInfo);
  server.on("/setNRF905",     HTTP_ANY,  handleSetNRF905);
  server.on("/nrfreset",      HTTP_POST, handleNRFReset);
  server.addHandler(&webSocket);
  server.addHandler(&webSocket1);
  webSocket.onEvent(onWsEvent);
  webSocket1.onEvent(onWsEvent1);
  server.begin();

  nextionRestart();

  // nRF905
  if (!driver.init()) {
    ESP_LOGE("INIT", "nRF905 не инициализирован!");
  } else {
    driver.setChannel(175, false);    // 439.9 МГц
    driver.setRF(RH_NRF905::TransmitPowerm10dBm);
    ESP_LOGI("INIT", "nRF905 готов");
  }

  // I2C датчики
  Wire.begin();
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
  cond.setH(61);  // Высота над уровнем моря, метры

  // Мьютексы
  i2cMutex = xSemaphoreCreateMutex();
  if (!i2cMutex)    ESP_LOGE("INIT", "Ошибка создания i2cMutex!");
  driverMutex = xSemaphoreCreateMutex();
  if (!driverMutex) ESP_LOGE("INIT", "Ошибка создания driverMutex!");

  // Задачи FreeRTOS
  xTaskCreate(taskNRF905,             "NRF905 Receiver",  4096, NULL, 5, &taskNRF905Handle);
  xTaskCreate(taskBMP280,             "BMP280 Sensor",    2048, NULL, 4, &taskBMP280Handle);
  xTaskCreate(taskCO2Read,            "CO2 read task",    2048, NULL, 3, &taskCO2ReadHandle);
  xTaskCreate(taskTVOCRead,           "ENS160 read task", 4096, NULL, 2, &taskTVOCReadHandle);
  xTaskCreate(taskGetTime,            "Get NTP Time",     4096, NULL, 2, &taskGetTimeHandle);
  xTaskCreate(taskSendDataToInfluxDB, "InfluxDBTask",     4096, NULL, 6, &taskSendDataToInfluxDBHandle);
  xTaskCreate(taskForecast,           "Forecast task",    2048, NULL, 1, &taskForecasterHandle);
  xTaskCreate(processNextionTask,     "Nextion",          4096, NULL, 3, &processNextionTaskHandle);
  xTaskCreatePinnedToCore(wifi_monitor_task, "WiFiMonitor", 2048, NULL, 2, NULL, 0);
}

// ─────────────────────────────────────────────────────────────
//  Loop — не используется: всё работает через задачи FreeRTOS
// ─────────────────────────────────────────────────────────────

void loop() {
  vTaskDelete(NULL);
}