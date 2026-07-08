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

char ssid[33]          = SECRET_WIFI_SSID;
char password[65]      = SECRET_WIFI_PASSWORD;
char http_username[33] = SECRET_HTTP_USER;
char http_password[65] = SECRET_HTTP_PASSWORD;

bool   useStaticIP       = SECRET_USE_STATIC_IP;
char   staticIP[16]      = SECRET_STATIC_IP;
char   staticGateway[16] = SECRET_STATIC_GATEWAY;
char   staticSubnet[16]  = SECRET_STATIC_SUBNET;
char   staticDNS[16]     = SECRET_STATIC_DNS;

// ─────────────────────────────────────────────────────────────
//  NTP
// ─────────────────────────────────────────────────────────────

char   ntpServer[64]       = SECRET_NTP_SERVER;
long   gmtOffset_sec       = SECRET_TZ_OFFSET_SEC;
const int daylightOffset_sec = 0;

// ─────────────────────────────────────────────────────────────
//  InfluxDB
// ─────────────────────────────────────────────────────────────

char influxDBHost[64]     = SECRET_INFLUX_HOST;
int  influxDBPort         = SECRET_INFLUX_PORT;
char influxDBDatabase[33] = SECRET_INFLUX_DATABASE;

// ─────────────────────────────────────────────────────────────
//  Пины периферии (определяются в board_config.h)
// ─────────────────────────────────────────────────────────────
#include "board_config.h"

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

double latitude      = SECRET_LATITUDE;
double longitude     = SECRET_LONGITUDE;
int    tzOffset      = SECRET_TZ_OFFSET;

// ─────────────────────────────────────────────────────────────
//  UART2 → Nextion | UART1 → MH-Z19 (CO2)
// ─────────────────────────────────────────────────────────────

HardwareSerial nextion(2);
HardwareSerial mh19(1);

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
//  Очередь команд для nRF905
// ─────────────────────────────────────────────────────────────


#define NRF905_CMD_LEN       16  // Максимальная длина текстовой команды + '\0'
#define NRF905_CMD_QUEUE_LEN  5  // Ёмкость очереди (буфер команд)
 
QueueHandle_t nrf905CmdQueue    = NULL;
TaskHandle_t  taskNRF905TxHandle = NULL;

// ─────────────────────────────────────────────────────────────
//  Примитивы синхронизации
// ─────────────────────────────────────────────────────────────

SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t driverMutex;
portMUX_TYPE      mutexMux  = portMUX_INITIALIZER_UNLOCKED;
TimerHandle_t     wifiTimer;
nvs_handle_t      nrf905NvsHandle = 0;

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

// Флаг прерывания DR nRF905 (только ESP32-S3)
#ifdef ESP32S3
volatile bool nrf905DataReady = false;
void IRAM_ATTR nrf905DRISR() {
  nrf905DataReady = true;
}
#endif

// Время восхода и заката (минуты с полуночи, вычисляются раз в сутки)
double sunriseTime = 0.0;
double sunsetTime  = 0.0;

// ─────────────────────────────────────────────────────────────
//  Nextion: текущая активная страница
// ─────────────────────────────────────────────────────────────

enum NextionPage : uint8_t { PAGE0, PAGE1, PAGE2, PAGE3, PAGE_UNKNOWN };
NextionPage currentPage = PAGE0;

// ─────────────────────────────────────────────────────────────
//  Nextion: конфигурация WiFi через дисплей
// ─────────────────────────────────────────────────────────────

enum WifiCfgState { WIFI_CFG_IDLE, WIFI_CFG_WAIT_SSID, WIFI_CFG_WAIT_PASS };
WifiCfgState wifiCfgState = WIFI_CFG_IDLE;
char wifiCfgSSID[33] = {};
char wifiCfgPass[65] = {};

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
//  Диагностика памяти
// ─────────────────────────────────────────────────────────────

// Безопасное удаление — обнуляет указатель после освобождения
template <typename T>
inline void safeDelete(T *&ptr) {
  if (ptr) { delete ptr; ptr = nullptr; }
}

template <typename T>
inline void safeDeleteArray(T *&ptr) {
  if (ptr) { delete[] ptr; ptr = nullptr; }
}

// Точечный лог текущего состояния кучи
#define HEAP_CHECK(tag) \
  ESP_LOGI(tag, "Heap: free=%u maxAlloc=%u", \
           ESP.getFreeHeap(), ESP.getMaxAllocHeap())

// ─────────────────────────────────────────────────────────────
//  Прототипы функций
// ─────────────────────────────────────────────────────────────

bool  isTaskActive(TaskHandle_t taskHandle);
void  checkMutex();
void  resetI2CBus();
void  resetNRF905();
void  nextionRestart();
void  reconnectWiFi();
void  applyWiFiConfig();
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
void  taskNRF905Tx(void *pvParameters);
void  heap_monitor_task(void *pvParameters);

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
//  Астрономические вычисления: высота солнца и истинный полдень
// ─────────────────────────────────────────────────────────────

static double astroDegToRad(double deg) {
  return M_PI * deg / 180.0;
}

static double astroRadToDeg(double rad) {
  return 180.0 * rad / M_PI;
}

// Юлианская дата из календарной
static double astroCalcJD(int y, int m, int d) {
  if (m <= 2) { y--; m += 12; }
  double A = floor(y / 100.0);
  double B = 2.0 - A + floor(A / 4.0);
  return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + d + B - 1524.5;
}

// Уравнение времени (минуты)
static double astroCalcEquationOfTime(double jd) {
  double T = (jd - 2451545.0) / 36525.0;

  double L0 = 280.46646 + T * (36000.76983 + 0.0003032 * T);
  L0 = fmod(L0, 360.0);
  if (L0 < 0) L0 += 360.0;

  double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  M = fmod(M, 360.0);
  if (M < 0) M += 360.0;

  double epsilon = 23.0 + (26.0 + (21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  double omega = 125.04 - 1934.136 * T;
  double e = epsilon + 0.00256 * cos(astroDegToRad(omega));

  double y = tan(astroDegToRad(e) / 2.0);
  y *= y;

  double sin2L0 = sin(2.0 * astroDegToRad(L0));
  double sinM   = sin(astroDegToRad(M));
  double cos2L0 = cos(2.0 * astroDegToRad(L0));
  double sin4L0 = sin(4.0 * astroDegToRad(L0));
  double sin2M  = sin(2.0 * astroDegToRad(M));

  double Etime = y * sin2L0 - 2.0 * 0.016708634 * sinM
               + 4.0 * 0.016708634 * y * sinM * cos2L0
               - 0.5 * y * y * sin4L0
               - 1.25 * 0.016708634 * 0.016708634 * sin2M;

  return astroRadToDeg(Etime) * 4.0;  // минуты
}

// Склонение Солнца (градусы)
static double astroCalcSunDeclination(double jd) {
  double T = (jd - 2451545.0) / 36525.0;

  double epsilon0 = 23.0 + (26.0 + (21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
  double omega = 125.04 - 1934.136 * T;
  double epsilon = epsilon0 + 0.00256 * cos(astroDegToRad(omega));

  double L0 = 280.46646 + T * (36000.76983 + 0.0003032 * T);
  L0 = fmod(L0, 360.0);
  if (L0 < 0) L0 += 360.0;

  double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
  M = fmod(M, 360.0);
  if (M < 0) M += 360.0;

  double C = sin(astroDegToRad(M)) * (1.914602 - T * (0.004817 + 0.000014 * T))
           + sin(astroDegToRad(2.0 * M)) * (0.019993 - 0.000101 * T)
           + sin(astroDegToRad(3.0 * M)) * 0.000289;

  double sunLong = L0 + C;
  double lambda = sunLong - 0.00569 - 0.00478 * sin(astroDegToRad(omega));

  double sint = sin(astroDegToRad(epsilon)) * sin(astroDegToRad(lambda));
  return astroRadToDeg(asin(sint));
}

// Высота солнца над горизонтом (градусы)
double calcSunElevation(double latitude, double longitude, time_t timestamp) {
  struct tm *timeinfo = localtime(&timestamp);

  int y = timeinfo->tm_year + 1900;
  int m = timeinfo->tm_mon + 1;
  int d = timeinfo->tm_mday;
  double jd = astroCalcJD(y, m, d);

  double eqTime = astroCalcEquationOfTime(jd);
  double decl   = astroCalcSunDeclination(jd);

  // Местное время в часах
  double hour = timeinfo->tm_hour + timeinfo->tm_min / 60.0 + timeinfo->tm_sec / 3600.0;

  // Солнечное время (часы): местное время + поправка на долготу + уравнение времени
  // (longitude - tzOffset*15) — смещение от референсного меридиана часового пояса
  double solarTime = hour + (longitude - tzOffset * 15.0) * 4.0 / 60.0 + eqTime / 60.0;

  // Часовой угол (градусы)
  double hourAngle = (solarTime - 12.0) * 15.0;

  // Высота солнца
  double latRad  = astroDegToRad(latitude);
  double decRad  = astroDegToRad(decl);
  double haRad   = astroDegToRad(hourAngle);

  double sinElev = sin(latRad) * sin(decRad) + cos(latRad) * cos(decRad) * cos(haRad);
  return astroRadToDeg(asin(sinElev));
}

// Время истинного полдня (секунды с полуночи местного времени)
double calcSolarNoon(double longitude, time_t timestamp) {
  struct tm *timeinfo = localtime(&timestamp);

  int y = timeinfo->tm_year + 1900;
  int m = timeinfo->tm_mon + 1;
  int d = timeinfo->tm_mday;
  double jd = astroCalcJD(y, m, d);

  double eqTime = astroCalcEquationOfTime(jd);

  // Истинный полдень в UTC (минуты с полуночи UTC)
  double solarNoonUTC = 720.0 - eqTime - longitude * 4.0;

  // Перевод в местное время (секунды с полуночи)
  double solarNoonLocal = (solarNoonUTC + tzOffset * 60.0) * 60.0;

  // Нормализация в пределах суток
  while (solarNoonLocal < 0) solarNoonLocal += 86400.0;
  while (solarNoonLocal >= 86400.0) solarNoonLocal -= 86400.0;

  return solarNoonLocal;
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

  IPAddress ip = WiFi.localIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  snprintf(buffer, len,
           "Uptime: %s\nChip model: %s\nChip rev.: %d\n"
           "WiFi RSSI: %d dBm\nIP address: %s\n"
           "Free Heap: %u bytes\nMax Alloc: %u bytes\n"
           "%s"
           "WebServer free stack: %d bytes\n",
           uptimeStr,
           ESP.getChipModel(), ESP.getChipRevision(),
           WiFi.RSSI(),
           ipStr,
           ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
           (ESP.getFreeHeap() < 32768) ? "*** LOW MEMORY ***\n" : "",
           uxTaskGetStackHighWaterMark(NULL));
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

// ─────────────────────────────────────────────────────────────
//  NVS: сохранение/загрузка настроек nRF905
// ─────────────────────────────────────────────────────────────

void nrf905SaveSettings(int channel, bool band, const char *power) {
  if (nrf905NvsHandle == 0) return;
  nvs_set_i32(nrf905NvsHandle, "channel", channel);
  nvs_set_u8(nrf905NvsHandle, "band", band ? 1 : 0);
  nvs_set_str(nrf905NvsHandle, "power", power);
  nvs_commit(nrf905NvsHandle);
  ESP_LOGI("NRF905", "Настройки сохранены в NVS: ch=%d band=%d pwr=%s", channel, band, power);
}

bool nrf905LoadSettings(int &channel, bool &band, char *power, size_t powerLen) {
  if (nrf905NvsHandle == 0) return false;
  int32_t ch; uint8_t b;
  if (nvs_get_i32(nrf905NvsHandle, "channel", &ch) != ESP_OK) return false;
  if (nvs_get_u8(nrf905NvsHandle, "band", &b) != ESP_OK) return false;
  if (nvs_get_str(nrf905NvsHandle, "power", power, &powerLen) != ESP_OK) return false;
  channel = (int)ch;
  band = (b != 0);
  ESP_LOGI("NRF905", "Настройки загружены из NVS: ch=%d band=%d pwr=%s", channel, band, power);
  return true;
}

// ─────────────────────────────────────────────────────────────
//  NVS: сохранение/загрузка системных настроек
// ─────────────────────────────────────────────────────────────

nvs_handle_t settingsNvsHandle = 0;

void settingsSaveAll() {
  if (settingsNvsHandle == 0) return;
  nvs_set_str(settingsNvsHandle, "wifi_ssid",   ssid);
  nvs_set_str(settingsNvsHandle, "wifi_pass",   password);
  nvs_set_str(settingsNvsHandle, "http_user",   http_username);
  nvs_set_str(settingsNvsHandle, "http_pass",   http_password);
  nvs_set_u8(settingsNvsHandle, "use_static_ip",  useStaticIP ? 1 : 0);
  nvs_set_str(settingsNvsHandle, "static_ip",      staticIP);
  nvs_set_str(settingsNvsHandle, "static_gateway", staticGateway);
  nvs_set_str(settingsNvsHandle, "static_subnet",  staticSubnet);
  nvs_set_str(settingsNvsHandle, "static_dns",     staticDNS);
  nvs_set_str(settingsNvsHandle, "influx_host", influxDBHost);
  nvs_set_i32(settingsNvsHandle, "influx_port", influxDBPort);
  nvs_set_str(settingsNvsHandle, "influx_db",   influxDBDatabase);
  nvs_set_str(settingsNvsHandle, "ntp_server",  ntpServer);
  nvs_set_i32(settingsNvsHandle, "tz_sec",      gmtOffset_sec);
  nvs_set_str(settingsNvsHandle, "latitude",    String(latitude, 6).c_str());
  nvs_set_str(settingsNvsHandle, "longitude",   String(longitude, 6).c_str());
  nvs_set_i32(settingsNvsHandle, "tz_offset",   tzOffset);
  nvs_commit(settingsNvsHandle);
  ESP_LOGI("SETTINGS", "Настройки сохранены в NVS");
}

void settingsLoadAll() {
  if (settingsNvsHandle == 0) return;
  char buf[64];
  size_t len;
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "wifi_ssid", buf, &len) == ESP_OK) strncpy(ssid, buf, sizeof(ssid) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "wifi_pass", buf, &len) == ESP_OK) strncpy(password, buf, sizeof(password) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "http_user", buf, &len) == ESP_OK) strncpy(http_username, buf, sizeof(http_username) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "http_pass", buf, &len) == ESP_OK) strncpy(http_password, buf, sizeof(http_password) - 1);
  uint8_t ipMode;
  if (nvs_get_u8(settingsNvsHandle, "use_static_ip", &ipMode) == ESP_OK) useStaticIP = (ipMode != 0);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_ip", buf, &len) == ESP_OK) strncpy(staticIP, buf, sizeof(staticIP) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_gateway", buf, &len) == ESP_OK) strncpy(staticGateway, buf, sizeof(staticGateway) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_subnet", buf, &len) == ESP_OK) strncpy(staticSubnet, buf, sizeof(staticSubnet) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_dns", buf, &len) == ESP_OK) strncpy(staticDNS, buf, sizeof(staticDNS) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "influx_host", buf, &len) == ESP_OK) strncpy(influxDBHost, buf, sizeof(influxDBHost) - 1);
  int32_t port;
  if (nvs_get_i32(settingsNvsHandle, "influx_port", &port) == ESP_OK) influxDBPort = (int)port;
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "influx_db", buf, &len) == ESP_OK) strncpy(influxDBDatabase, buf, sizeof(influxDBDatabase) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "ntp_server", buf, &len) == ESP_OK) strncpy(ntpServer, buf, sizeof(ntpServer) - 1);
  int32_t tz;
  if (nvs_get_i32(settingsNvsHandle, "tz_sec", &tz) == ESP_OK) gmtOffset_sec = tz;
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "latitude", buf, &len) == ESP_OK) latitude = atof(buf);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "longitude", buf, &len) == ESP_OK) longitude = atof(buf);
  if (nvs_get_i32(settingsNvsHandle, "tz_offset", &tz) == ESP_OK) tzOffset = (int)tz;
  ESP_LOGI("SETTINGS", "Настройки загружены из NVS");
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
    nrf905SaveSettings(channel, band, powerStr.c_str());
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

void resetNVS() {
  ESP_LOGW("NVS", "Полная очистка NVS...");

  if (nrf905NvsHandle != 0) { nvs_close(nrf905NvsHandle); nrf905NvsHandle = 0; }
  if (settingsNvsHandle != 0) { nvs_close(settingsNvsHandle); settingsNvsHandle = 0; }

  nvs_flash_erase();
  nvs_flash_init();

  delay(500);
  ESP.restart();
}

void handleResetNVS(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(200, "text/plain", "NVS cleared.");
  resetNVS();
}

// ─────────────────────────────────────────────────────────────
//  Системные настройки: GET/POST
// ─────────────────────────────────────────────────────────────

void handleGetSettings(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;

  char maskedPass[17];
  snprintf(maskedPass, sizeof(maskedPass), "****%s", password + strlen(password) - (strlen(password) > 4 ? 4 : strlen(password)));

  char json[1536];
  snprintf(json, sizeof(json),
    "{\"wifi_ssid\":\"%s\",\"wifi_pass\":\"****\","
    "\"http_user\":\"%s\",\"http_pass\":\"****\","
    "\"use_static_ip\":%d,\"static_ip\":\"%s\",\"static_gateway\":\"%s\",\"static_subnet\":\"%s\",\"static_dns\":\"%s\","
    "\"influx_host\":\"%s\",\"influx_port\":%d,\"influx_db\":\"%s\","
    "\"ntp_server\":\"%s\","
    "\"latitude\":%.6f,\"longitude\":%.6f,"
    "\"tz_offset\":%d,\"tz_sec\":%ld}",
    ssid, http_username,
    useStaticIP ? 1 : 0, staticIP, staticGateway, staticSubnet, staticDNS,
    influxDBHost, influxDBPort, influxDBDatabase,
    ntpServer, latitude, longitude,
    tzOffset, gmtOffset_sec);

  request->send(200, "application/json", json);
}

void handleSetSettings(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;

  if (request->hasParam("wifi_ssid", true))   strncpy(ssid,          request->getParam("wifi_ssid",  true)->value().c_str(), sizeof(ssid) - 1);
  if (request->hasParam("wifi_pass", true)) {
    const char *p = request->getParam("wifi_pass", true)->value().c_str();
    if (strlen(p) > 0 && strcmp(p, "****") != 0) strncpy(password, p, sizeof(password) - 1);
  }
  if (request->hasParam("http_user", true))   strncpy(http_username, request->getParam("http_user",  true)->value().c_str(), sizeof(http_username) - 1);
  if (request->hasParam("http_pass", true)) {
    const char *p = request->getParam("http_pass", true)->value().c_str();
    if (strlen(p) > 0 && strcmp(p, "****") != 0) strncpy(http_password, p, sizeof(http_password) - 1);
  }
  if (request->hasParam("use_static_ip", true))
    useStaticIP = (request->getParam("use_static_ip", true)->value() == "1");
  if (request->hasParam("static_ip", true))
    strncpy(staticIP, request->getParam("static_ip", true)->value().c_str(), sizeof(staticIP) - 1);
  if (request->hasParam("static_gateway", true))
    strncpy(staticGateway, request->getParam("static_gateway", true)->value().c_str(), sizeof(staticGateway) - 1);
  if (request->hasParam("static_subnet", true))
    strncpy(staticSubnet, request->getParam("static_subnet", true)->value().c_str(), sizeof(staticSubnet) - 1);
  if (request->hasParam("static_dns", true))
    strncpy(staticDNS, request->getParam("static_dns", true)->value().c_str(), sizeof(staticDNS) - 1);
  if (request->hasParam("influx_host", true)) strncpy(influxDBHost,     request->getParam("influx_host", true)->value().c_str(), sizeof(influxDBHost) - 1);
  if (request->hasParam("influx_port", true)) influxDBPort = request->getParam("influx_port", true)->value().toInt();
  if (request->hasParam("influx_db", true))   strncpy(influxDBDatabase, request->getParam("influx_db",   true)->value().c_str(), sizeof(influxDBDatabase) - 1);
  if (request->hasParam("ntp_server", true))  strncpy(ntpServer, request->getParam("ntp_server", true)->value().c_str(), sizeof(ntpServer) - 1);
  if (request->hasParam("latitude", true))    latitude  = request->getParam("latitude",  true)->value().toDouble();
  if (request->hasParam("longitude", true))   longitude = request->getParam("longitude", true)->value().toDouble();
  if (request->hasParam("tz_offset", true))   tzOffset  = request->getParam("tz_offset", true)->value().toInt();
  if (request->hasParam("tz_sec", true))      gmtOffset_sec = request->getParam("tz_sec", true)->value().toInt();

  settingsSaveAll();
  ESP_LOGI("SETTINGS", "Настройки обновлены из веб-интерфейса");
  request->send(200, "text/plain", "Settings saved. Reboot to apply.");
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

// Отправляет текущее время, данные восхода/заката, высоту солнца и полдень по /ws1
void sendTimeData() {
  if (webSocket1.count() == 0) return;

  time_t now = time(nullptr);

  StaticJsonDocument<256> json;
  json["nowTime"]      = (double)now;
  json["sunriseTime"]  = sunriseTime * 60;
  json["sunsetTime"]   = sunsetTime  * 60;
  json["sunElevation"] = calcSunElevation(latitude, longitude, now);
  json["solarNoon"]    = calcSolarNoon(longitude, now);

  static char jsonBuffer[320];
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
    IPAddress rip = client->remoteIP();
    ESP_LOGI("WS", "Клиент #%u подключён (%d.%d.%d.%d)", client->id(),
             rip[0], rip[1], rip[2], rip[3]);
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
  pinMode(I2C_SCL, OUTPUT);
  pinMode(I2C_SDA, INPUT_PULLUP);
  for (int i = 0; i < 10; i++) {
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
    digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5);
  }
  digitalWrite(I2C_SCL, HIGH);
  vTaskDelay(pdMS_TO_TICKS(10));

  Wire.end();
  Wire.begin(I2C_SDA, I2C_SCL);
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
      driver.setRF(RH_NRF905::TransmitPower10dBm);
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
      String resp = http.getString();
      ESP_LOGD("InfluxDB", "Ответ: %s", resp.c_str());
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

// Ставит команду STM32 в очередь отправки через nRF905
void queueStmCommand(const char* cmd) {
    char buf[NRF905_CMD_LEN] = {};
    strncpy(buf, cmd, NRF905_CMD_LEN - 1);
    xQueueSend(nrf905CmdQueue, buf, pdMS_TO_TICKS(500));
}

// Ставит текстовую команду в очередь отправки на внешний блок STM32.
// POST /sendCommand   тело: cmd=HEATER|NRF_REST|REST
void handleSendCommand(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) return;
 
    if (!request->hasParam("cmd", true)) {
        request->send(400, "text/plain", "Параметр cmd не указан");
        return;
    }
 
    String cmdStr = request->getParam("cmd", true)->value();
    if (cmdStr.length() == 0 || cmdStr.length() >= NRF905_CMD_LEN) {
        request->send(400, "text/plain", "Недопустимая длина команды");
        return;
    }
 
    // Белый список — только известные команды STM32
    if (cmdStr != "HEATER" && cmdStr != "NRF_REST" && cmdStr != "REST") {
        String resp = "Неизвестная команда: ";
        resp.concat(cmdStr);
        request->send(400, "text/plain", resp);
        return;
    }
 
    char cmd[NRF905_CMD_LEN] = {};
    strncpy(cmd, cmdStr.c_str(), NRF905_CMD_LEN - 1);
 
    if (xQueueSend(nrf905CmdQueue, cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
        ESP_LOGI("NRF905_TX", "Команда поставлена в очередь: %s", cmd);
        request->send(200, "text/plain", cmdStr);
    } else {
        ESP_LOGW("NRF905_TX", "Очередь переполнена, команда отброшена: %s", cmd);
        request->send(503, "text/plain", "Очередь переполнена, попробуйте позже");
    }
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
  uint8_t       recoveryState       = 0;    // 0=норма, 1-2=NRF_REST, 3=REST

  while (true) {
#ifdef ESP32S3
    // ESP32-S3: ждём прерывание DR (пакет получен) с таймаутом 5 сек
    nrf905DataReady = false;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
#endif

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
          if (recoveryState > 0) {
            ESP_LOGI("NRF905", "Связь восстановлена (состояние %d → 0)", recoveryState);
            recoveryState = 0;
          }
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

    // Восстановление связи: 3 стадии по 5 минут
    if (millis() - lastReceived >= 300000UL) {
      recoveryState++;

      if (recoveryState <= 2) {
        ESP_LOGE("NRF905", "Нет данных >%d мин, NRF_REST (попытка %d)...",
                 recoveryState * 5, recoveryState);
        resetNRF905();
        char cmd[] = "NRF_REST";
        xQueueSend(nrf905CmdQueue, cmd, pdMS_TO_TICKS(500));
      } else {
        ESP_LOGE("NRF905", "NRF_REST не помог, отправляю REST...");
        char cmd[] = "REST";
        xQueueSend(nrf905CmdQueue, cmd, pdMS_TO_TICKS(500));
        recoveryState = 0;
      }

      lastReceived = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// Отправка команд на внешний блок через nRF905.
//
// Архитектура очереди:
//   handleSendCommand() → xQueueSend → [nrf905CmdQueue] → taskNRF905Tx → nRF905 TX
//
// Синхронизация с приёмом:
//   driverMutex — общий с taskNRF905 (RX). Пока идёт TX, RX-задача
//   ждёт мьютекс и не трогает драйвер. После waitPacketSent() модуль
//   автоматически возвращается в RX-режим.
void taskNRF905Tx(void *pvParameters) {
    char cmd[NRF905_CMD_LEN];
 
    while (true) {
        // Блокирующее ожидание — задача спит, пока очередь пуста
        if (xQueueReceive(nrf905CmdQueue, cmd, portMAX_DELAY) != pdTRUE) continue;
 
        ESP_LOGI("NRF905_TX", "Отправка команды: %s", cmd);
 
        if (xSemaphoreTake(driverMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            uint8_t len = (uint8_t)strlen(cmd);
            driver.send((uint8_t *)cmd, len);
            driver.waitPacketSent();   // после этого модуль → RX
            xSemaphoreGive(driverMutex);
            ESP_LOGI("NRF905_TX", "Команда '%s' отправлена", cmd);
        } else {
            ESP_LOGE("NRF905_TX", "Таймаут driverMutex! Команда '%s' потеряна", cmd);
        }
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
        sun.setPosition(latitude, longitude, tzOffset);
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
  nextion.printf("t0.txt=\"%.1f%%\"", humidity);                            nextionFin();
  nextion.printf("t1.txt=\"%.1f\xc2\xb0\x43\"", temperature);              nextionFin();
  nextion.printf("t3.txt=\"%.1f\xc2\xb0\x43\"", dewPoint);                 nextionFin();
  nextion.printf("t4.txt=\"%.1f ug/m3\"", pm25Level);                       nextionFin();
  nextion.printf("t2.txt=\"%.1f hPa\"", pressure);                          nextionFin();

  int pic;
  if      (forecast < 2.0f)  pic = 5;
  else if (forecast < 4.5f)  pic = 3;
  else if (forecast < 7.0f)  pic = 2;
  else                        pic = 4;
  nextion.printf("p0.pic=%d", pic); nextionFin();
}

// Отправка данных на дисплей Nextion для страницы 1 (домашние данные)
void sendPage1Data() {
  nextion.printf("t0.txt=\"%.1f%%\"", homeHum);                              nextionFin();
  nextion.printf("t1.txt=\"%.1f\xc2\xb0\x43\"", homeTemp);                  nextionFin();
  nextion.printf("t3.txt=\"%.1f\xc2\xb0\x43\"", homeDP);                    nextionFin();
  nextion.printf("t2.txt=\"%.1f hPa\"", pressure);                           nextionFin();
  nextion.printf("t4.txt=\"%d ppm\"", ppm);                                  nextionFin();
  nextion.printf("t5.txt=\"%d ppb\"", TVOC);                                 nextionFin();
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
  syncButtonState(0, taskTVOCReadHandle);
}

// Отправка системной информации на страницу 3 (WiFi + diag)
void sendPage3Data() {
  char info[256];
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    unsigned long sec = millis() / 1000;
    unsigned long h = sec / 3600;
    unsigned long m = (sec % 3600) / 60;
    snprintf(info, sizeof(info),
      "WiFi: Connected\r\n"
      "SSID: %s\r\n"
      "IP: %d.%d.%d.%d\r\n"
      "RSSI: %d dBm\r\n"
      "Uptime: %luh %lum\r\n"
      "Free Heap: %u KB\r\n"
      "Max Alloc: %u KB",
      WiFi.SSID().c_str(),
      ip[0], ip[1], ip[2], ip[3],
      WiFi.RSSI(),
      h, m,
      ESP.getFreeHeap() / 1024,
      ESP.getMaxAllocHeap() / 1024);
  } else {
    snprintf(info, sizeof(info),
      "WiFi: Disconnected\r\n"
      "Free Heap: %u KB\r\n"
      "Max Alloc: %u KB",
      ESP.getFreeHeap() / 1024,
      ESP.getMaxAllocHeap() / 1024);
  }
  nextion.printf("t2.txt=\"%s\"", info);
  nextionFin();
}

// Разбор бинарного сообщения от Nextion (события кнопок и смены страниц)
void processNextionMessageBinary(const uint8_t *msg, size_t len) {
  if (len < 5) return;
  if (!(msg[len-1] == 0xFF && msg[len-2] == 0xFF && msg[len-3] == 0xFF)) return;

  if (msg[0] == 0x66) {
    // Событие смены страницы
    switch (msg[1]) {
      case 0x00: currentPage = PAGE0; break;
      case 0x01: currentPage = PAGE1; break;
      case 0x02: currentPage = PAGE2; break;
      case 0x03: currentPage = PAGE3; break;
      default:   currentPage = PAGE_UNKNOWN; break;
    }
    ESP_LOGV("NEXTION", "Страница: %d", currentPage);
  } else if (msg[0] == 0x65) {
    // Событие от компонента (page, compID, event)
    uint8_t page  = msg[1];
    uint8_t compID = msg[2];

    if (page == 0x02) {
      // Страница 0x02 — управление
      switch (compID) {
        case 0x03: handleRestartFromNextion(); break;
        case 0x04: resetNRF905();              break;
        case 0x05: resetNVS();                  break;
        case 0x06: switchTaskTVOCRead();   syncButtonState(0, taskTVOCReadHandle);           break;
        case 0x07: switchTaskNRF905();     syncButtonState(1, taskNRF905Handle);             break;
        case 0x08: switchTaskCO2Read();    syncButtonState(2, taskCO2ReadHandle);            break;
        case 0x09: switchTaskNextion();    syncButtonState(3, processNextionTaskHandle);     break;
        case 0x0A: switchTaskBMP280();     syncButtonState(4, taskBMP280Handle);             break;
        case 0x0B: switchTaskInfluxDB();   syncButtonState(5, taskSendDataToInfluxDBHandle); break;
        case 0x0C: switchTaskForecaster(); syncButtonState(6, taskForecasterHandle);         break;
        case 0x0D: switchTaskNTP();        syncButtonState(7, taskGetTimeHandle);            break;
        case 0x0F: queueStmCommand("HEATER");  ESP_LOGI("NEXTION", "HEATER → STM32");  break;
        case 0x10: queueStmCommand("NRF_REST"); ESP_LOGI("NEXTION", "NRF_REST → STM32"); break;
        case 0x11: queueStmCommand("REST");     ESP_LOGI("NEXTION", "REST → STM32");     break;
      }
    } else if (page == 0x03 && compID == 0x03) {
      // Страница 0x03 — кнопка WiFi: ожидаем SSID и Password (пакеты 0x70)
      wifiCfgState = WIFI_CFG_WAIT_SSID;
      ESP_LOGI("NEXTION", "WiFi config: ожидание SSID");
    }
  } else if (msg[0] == 0x70 && wifiCfgState != WIFI_CFG_IDLE) {
    // Строка от Nextion (SSID или Password после нажатия кнопки WiFi)
    // msg[1..len-4] — содержимое строки (без 0x70 и 0xFF 0xFF 0xFF)
    size_t strLen = len - 4; // убираем 0x70 и три 0xFF
    if (strLen >= sizeof(wifiCfgSSID)) strLen = sizeof(wifiCfgSSID) - 1;

    if (wifiCfgState == WIFI_CFG_WAIT_SSID) {
      memcpy(wifiCfgSSID, &msg[1], strLen);
      wifiCfgSSID[strLen] = '\0';
      wifiCfgState = WIFI_CFG_WAIT_PASS;
      ESP_LOGI("NEXTION", "WiFi SSID: %s", wifiCfgSSID);
    } else if (wifiCfgState == WIFI_CFG_WAIT_PASS) {
      memcpy(wifiCfgPass, &msg[1], strLen);
      wifiCfgPass[strLen] = '\0';
      wifiCfgState = WIFI_CFG_IDLE;
      ESP_LOGI("NEXTION", "WiFi: пробуем подключиться к %s", wifiCfgSSID);

      // Отключаемся от текущей сети
      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(wifiCfgSSID, wifiCfgPass);

      // Ждём до 15 сек
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
      }

      if (WiFi.status() == WL_CONNECTED) {
        strncpy(ssid, wifiCfgSSID, sizeof(ssid) - 1);
        strncpy(password, wifiCfgPass, sizeof(password) - 1);
        settingsSaveAll();
        ESP_LOGI("NEXTION", "WiFi подключён! IP: %s", WiFi.localIP().toString().c_str());
        wifi_attempts = 0;
      } else {
        ESP_LOGW("NEXTION", "WiFi: не удалось подключиться к %s", wifiCfgSSID);
      }
      memset(wifiCfgSSID, 0, sizeof(wifiCfgSSID));
      memset(wifiCfgPass, 0, sizeof(wifiCfgPass));
    }
  }
}

// Задача Nextion: чтение команд с дисплея и периодическая отправка данных
void processNextionTask(void *parameter) {
  const size_t bufSize = 32;
  uint8_t      buffer[bufSize];
  size_t       bufIndex     = 0;
  unsigned long lastUpdate  = millis();
  unsigned long lastPage3Update = millis();

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
      switch (currentPage) {
        case PAGE0: sendPage0Data(); break;
        case PAGE1: sendPage1Data(); break;
        case PAGE2: sendPage2Data(); break;
        default: break;
      }
      lastUpdate = millis();
    }

    // Страница 3 — системная информация раз в 2 секунды
    if (currentPage == PAGE3 && millis() - lastPage3Update > 2000) {
      sendPage3Data();
      lastPage3Update = millis();
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
  applyWiFiConfig();
  esp_wifi_set_ps(WIFI_PS_NONE);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_attempts = 0;
    IPAddress ip = WiFi.localIP();
    ESP_LOGI("WIFI", "Переподключение успешно. RSSI: %d IP: %d.%d.%d.%d",
             WiFi.RSSI(), ip[0], ip[1], ip[2], ip[3]);
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
      IPAddress ip = WiFi.localIP();
      ESP_LOGI("WIFI", "IP получен: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
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
//  Мониторинг кучи: лог каждые 60 сек, рестарт при критическом минимуме
// ─────────────────────────────────────────────────────────────

#define HEAP_WARN_THRESHOLD   81920   // 32 КБ — предупреждение
#define HEAP_CRIT_THRESHOLD   65536   // 16 КБ — аварийный рестарт

void heap_monitor_task(void *pvParameters) {
  while (true) {
    uint32_t freeHeap   = ESP.getFreeHeap();
    uint32_t maxAlloc   = ESP.getMaxAllocHeap();

    ESP_LOGI("HEAP", "free=%u maxAlloc=%u", freeHeap, maxAlloc);

    if (freeHeap < HEAP_CRIT_THRESHOLD) {
      ESP_LOGE("HEAP", "Критический минимум кучи (%u байт)! Перезагрузка...", freeHeap);
      vTaskDelay(pdMS_TO_TICKS(1000));
      ESP.restart();
    }

    if (freeHeap < HEAP_WARN_THRESHOLD) {
      ESP_LOGW("HEAP", "Мало свободной памяти (%u байт)", freeHeap);
    }

    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}

// ─────────────────────────────────────────────────────────────
//  WiFi config helper
// ─────────────────────────────────────────────────────────────

void applyWiFiConfig() {
    if (useStaticIP && staticIP[0] != '\0') {
        IPAddress ip, gw, sn, dns;
        ip.fromString(staticIP);
        gw.fromString(staticGateway);
        sn.fromString(staticSubnet);
        dns.fromString(staticDNS);
        WiFi.config(ip, gw, sn, dns);
        ESP_LOGI("WIFI", "Static IP: %s", staticIP);
    } else {
        ESP_LOGI("WIFI", "Using DHCP");
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
  applyWiFiConfig();
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(1000);
  IPAddress ip = WiFi.localIP();
  ESP_LOGI("WIFI", "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  // HTTP-маршруты
  server.serveStatic("/", LittleFS, "/");
  server.on("/",          HTTP_ANY, [](AsyncWebServerRequest *r){ handleRoot(r); });
  server.on("/admin",     HTTP_POST, [](AsyncWebServerRequest *r){ handleAdmin(r); });
  server.on("/about",     HTTP_POST, [](AsyncWebServerRequest *r){ handleAbout(r); });
  server.on("/updateform",HTTP_POST, [](AsyncWebServerRequest *r){ handleUpdateForm(r); });
  server.on("/update",    HTTP_POST, handleUpdateEnd, handleUpdateUpload);
  server.onFileUpload(handleUpdateUpload);
  server.on("/restart",       HTTP_POST, handleRestart);
  server.on("/sendCommand", HTTP_POST, handleSendCommand);
  server.on("/graph-data",    HTTP_GET,  [](AsyncWebServerRequest *r){ handleGraphData(r); });
  server.on("/getTasksState", HTTP_GET,  [](AsyncWebServerRequest *r){ handleGetTasksState(r); });
  server.on("/toggleTask",    HTTP_ANY,  handleTaskControl);
  server.on("/sysinfo",       HTTP_GET,  handleSysInfo);
  server.on("/bmeinfo",       HTTP_GET,  handleBMEInfo);
  server.on("/nrf905Status",  HTTP_GET,  handlenRFInfo);
  server.on("/setNRF905",     HTTP_ANY,  handleSetNRF905);
  server.on("/nrfreset",      HTTP_POST, handleNRFReset);
  server.on("/resetNVS",      HTTP_POST, handleResetNVS);
  server.on("/getSettings",   HTTP_GET,  handleGetSettings);
  server.on("/setSettings",   HTTP_POST, handleSetSettings);
  server.addHandler(&webSocket);
  server.addHandler(&webSocket1);
  webSocket.onEvent(onWsEvent);
  webSocket1.onEvent(onWsEvent1);
  server.begin();

  nextionRestart();

  // nRF905: NVS для хранения настроек
  if (nvs_open("nrf905", NVS_READWRITE, &nrf905NvsHandle) != ESP_OK) {
    ESP_LOGE("INIT", "Не удалось открыть NVS для настроек nRF905");
  }

  // Системные настройки: загрузка из NVS
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
      driver.setChannel(175, false);    // 439.9 МГц
      driver.setRF(RH_NRF905::TransmitPower10dBm);
    }
    ESP_LOGI("INIT", "nRF905 готов");
  }

#ifdef ESP32S3
  // nRF905: прерывания DR/AM (только ESP32-S3)
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
  cond.setH(61);  // Высота над уровнем моря, метры

  // Мьютексы
  i2cMutex = xSemaphoreCreateMutex();
  if (!i2cMutex)    ESP_LOGE("INIT", "Ошибка создания i2cMutex!");
  driverMutex = xSemaphoreCreateMutex();
  if (!driverMutex) ESP_LOGE("INIT", "Ошибка создания driverMutex!");
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

// ─────────────────────────────────────────────────────────────
//  Loop — не используется: всё работает через задачи FreeRTOS
// ─────────────────────────────────────────────────────────────

void loop() {
  vTaskDelete(NULL);
}