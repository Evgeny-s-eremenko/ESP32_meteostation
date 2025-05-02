#include <Arduino.h>
#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
//#include <Adafruit_HMC5883_U.h>
//#include "HMC5883L.h"
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





// ----------------------------- Wi-Fi и сервер -----------------------------
const char *ssid = "REMOVED";
const char *password = "REMOVED";
const char* http_username = "evgen";  // Логин для доступа
const char* http_password = "REMOVED";   // Пароль для доступа

// Настройка NTP-сервера
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600 * 10; // Смещение для Komsomolsk-on-Amur (GMT+10)
const int   daylightOffset_sec = 0;

// Параметры InfluxDB
const char* influxDBHost = "REMOVED";
const int influxDBPort = 8086;
const char* influxDBDatabase = "REMOVED";

// Дескрипторы задач
extern TaskHandle_t taskWebServerHandle = NULL;
extern TaskHandle_t taskNRF905Handle = NULL;
extern TaskHandle_t taskCO2ReadHandle = NULL;
extern TaskHandle_t processNextionTaskHandle = NULL;
extern TaskHandle_t taskBMP280Handle = NULL;
extern TaskHandle_t taskSendDataToInfluxDBHandle = NULL;
extern TaskHandle_t taskForecasterHandle = NULL;
extern TaskHandle_t taskGetTimeHandle = NULL;
extern TaskHandle_t taskTVOCReadHandle = NULL;

// Мьютексы
SemaphoreHandle_t i2cMutex;
SemaphoreHandle_t driverMutex;
portMUX_TYPE mutexMux = portMUX_INITIALIZER_UNLOCKED; // Глобальный объект критической секции

// Таймер
TimerHandle_t wifiTimer;

// Флаги задач
volatile bool webServerRunning = false;
volatile bool nRF905Running = false;
volatile bool CO2ReadRunning = false;
volatile bool processNextionRunning = false;
volatile bool BMP280Running = false;
volatile bool sendDataToInfluxDBRunning = false;
volatile bool forecasterRunning = false;
volatile bool getTimeRunning = false;
volatile bool TVOCReadRunning = false;

// Функция проверки состояния задачи
bool isTaskActive(TaskHandle_t taskHandle) {
  if (taskHandle == NULL) return false; // Задача не создана
  
  eTaskState state = eTaskGetState(taskHandle);
  return (state == eRunning) || 
         (state == eReady) || 
         (state == eBlocked);
}

// ---------------------------- Пины и устройства ----------------------------
#define NRF905_SPI_SCK 14
#define NRF905_SPI_MISO 12
#define NRF_SPI_MOSI 13
#define NRF905_CE 27
#define NRF905_TX_EN 25
#define NRF905_CS 15
#define NRF905_PWR_UP_PIN 26 // пин принудительного сброса nRF905


// --------------------------------- Объекты ---------------------------------

// ------------------------------ Координаты ---------------------------------

double lat = 50.5302;
double lon = 137.0044;
int tzOffset = 10; // Часовой пояс (UTC+10)

// ---------------------- Определение пинов serial2 --------------------------
HardwareSerial nextion(2); // Используем Serial2 для связи с дисплеем
#define RX2 16  // RX пин ESP32
#define TX2 17  // TX пин ESP32


//----------------------- Определение пинов Serial1 --------------------------

HardwareSerial mh19(1); //Serial1 для датчика CO2
#define RX1 32
#define TX1 33


Adafruit_BME280 bme;                                  // Датчик давления BMP280
SparkFun_ENS160 ens160;
AHT20 aht20;                                  // Датчик компенсации T и H AHT21
//HMC5883L mag;
RH_NRF905 driver(NRF905_CE, NRF905_TX_EN, NRF905_CS); // Радиомодуль nRF905
AsyncWebServer server(80);      // Асинхронный HTTP сервер
AsyncWebSocket webSocket("/ws"); // Асинхронный WebSocket сервер
AsyncWebSocket webSocket1("/ws1");
Forecaster cond;
SunSet sun;



// -------------------------- Объявления функций (прототипы) --------------------------
void switchTaskTVOCRead();
void switchTaskBMP280();
void sendGraphData();
void sendCommand();
void syncWebServerButtonState();
void processNextionTask();
void handleGraphData();
void handleRoot();
void nextionRestart();
void reconnectWiFi();
void checkMutex();
void processNextionTask(void *pvParameters);
void taskForecast(void *pvParameters);
void taskCO2Read(void *pvParameters);
void taskGetTime(void *pvParameters);
void taskNRF905(void *pvParameters);
void taskBMP280(void *pvParameters);
void taskTVOCRead(void *pvParameters);
void taskSendDataToInfluxDB(void *pvParameters);
float calculateDewPoint(float temperature, float humidity);

// --------------------------- Глобальные переменные ---------------------------
volatile float temperature = 0.0f;
volatile float humidity = 0.0f;
volatile float dewPoint = 0.0f;
volatile float pressure = 0.0f;
volatile float homeTemp = 0.0f;
volatile float homeHum = 0.0f;
volatile float homeDP = 0.0f;
volatile float trend = 0.0f;
float es(float T) {
  return 6.112 * exp((17.62 * T) / (243.12 + T)); // давление насыщенного пара, мбар
}
float forecast = 0;
int month = -1;
volatile int ppm = 400;
volatile int TVOC = 0;
volatile int AQI = 1;
volatile int ECO2 = 400;
volatile uint32_t i2cResetCount = 0;
double sunriseTime;
double sunsetTime;

// Переменная для хранения текущей страницы Nextion
String currentPage = "page0";

// Статус датчика ENS160
int ensStatus; 

// Параметры попыток
const uint8_t MAX_ATTEMPTS_PER_CYCLE = 3;
const uint8_t MAX_CYCLES = 3;  // всего циклов попыток
const uint32_t SHORT_COOLDOWN = 30000;   // 30 секунд между попытками в одном цикле
const uint32_t LONG_COOLDOWN  = 300000;   // 5 минут между циклами

// Глобальные переменные
uint8_t wifi_attempts = 0;  // суммарное число попыток (максимум 9)
uint32_t last_reconnect_time = 0;

// -------------------------- Функция расчета точки росы -----------------------------
float calculateDewPoint(float temperature, float humidity)
{
  float a = 17.27;
  float b = 237.7;
  float alpha = ((a * temperature) / (b + temperature)) + log(humidity / 100.0);
  return (b * alpha) / (a - alpha);
}

float calculatehomeDP(float homeTemp, float homeHum)
{
  float a = 17.27;
  float b = 237.7;
  float alpha = ((a * homeTemp) / (b + homeHum)) + log(homeHum / 100.0);
  return (b * alpha) / (a - alpha);
}

// Инициализация файловой системы
void setupFileSystem()
{
  if (!LittleFS.begin())
  {
    ESP_LOGE("INIT", "Failed to initialize LittleFS");
  }
  else
  {
    ESP_LOGI("INIT", "LittleFS initialized");
  }
}

// ---------------------------- Обработчики HTTP запросов -----------------------------

// Функция авторизации
bool isAuthenticated(AsyncWebServerRequest *request) {
  if (!request->authenticate(http_username, http_password)) {
      request->requestAuthentication();
      return false;
  }
  return true;
}


void handleGraphData(AsyncWebServerRequest *request) {
  StaticJsonDocument<256> doc;
  
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["dewPoint"] = dewPoint;
  doc["pressure"] = pressure;
  doc["homeTemp"] = homeTemp;
  doc["homeHum"] = homeHum;
  doc["homeDP"] = homeDP;
  doc["forecast"] = forecast;
  doc["trend"] = trend;
  doc["CO2"] = ppm;
  doc["TVOC"] = TVOC;

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}


// Общая функция для формирования JSON состояния задач
void buildTaskStateJson(char* buffer, size_t bufferSize) {
  const char* jsonFormat = 
    "{\"webServer\":%s,\"nRF905\":%s,\"CO2\":%s,\"nextion\":%s,"
    "\"BMP280\":%s,\"InfluxDB\":%s,\"Forecaster\":%s,\"NTP\":%s,\"TVOC\":%s}";
    
  snprintf(buffer, bufferSize, jsonFormat,
    isTaskActive(taskWebServerHandle) ? "true" : "false",
    isTaskActive(taskNRF905Handle) ? "true" : "false",
    isTaskActive(taskCO2ReadHandle) ? "true" : "false",
    isTaskActive(processNextionTaskHandle) ? "true" : "false",
    isTaskActive(taskBMP280Handle) ? "true" : "false",
    isTaskActive(taskSendDataToInfluxDBHandle) ? "true" : "false",
    isTaskActive(taskForecasterHandle) ? "true" : "false",
    isTaskActive(taskGetTimeHandle) ? "true" : "false",
    isTaskActive(taskTVOCReadHandle) ? "true" : "false");
}

// HTTP обработчик
void handleGetTasksState(AsyncWebServerRequest *request) {
  char jsonBuffer[256]; // С запасом для будущих полей
  
  buildTaskStateJson(jsonBuffer, sizeof(jsonBuffer));
  ESP_LOGD("WEB", "Sending task states via HTTP: %s", jsonBuffer);
  
  request->send(200, "application/json", jsonBuffer);
}

// WebSocket отправка
void sendTaskStateUpdate() {
  char jsonBuffer[256];
  
  buildTaskStateJson(jsonBuffer, sizeof(jsonBuffer));
  webSocket.textAll(jsonBuffer);
  
  ESP_LOGD("WEB", "Sending task states to WS: %s", jsonBuffer);
}

void sendTimeData() {
  if(webSocket1.count() > 0) {
    StaticJsonDocument<128> json;
    json["nowTime"] = time(nullptr);
    json["sunriseTime"] = sunriseTime * 60;
    json["sunsetTime"] = sunsetTime * 60;

    // Статический буфер + проверка переполнения
    static char jsonBuffer[160]; // Размер должен быть больше максимально возможного JSON
    const size_t len = serializeJson(json, jsonBuffer, sizeof(jsonBuffer));
    
    if(len >= sizeof(jsonBuffer) - 1) {
      ESP_LOGE("WEB", "JSON buffer overflow!");
      return;
    }
    
    webSocket1.textAll(jsonBuffer, len);
    ESP_LOGD("WEB", "Sending time data to WS: %.*s", len, jsonBuffer);
  }
}


void handleRoot(AsyncWebServerRequest *request) {
  if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
  } else {
      request->send(404, "text/plain", "index.html not found");
  }
}

void handleUpdateForm(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) {
      request->send(401, "text/plain", "Unauthorized");
      return;
  }

  if (LittleFS.exists("/updateform.html")) {
      request->send(LittleFS, "/updateform.html", "text/html");
  } else {
      request->send(404, "text/plain", "updateform.html not found");
  }
}

void handleUpdateUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!isAuthenticated(request)) {
      ESP_LOGW("WEB", "Authentication failed");
      request->send(401, "text/plain", "Unauthorized");
      return;
  }

  if (index == 0) {
      ESP_LOGW("UPDATE", "Update started: %s\n", filename.c_str());

      if (filename.indexOf("littlefs") >= 0) {
          ESP_LOGW("UPDATE", "Updating filesystem");
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
              Update.printError(Serial);
              request->send(500, "text/plain", "Failed to start filesystem update");
              return;
          }
      } else {
          ESP_LOGW("UPDATE", "Updating firmware");
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
              Update.printError(Serial);
              request->send(500, "text/plain", "Failed to start firmware update");
              return;
          }
      }
  }

  if (len > 0) {
      if (Update.write(data, len) != len) {
          Update.printError(Serial);
          request->send(500, "text/plain", "Error writing update data");
          return;
      }
  }

  if (final) {
      if (Update.end(true)) {
          ESP_LOGW("UPDATE", "Update completed: %u bytes\n", index + len);
      } else {
          Update.printError(Serial);
          request->send(500, "text/plain", "Update failed");
          return;
      }
  }
}

void handleUpdateEnd(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");

  if (!Update.hasError()) {
      ESP_LOGW("UPDATE", "Update successful, restarting...");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      ESP.restart();
  } else {
      ESP_LOGE("UPDATE", "Update failed");
  }
}


void handleAdmin(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(LittleFS, "/REMOVED.html", "text/html");
}

void handleAbout(AsyncWebServerRequest *request) {
  ESP_LOGV("WEB", "Attempting to load /about.html");
  if (LittleFS.exists("/about.html")) {
      ESP_LOGV("WEB", "/about.html found, sending...");
      request->send(LittleFS, "/about.html", "text/html");
  } else {
      ESP_LOGE("WEB", "/about.html not found");
      request->send(404, "text/plain", "about.html not found");
  }
}

void handleRestart(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
    request->send(200, "text/plain", "ESP32 is restarting...");
    ESP_LOGW("SYS", "ESP32 is restarting from command");
    nextionRestart();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
}

void handleRestartFromNextion() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  ESP.restart();
}

void resetNRF905() {
  ESP_LOGW("NRF905", "Performing nRF905 reset...");
  digitalWrite(NRF905_PWR_UP_PIN, LOW);  // Выключаем питание (пин LOW)
  vTaskDelay(200 / portTICK_PERIOD_MS);  // Задержка по datasheet
  digitalWrite(NRF905_PWR_UP_PIN, HIGH); // Включаем питание (пин HIGH)
  ESP_LOGW("NRF905", "nRF905 reset complete.");
  vTaskDelay(100 / portTICK_PERIOD_MS);
   // Переинициализация и настройка модуля
  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    if (driver.init()) {
        ESP_LOGW("NRF905", "nRF905 reinitialized successfully.");
        driver.setChannel(175, false); // Канал 175 = 439.9 МГц
        driver.setRF(RH_NRF905::TransmitPowerm2dBm);
        // Другие необходимые настройки...
      } else {
        ESP_LOGE("NRF905", "Failed to reinitialize nRF905.");
      }
    xSemaphoreGive(driverMutex);
    }
}

void resetI2CBus()
{
  i2cResetCount++;
  vSemaphoreDelete(i2cMutex);
  ESP_LOGE("SYS", "Deleting i2cMutex. Resetting I2C bus...");
  pinMode(22, OUTPUT);
  pinMode(21, INPUT_PULLUP); // NACK-сигнал для датчиков

  // Генерируем 10 импульсов на SCL
  for (int i = 0; i < 10; i++)
  {
    digitalWrite(22, HIGH);
    delayMicroseconds(5);
    digitalWrite(22, LOW);
    delayMicroseconds(5);
  }

  // Устанавливаем SCL в HIGH (чтобы освободить шину)
  digitalWrite(22, HIGH);
  vTaskDelay(10 / portTICK_PERIOD_MS);

  // Возвращаем SCL и SDA в режим работы с I2C
  Wire.end();         // Завершаем работу с шиной
  Wire.begin(21, 22); // Переинициализируем I2C
  checkMutex();
  ESP_LOGW("SYS", "Re-creating i2cMutex. I2C bus was reseted");

  ens160.setOperatingMode(SFE_ENS160_RESET);

  if (!bme.begin(0x76))
  {
    ESP_LOGE("INIT", "BME280 not detected. Please check wiring!");
  }
  if (!ens160.begin())
  {
    ESP_LOGE("INIT", "ENS160 not detected. Please check wiring!");
  }
  if (!aht20.begin())
  {
    ESP_LOGE("INIT", "AHT20 not detected. Please check wiring!");
  }
  vTaskDelay(100 / portTICK_PERIOD_MS);
}

void handleNRFReset(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  resetNRF905();
  request->send(200, "text/plain", "nRF905 has been reset.");
}

std::string formatTime(int value) {
  char buffer[3]; // "00" + null terminator
  snprintf(buffer, sizeof(buffer), "%02d", value);
  return std::string(buffer);
}

void getSystemInfo(char *buffer, size_t len) {
  unsigned long uptime = millis() / 1000;
  int days = uptime / 86400;
  int hours = (uptime % 86400) / 3600;
  int minutes = (uptime % 3600) / 60;
  int seconds = uptime % 60;

  char uptimeStr[20];
  snprintf(uptimeStr, sizeof(uptimeStr), "%dd %02dh %02dm %02ds", days, hours, minutes, seconds);

  snprintf(buffer, len,
           "Uptime: %s\nChip model: %s\nChip rev.: %d\nWiFi RSSI: %d dBm\n"
           "IP address: %s\nFree Heap: %d bytes\nWebServer task free stack: %d bytes\n"
           "InfluxDB task free stack: %d bytes\n",
           uptimeStr, ESP.getChipModel(), ESP.getChipRevision(), WiFi.RSSI(),
           WiFi.localIP().toString().c_str(), ESP.getFreeHeap(),
           uxTaskGetStackHighWaterMark(NULL),
           uxTaskGetStackHighWaterMark(taskSendDataToInfluxDBHandle));
}


// Функция получения статуса датчика BME280 по I2C
const char *modeToString(uint8_t mode) {
  switch (mode) {
    case 0x00: return "DEEP_SLEEP";
    case 0x01: return "IDLE";
    case 0x02: return "STANDARD";
    case 0xF0: return "RESET";
    default:   return "UNKNOWN";
  }
}

void getBME280Status(char *buffer, size_t len) {
  size_t offset = 0;

  // Проверяем, инициализирован ли мьютекс, если нет — создаём
  checkMutex();
  
  if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
    // Проверка BME280
    if (!bme.begin(0x76)) {
      offset += snprintf(buffer + offset, len - offset, "BME280: Not Found\n");
    } else {
      offset += snprintf(buffer + offset, len - offset, "BME280: Found\n");
      offset += snprintf(buffer + offset, len - offset, "Temp: %.2f °C\n", bme.readTemperature());
      offset += snprintf(buffer + offset, len - offset, "Humidity: %.2f %%\n", bme.readHumidity());
      offset += snprintf(buffer + offset, len - offset, "Pressure: %.2f hPa\n", bme.readPressure() / 100.0F);
    }
    
    // Проверка ENS160
    if (!ens160.begin()) {
      offset += snprintf(buffer + offset, len - offset, "ENS160: Not Found\n");
    } else {
      offset += snprintf(buffer + offset, len - offset, "ENS160: Found\n");
      offset += snprintf(buffer + offset, len - offset, "Mode: %s\n", modeToString(ens160.getOperatingMode()));
      offset += snprintf(buffer + offset, len - offset, "Status: %s\n", ens160.getOperationError() ? "Error" : "OK");
      offset += snprintf(buffer + offset, len - offset, "I2C Reset Count: %d\n", i2cResetCount);
    }
    
    xSemaphoreGive(i2cMutex);
  } else if (i2cMutex == NULL) {
    snprintf(buffer, len, "i2cMutex is NULL! Failed to acquire i2cMutex! From task - %s\n", pcTaskGetTaskName(NULL));
    ESP_LOGE("MUTEX", "i2cMutex is NULL! Task: %s", pcTaskGetTaskName(NULL));
} else {
    ESP_LOGE("MUTEX", "Failed to acquire i2cMutex! From task: %s | Mutex holder: %s", 
        pcTaskGetTaskName(NULL), 
        xSemaphoreGetMutexHolder(i2cMutex) ? pcTaskGetTaskName(xSemaphoreGetMutexHolder(i2cMutex)) : "None");
    resetI2CBus();
  }
}


void getNRF905Status(char *buffer, size_t bufferSize) {
  char temp[32];          
  uint8_t config[10];

  // Чтение регистров
  uint8_t status_reg = driver.spiBurstReadRegister(RH_NRF905_REG_W_CONFIG, config, 10);

  // Заполнение строки статуса
  int pos = snprintf(buffer, bufferSize, "Status Register: 0x%02X\nDecoded Status:\n", status_reg);

  if (status_reg & 0x20) pos += snprintf(buffer + pos, bufferSize - pos, "[DR] Data Ready\n");
  if (status_reg & 0x80) pos += snprintf(buffer + pos, bufferSize - pos, "[AM] Address Match\n");
  if (status_reg & 0x40) pos += snprintf(buffer + pos, bufferSize - pos, "[CRC_ERR] Error\n");
  else pos += snprintf(buffer + pos, bufferSize - pos, "[CRC_OK]\n");

  // Конфигурация канала
  pos += snprintf(buffer + pos, bufferSize - pos, "Channel: %d\n", config[0]);

  // Частота
  uint8_t band_bit = config[1] & RH_NRF905_CONFIG_1_HFREQ_PLL;
  float freq = 422.4 + (config[0] / 10.0);
  if (band_bit) freq *= 2;

  pos += snprintf(buffer + pos, bufferSize - pos, "Frequency: %.3f MHz\n", freq);

  // Мощность передачи
  uint8_t pwr = (config[1] & RH_NRF905_CONFIG_1_PA_PWR) >> 2;
  const char* pwr_str[] = {"-10 dBm", "-2 dBm", "+6 dBm", "+10 dBm"};
  pos += snprintf(buffer + pos, bufferSize - pos, "TX Power: %s\n", pwr_str[pwr]);

  // RAW Config
  pos += snprintf(buffer + pos, bufferSize - pos, "RAW Config: ");
  for (int i = 0; i < 10; i++) {
      snprintf(temp, sizeof(temp), "%02X ", config[i]);
      strncat(buffer, temp, bufferSize - strlen(buffer) - 1);
  }
  strncat(buffer, "\n", bufferSize - strlen(buffer) - 1);
}



RH_NRF905::TransmitPower getTransmitPowerFromString(const String &powerStr) {
  if (powerStr == "TransmitPowerm10dBm") {
    return RH_NRF905::TransmitPowerm10dBm;  // -10 dBm
  }
  else if (powerStr == "TransmitPowerm2dBm") {
    return RH_NRF905::TransmitPowerm2dBm;   // -2 dBm
  }
  else if (powerStr == "TransmitPower6dBm") {
    return RH_NRF905::TransmitPower6dBm;    // 6 dBm
  }
  else if (powerStr == "TransmitPower10dBm") {
    return RH_NRF905::TransmitPower10dBm;   // 10 dBm
  }
  // Значение по умолчанию
  return RH_NRF905::TransmitPower10dBm;
}

void handleSysInfo(AsyncWebServerRequest *request) {
  static char info[256];  // Размер буфера нужно подобрать под ваши данные
  getSystemInfo(info, sizeof(info));
  request->send(200, "text/plain", info);
}

void handleBMEInfo(AsyncWebServerRequest *request) {
  static char statusBuffer[256];  // Буфер для ответа
  getBME280Status(statusBuffer, sizeof(statusBuffer));
  request->send_P(200, "text/plain", statusBuffer);
}

void handlenRFInfo(AsyncWebServerRequest *request) {
  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
      static char status[256];  
      getNRF905Status(status, sizeof(status));
      request->send_P(200, "text/plain", status);
      xSemaphoreGive(driverMutex);
  }
}

void handleSetNRF905(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) {
      request->send(401, "text/plain", "Unauthorized");
      return;
  }

  // Проверка наличия всех необходимых параметров
  if (request->hasParam("channel", true) && 
      request->hasParam("band", true) && 
      request->hasParam("power", true)) {

      // Получение параметров из POST-запроса (нужно указать true для чтения из тела запроса)
      int channel = request->getParam("channel", true)->value().toInt();
      bool band = (request->getParam("band", true)->value() == "true");
      String powerStr = request->getParam("power", true)->value();

      ESP_LOGI("NRF905", "Settings received: channel = %d, band = %s, power = %s\n",
                    channel, (band ? "hiband" : "lowband"), powerStr.c_str());

      if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
          // Применение настроек через драйвер
          driver.setChannel(channel, band);
          driver.setRF(getTransmitPowerFromString(powerStr));
          xSemaphoreGive(driverMutex);
      }

      request->send(200, "text/plain", "Settings nRF905 applied");
  } else {
      request->send(400, "text/plain", "Invalid parameters");
  }
}

// Функция для отправки данных в InfluxDB (без аргументов)
void sendDataToInfluxDB()
{
    WiFiClient client;
    HTTPClient http;

    char influxDBLine[256];
    int offset = snprintf(influxDBLine, sizeof(influxDBLine), "weather,location=home ");

    if (temperature != 0.0f)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "temperature=%.2f,", temperature);
    }

    if (humidity != 0.0f)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "humidity=%.2f,", humidity);
    }

    if (dewPoint != 0.0f)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "dewPoint=%.2f,", dewPoint);
    }

    if (pressure != 0.0f)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "pressure=%.2f,", pressure);
    }

    if (forecast >= 0)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "forecast=%.2f,", forecast);
    }

    if (trend >= -30 && trend <= 30)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "trend=%.2f,", trend);
    }
    else
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "trend=%.2f,", 0.0f);
    }

    if (homeTemp != 0.0f)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "homeTemp=%.2f,", homeTemp);
    }

    if (homeHum != 0.0f)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "homeHum=%.2f,", homeHum);
    }

    if (homeDP != 0.0f)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "homeDP=%.2f,", homeDP);
    }

    if (ppm != 0)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "CO2=%d,", ppm);
    }

    if (TVOC != -1)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "TVOC=%d,", TVOC);
    }

    if (AQI != -1)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "AQI=%d,", AQI);
    }

    if (ECO2 != -1)
    {
        offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset, "ECO2=%d", ECO2);
    }

    // Убираем последнюю запятую, если она есть
    if (influxDBLine[offset - 1] == ',')
    {
        influxDBLine[offset - 1] = '\0';
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/write?db=%s", influxDBHost, influxDBPort, influxDBDatabase);

    http.begin(client, url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int httpResponseCode = http.POST(influxDBLine);

    if (httpResponseCode > 0)
    {
        ESP_LOGI("InfluxDB", "HTTP Response code: %d", httpResponseCode);
        if (httpResponseCode != 204)
        {
            ESP_LOGD("InfluxDB", "Server response: %s", http.getString().c_str()); // Логируем ответ сервера на уровне DEBUG
        }
    }
    else
    {
        ESP_LOGE("InfluxDB", "Error sending data to InfluxDB: %s", http.errorToString(httpResponseCode).c_str());
    }

    http.end();
}


// Функция получения номера месяца
int getMonth() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    return timeinfo.tm_mon + 1;  // tm_mon возвращает месяц от 0 до 11
  }
  return -1;  // Ошибка получения месяца
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
      ESP_LOGI("WS", "WebSocket client #%u connected from %s", client->id(), client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
      ESP_LOGI("WS", "WebSocket client #%u disconnected", client->id());
  } else if (type == WS_EVT_DATA) {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
          ESP_LOGD("WS", "Received message: %.*s", len, (char *)data);
      }
  }
}

void onWsEvent1(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
      ESP_LOGI("WS", "Client connected to /ws1");
  } else if (type == WS_EVT_DATA) {
      if (strncmp((char *)data, "getTime", len) == 0) {
          ESP_LOGD("WS", "Received 'getTime' request");
          sendTimeData();
      }
  }
}

// ----------------------------- nextion finalizer -----------------------

void nextionFin() {
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

// ----------------------- Функция проверки мьютека ----------------------

void checkMutex() {
  taskENTER_CRITICAL(&mutexMux);  // Входим в критическую секцию

  if (i2cMutex == NULL) {
    i2cMutex = xSemaphoreCreateMutex();
    if (i2cMutex == NULL) {
      ESP_LOGE("MUTEX", "Failed to create i2cMutex! Task: %s", pcTaskGetTaskName(NULL));
    } else {
      ESP_LOGI("MUTEX", "Created i2cMutex in task: %s", pcTaskGetTaskName(NULL));
    }
  }

  taskEXIT_CRITICAL(&mutexMux);  // Выходим из критической секции
}
// ----------------------- Функции запуска и остановки задач -------------
void nextionWakeUP()  {
  nextion.print("sleep=0");
  nextionFin();
}

void nextionSleep()  {
  nextion.print("sleep=1");
  nextionFin();
}

void nextionRestart() {
  nextion.print("rest");
  nextionFin();
}

void switchTaskInfluxDB() {
  if (isTaskActive(taskSendDataToInfluxDBHandle)) {
    // Если задача активна - останавливаем
    vTaskSuspend(taskSendDataToInfluxDBHandle);
    sendDataToInfluxDBRunning = false;
    ESP_LOGD("SYS", "InfluxDB: Stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskSendDataToInfluxDBHandle == NULL) {
      xTaskCreate(
        taskSendDataToInfluxDB,
        "InfluxDBTask", 
        4096,
        NULL,
        6,
        &taskSendDataToInfluxDBHandle
      );
      ESP_LOGD("SYS", "InfluxDB: created and running");
    } else {
      vTaskResume(taskSendDataToInfluxDBHandle);
      ESP_LOGD("SYS", "InfluxDB: resumed");
    }
    sendDataToInfluxDBRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskCO2Read() {
  if (isTaskActive(taskCO2ReadHandle)) {
    // Если задача активна - останавливаем
    mh19.end();
    vTaskSuspend(taskCO2ReadHandle);
    CO2ReadRunning = false;
    ESP_LOGD("SYS", "CO2 Task: stopped");
  } else {
    mh19.begin(9600, SERIAL_8N1, RX1, TX1);
    // Если задача неактивна - запускаем/возобновляем
    if (taskCO2ReadHandle == NULL) {
      xTaskCreate(
        taskCO2Read,
        "CO2 read task", 
        2048,
        NULL,
        3,
        &taskCO2ReadHandle
      );
      ESP_LOGD("SYS", "CO2 Task: created and running");
    } else {
      vTaskResume(taskCO2ReadHandle);
      ESP_LOGD("SYS", "CO2 Task: resumed");
    }
    CO2ReadRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskNRF905() {
  if (isTaskActive(taskNRF905Handle)) {
    // Если задача активна - останавливаем
    vTaskSuspend(taskNRF905Handle);
    nRF905Running = false;
    ESP_LOGD("SYS", "NRF905 Task: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskNRF905Handle == NULL) {
      xTaskCreate(
        taskNRF905,
        "NRF905 Receiver", 
        4096,
        NULL,
        5,
        &taskNRF905Handle
      );
      ESP_LOGD("SYS", "NRF905 Task: created and running");
    } else {
      vTaskResume(taskNRF905Handle);
      ESP_LOGD("SYS", "NRF905 Task: resumed");
    }
    nRF905Running = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskNextion() {
  if (isTaskActive(processNextionTaskHandle)) {
    // Если задача активна - останавливаем
    nextionSleep();
    vTaskDelay(200 / portTICK_PERIOD_MS);
    vTaskSuspend(processNextionTaskHandle);
    processNextionRunning = false;
    ESP_LOGD("SYS", "Nextion Task: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (processNextionTaskHandle == NULL) {
      xTaskCreate(
        processNextionTask,
        "Nextion", 
        4096,
        NULL,
        3,
        &processNextionTaskHandle
      );
      ESP_LOGD("SYS", "Nextion Task: created and running");
    } else {
      vTaskResume(processNextionTaskHandle);
      nextionWakeUP();
      ESP_LOGD("SYS", "Nextion Task: resumed");
    }
    processNextionRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskBMP280() {
  if (isTaskActive(taskBMP280Handle)) {
    // Если задача активна - останавливаем
    vTaskSuspend(taskBMP280Handle);
    BMP280Running = false;
    ESP_LOGD("SYS", "BME280 Task: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskBMP280Handle == NULL) {
      xTaskCreate(
        taskBMP280,
        "BMP280 Sensor", 
        2048,
        NULL,
        4,
        &taskBMP280Handle
      );
      ESP_LOGD("SYS", "BME280 Task: created and running");
    } else {
      vTaskResume(taskBMP280Handle);
      ESP_LOGD("SYS", "BME280 Task: resumed");
    }
    BMP280Running = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskForecaster() {
  if (isTaskActive(taskForecasterHandle)) {
    // Если задача активна - останавливаем
    vTaskSuspend(taskForecasterHandle);
    forecasterRunning = false;
    ESP_LOGD("SYS", "Forecast Task: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskForecasterHandle == NULL) {
      xTaskCreate(
        taskForecast,
        "Forecast task", 
        2048,
        NULL,
        1,
        &taskForecasterHandle
      );
      ESP_LOGD("SYS", "Forecast Task: created and running");
    } else {
      vTaskResume(taskForecasterHandle);
      ESP_LOGD("SYS", "Forecast Task: resumed");
    }
    forecasterRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskNTP() {
  if (isTaskActive(taskGetTimeHandle)) {
    // Если задача активна - останавливаем
    vTaskSuspend(taskGetTimeHandle);
    getTimeRunning = false;
    ESP_LOGD("SYS", "NTP Task: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskGetTimeHandle == NULL) {
      xTaskCreate(
        taskGetTime,
        "Get NTP Time", 
        4096,
        NULL,
        2,
        &taskGetTimeHandle
      );
      ESP_LOGD("SYS", "NTP Task: created and running");
    } else {
      vTaskResume(taskGetTimeHandle);
      ESP_LOGD("SYS", "NTP Task: resumed");
    }
    getTimeRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskTVOCRead() {
  if (isTaskActive(taskTVOCReadHandle)) {
    // Если задача активна - останавливаем
    // Проверяем, инициализирован ли мьютекс, если нет — создаём
    checkMutex();
    if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
        {
          ens160.setOperatingMode(SFE_ENS160_DEEP_SLEEP);
          xSemaphoreGive(i2cMutex);
        }
    vTaskSuspend(taskTVOCReadHandle);
    TVOCReadRunning = false;
    ESP_LOGD("SYS", "TVOC reading Task: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskTVOCReadHandle == NULL) {
      if (!ens160.begin()) {
        ESP_LOGE("INIT", "ENS160 not detected. Please check wiring!");
      } else {
        ESP_LOGI("INIT", "ENS160 detected");  
      }
  
      if (!aht20.begin()) {
        ESP_LOGE("INIT", "AHT21 not detected. Please check wiring!");
      } else {
        ESP_LOGI("INIT", "AHT21 detected");
    }
      xTaskCreate(
        taskTVOCRead,
        "ENS160 read task", 
        4096,
        NULL,
        2,
        &taskTVOCReadHandle
      );
      ESP_LOGD("SYS", "TVOC reading Task: created and running");
    } else {
      checkMutex();
      if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
        {
          ens160.setOperatingMode(SFE_ENS160_RESET);
          vTaskDelay(100 / portTICK_PERIOD_MS);
          ens160.setOperatingMode(SFE_ENS160_STANDARD);
          xSemaphoreGive(i2cMutex);
        }
      vTaskResume(taskTVOCReadHandle);
      ESP_LOGD("SYS", "TVOC reading Task: resumed");
    }
    TVOCReadRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void handleTaskControl(AsyncWebServerRequest *request) {
  // Проверяем наличие параметра "task" в теле POST-запроса
  if (!request->hasParam("task", true)) {
      request->send(400, "text/plain", "Bad Request: Missing task parameter");
      return;
  }

  // Получаем значение параметра "task" из тела запроса
  String task = request->getParam("task", true)->value();
  ESP_LOGD("SYS", "Task switch request received: %s\n", task.c_str());

  // Выбор задачи для переключения
  if (task == "CO2") {
      switchTaskCO2Read();
  } else if (task == "nRF905") {
      switchTaskNRF905();
  } else if (task == "nextion") {
      switchTaskNextion();
  } else if (task == "BMP280") {
      switchTaskBMP280();
  } else if (task == "InfluxDB") {
      switchTaskInfluxDB();
  } else if (task == "Forecaster") {
      switchTaskForecaster();
  } else if (task == "NTP") {
      switchTaskNTP();
  } else if (task == "TVOC") {
      switchTaskTVOCRead();
  } else {
      request->send(400, "text/plain", "Bad Request: Unknown task");
      return;
  }

  // Обновляем статус задач и отправляем JSON-ответ клиенту
  sendTaskStateUpdate();
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

// --------------------------- Задачи FreeRTOS ---------------------------

void taskSendDataToInfluxDB(void *pvParameters)
{
  while (1)
  {

    sendDataToInfluxDB();

    vTaskDelay(36000 / portTICK_PERIOD_MS); // Отправка данных раз в минуту
  }
}

void taskNRF905(void *pvParameters)
{
  // Запоминаем время последнего успешного получения данных
  unsigned long lastReceived = millis();

  while (true)
  {
    if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
      if (driver.available())
      {
        uint8_t buf[RH_NRF905_MAX_MESSAGE_LEN + 1];
        uint8_t len = sizeof(buf) - 1;
        if (driver.recv(buf, &len))
        {
          buf[len] = '\0'; // Завершаем строку
          ESP_LOGD("NRF905", "Raw data received: %s", (char *)buf);

          float temp, hum;
          if (sscanf((char *)buf, "T:%f H:%f", &temp, &hum) == 2)
          {
            // Обновляем глобальные переменные
            temperature = temp;
            humidity = hum;
            dewPoint = calculateDewPoint(temperature, humidity);

            // Сбрасываем таймер, так как данные получены
            lastReceived = millis();
          }
        }
      }
    xSemaphoreGive(driverMutex);
    }
    
    // Проверяем, прошло ли больше 10 минут без получения данных
    if ((millis() - lastReceived) >= 600000)  // 600000 мс = 10 минут
    {
      ESP_LOGE("NRF905", "No data available for more than 10 minutes. Resetting nRF905...");
      resetNRF905();  // Вызываем функцию сброса nRF905
      // Обновляем lastReceived, чтобы не вызывать сброс повторно сразу же
      lastReceived = millis();
    }
    
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Задержка 1 секунда
  }
}

void taskBMP280(void *pvParameters)
{
  while (true)
  {
    checkMutex();
    if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
    {
      float rawTemp = bme.readTemperature();
      float rawHum  = bme.readHumidity();
      pressure      = bme.readPressure() / 100.0f; // гПа

      // Корректируем температуру
      homeTemp = rawTemp - 3.0f;

      // Корректируем влажность по формуле
      float eRaw  = es(rawTemp);
      float eCorr = es(homeTemp);
      homeHum = rawHum * (eCorr / eRaw);
      if (homeHum > 100.0f) homeHum = 100.0f; // ограничение

      xSemaphoreGive(i2cMutex);
      homeDP = calculatehomeDP(homeTemp, homeHum);
    }
    else
    {
      ESP_LOGE("MUTEX", "Failed to acquire i2cMutex! From task: %s | Mutex holder: %s", 
        pcTaskGetTaskName(NULL), 
        xSemaphoreGetMutexHolder(i2cMutex) ? pcTaskGetTaskName(xSemaphoreGetMutexHolder(i2cMutex)) : "None");
      resetI2CBus();
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); // Задержка 5 секунд
  }
}

// void taskGeomagnetic(void *pvParameters) {
//   int16_t x_raw, y_raw, z_raw;

//   while (true) {
//     checkMutex();
//     if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
//     {
//       if (mag.readRaw(x_raw, y_raw, z_raw)) {
//         float x = x_raw * mag.scale;
//         float y = y_raw * mag.scale;
//         float z = z_raw * mag.scale;
//         float B = sqrt(x*x + y*y + z*z);
//         ESP_LOGD("SENSORS", "Mag field: X=%.2f Y=%.2f Z=%.2f B=%.2f uT\n", x, y, z, B);
//         xSemaphoreGive(i2cMutex);
//       } else {
//         ESP_LOGE("SENSORS", "Read sensor HMC5883L error!");
//       }
//     }
//     else
//     {
//       // Ошибка: превышено время ожидания мьютекса
//       ESP_LOGE("MUTEX", "Failed to acquire i2cMutex! From task: %s | Mutex holder: %s", 
//         pcTaskGetTaskName(NULL), 
//         xSemaphoreGetMutexHolder(i2cMutex) ? pcTaskGetTaskName(xSemaphoreGetMutexHolder(i2cMutex)) : "None");
//       resetI2CBus();
//     }

//     vTaskDelay(pdMS_TO_TICKS(5000)); // 10 секунд
//   }
// }

void taskGetTime(void *pvParameters)
{
  static int currentDay = 32;
  struct tm timeinfo;

  for (;;)
  { // Бесконечный цикл в задаче
    if (getLocalTime(&timeinfo))
    {
      if (currentDay != timeinfo.tm_mday)
      {
        sun.setCurrentDate(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        sun.setPosition(lat, lon, tzOffset);
        sunriseTime = sun.calcSunrise();
        sunsetTime = sun.calcSunset();
        currentDay = timeinfo.tm_mday;
      }

      // Обновляем текущий месяц
      month = timeinfo.tm_mon + 1;
    }
    else
    {
      ESP_LOGE("NTP", "Failed to get time via NTP.");
    }

    vTaskDelay(pdMS_TO_TICKS(60000)); // Проверка времени раз в минуту
  }

  configASSERT(NULL); // Если задача выйдет из цикла, это вызовет ошибку
}

void taskForecast(void *pvParameters) {
  for (;;) {
    int month = getMonth();
    if (month != -1) {
      ESP_LOGD("FORECAST", "Sending month to forecast: %i", month);
      cond.setMonth(month);  // Устанавливаем текущий месяц в Forecaster
    }
    int p = pressure * 100;
    if (p !=0 && !isnan(p)) {
    
    cond.addP(p, temperature);
    } else  {
      vTaskDelay((5000) / portTICK_PERIOD_MS);
    }
    ESP_LOGD("FORECAST", "Parameters sent to forecast: %i Pa", p);
    forecast = cond.getCast();
    trend = cond.getTrend() / 100.0f;
    ESP_LOGD("FORECAST", "Barometric trend: %f hPa\n Forecast: %f", trend, forecast);

    vTaskDelay((30 * 60 * 1000) / portTICK_PERIOD_MS);  // 30 минут задержки
  }
}

// Функция отправки данных на экран для page0
void sendPage0Data() {
  String cmd;
  
  // t0: humidity
  cmd = "t0.txt=\"" + String(humidity) + "%\"";
  nextion.print(cmd);
  nextionFin();
  
  // t1: temperature
  cmd = "t1.txt=\"" + String(temperature) + "°C\"";
  nextion.print(cmd);
  nextionFin();
  
  // t3: dewpoint
  cmd = "t3.txt=\"" + String(dewPoint) + "°C\"";
  nextion.print(cmd);
  nextionFin();
  
  // t2: pressure
  cmd = "t2.txt=\"" + String(pressure) + " hPa\"";
  nextion.print(cmd);
  nextionFin();
  
  // p0: картинка, определяем id по значению forecast
  int pic;
  if (forecast < 2)
    pic = 5;
  else if (forecast < 4.5)
    pic = 3;
  else if (forecast < 7)
    pic = 2;
  else
    pic = 4;
  
  cmd = "p0.pic=" + String(pic);
  nextion.print(cmd);
  nextionFin();
}

// Функция отправки данных на экран для page1
void sendPage1Data() {
  String cmd;
  
  // t0: homeHum
  cmd = "t0.txt=\"" + String(homeHum) + "%\"";
  nextion.print(cmd);
  nextionFin();
  
  // t1: homeTemp
  cmd = "t1.txt=\"" + String(homeTemp) + "°C\"";
  nextion.print(cmd);
  nextionFin();
  
  // t3: homeDP
  cmd = "t3.txt=\"" + String(homeDP) + "°C\"";
  nextion.print(cmd);
  nextionFin();
  
  // t2: pressure (используем ту же переменную)
  cmd = "t2.txt=\"" + String(pressure) + " hPa\"";
  nextion.print(cmd);
  nextionFin();

  // t4: CO2
  cmd = "t4.txt=\"" + String(ppm) + " ppm\"";
  nextion.print(cmd);
  nextionFin();

  // t5: TVOC
  cmd = "t5.txt=\"" + String(TVOC) + " ppb\"";
  nextion.print(cmd);
  nextionFin();
}

// Функция синхронизации состояния кнопок Nextion
void syncButtonState(int buttonId, TaskHandle_t taskHandle) {
  nextion.printf("bt%d.val=%d", buttonId, isTaskActive(taskHandle) ? 1 : 0);
  nextionFin();
}

// Отправка данных о состоянии кнопок на Nextion
void sendPage2Data() {
  syncButtonState(0, taskWebServerHandle);
  syncButtonState(1, taskNRF905Handle);
  syncButtonState(2, taskCO2ReadHandle);
  syncButtonState(3, processNextionTaskHandle);
  syncButtonState(4, taskBMP280Handle);
  syncButtonState(5, taskSendDataToInfluxDBHandle);
  syncButtonState(6, taskForecasterHandle);
  syncButtonState(7, taskGetTimeHandle);
}

// Обработчик бинарного сообщения от Nextion
void processNextionMessageBinary(const uint8_t* msg, size_t len) {
  // Минимальная длина сообщения должна быть 5 байт
  if (len < 5) return;

  // Проверяем, что последние три байта равны 0xFF
  if (!(msg[len-1] == 0xFF && msg[len-2] == 0xFF && msg[len-3] == 0xFF)) return;

  if (msg[0] == 0x66) {
    // Событие смены страницы
    uint8_t pageIndex = msg[1];
    switch (pageIndex) {
      case 0x00:
        currentPage = "page0";
        break;
      case 0x01:
        currentPage = "page1";
        break;
      case 0x02:
        currentPage = "page2";
        break;
      default:
        currentPage = "unknown";
        break;
    }
    ESP_LOGV("NEXTION", "Page changed: %s", currentPage);
  }
  else if (msg[0] == 0x65) {
    // Событие от компонента
    // Формат: 65, <PageID>, <ComponentID>, <EventValue>, FF, FF, FF
    uint8_t compID = msg[2];
    if (compID == 0x03) {
      ESP_LOGV("NEXTION", "Button pressed: b2");
      handleRestartFromNextion();
    }
    else if (compID == 0x04) {
      ESP_LOGV("NEXTION", "Button pressed: b3");
      resetNRF905();
    }
    else if (compID == 0x05)  {
      ESP_LOGV("NEXTION", "Button pressed: b4");
      ESP.restart();
    }
    else if (compID == 0x07) {
      ESP_LOGV("NEXTION", "Button pressed: bt1");
      // Переключаем веб-сервер
      switchTaskNRF905();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(1, taskNRF905Handle);
    }
    else if (compID == 0x08) {
      ESP_LOGV("NEXTION", "Button pressed: bt2");
      // Переключаем веб-сервер
      switchTaskCO2Read();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(2, taskCO2ReadHandle);
    }
    else if (compID == 0x09) {
      ESP_LOGV("NEXTION", "Button pressed: bt3");
      // Переключаем веб-сервер
      switchTaskNextion();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(3, processNextionTaskHandle);
    }
    else if (compID == 0x0A) {
      ESP_LOGV("NEXTION", "Button pressed: bt4");
      // Переключаем веб-сервер
      switchTaskBMP280();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(4, taskBMP280Handle);
    }
    else if (compID == 0x0B) {
      ESP_LOGV("NEXTION", "Button pressed: bt5");
      // Переключаем веб-сервер
      switchTaskInfluxDB();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(5, taskSendDataToInfluxDBHandle);
    }
    else if (compID == 0x0C) {
      ESP_LOGV("NEXTION", "Button pressed: bt6");
      // Переключаем веб-сервер
      switchTaskForecaster();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(6, taskForecasterHandle);
    }
    else if (compID == 0x0D) {
      ESP_LOGV("NEXTION", "Button pressed: bt7");
      // Переключаем веб-сервер
      switchTaskNTP();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(7, taskGetTimeHandle);
    }
  }
}


void processNextionTask(void * parameter) {
  const size_t bufSize = 32;
  uint8_t buffer[bufSize];
  size_t bufIndex = 0;
  unsigned long lastUpdateTime = millis();
  

  for (;;) {
    // Читаем доступные байты с Nextion (например, через HardwareSerial nextion)
    while (nextion.available()) {
      uint8_t b = nextion.read();
      // Добавляем байт в буфер, если есть место
      if (bufIndex < bufSize) {
        buffer[bufIndex++] = b;
      }
      // Если получено три 0xFF подряд, считаем, что пакет завершён
      if (bufIndex >= 3 &&
          buffer[bufIndex-1] == 0xFF &&
          buffer[bufIndex-2] == 0xFF &&
          buffer[bufIndex-3] == 0xFF) {
        // Обрабатываем пакет
        processNextionMessageBinary(buffer, bufIndex);
        bufIndex = 0; // очищаем буфер для следующего пакета
      }
      // Если буфер переполнен без корректного завершения, сбрасываем его
      if (bufIndex >= bufSize) {
        bufIndex = 0;
      }
    }

        // Периодически отправляем данные, если на активной странице с показаниями
        if (millis() - lastUpdateTime > 1000) {  // раз в 1 секунду
          if (currentPage == "page0") {
            sendPage0Data();
          }
          else if (currentPage == "page1") {
            sendPage1Data();
          }
          else if (currentPage == "page2") {
            sendPage2Data();
          }
          lastUpdateTime = millis();
        }

    vTaskDelay(15 / portTICK_PERIOD_MS);
  }
}

void taskCO2Read(void *pvParameters) {
  byte cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte response[9];
  const int MAX_ERRORS = 3;
  int errorCount = 0;
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
        ESP_LOGV("SENSORS", "CO2: %d ppm\n", ppm);
        errorCount = 0; // Сброс ошибок при успехе
      } else {
        errorCount++;
        ESP_LOGE("SENSORS", "Read error from MH-Z1911A! Counter: %d/%d\n", errorCount, MAX_ERRORS);
      }
    } else {
      errorCount++;
      ESP_LOGE("SENSORS", "MH-Z1911A CRC Error! Counter: %d/%d\n", errorCount, MAX_ERRORS);
    }

    // Проверка на превышение ошибок
    if (errorCount >= MAX_ERRORS) {
      ESP_LOGE("SYS", "The sensor MH-Z1911A is not connected. Deleting taskCO2Read");
      mh19.end();
      taskCO2ReadHandle = NULL;
      CO2ReadRunning = false;
      sendTaskStateUpdate();
      vTaskDelete(NULL);
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void taskTVOCRead(void *pvParameters)
{
    unsigned long lastCompensationTime = millis();
    float rH, tempAHT;

    const int MAX_ERRORS = 3;
    int ens160ErrorCount = 0;
    int aht21ErrorCount = 0;

    while (true)
    {
      checkMutex();
        // Получаем доступ к I²C
        if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
        {
            // Проверка доступности ENS160
            if (ens160.begin())
            {
                //ens160.setOperatingMode(SFE_ENS160_STANDARD);
                //vTaskDelay(50 / portTICK_PERIOD_MS);
                if (ens160.checkDataStatus()) 
                {
                  AQI = ens160.getAQI();
                  TVOC = ens160.getTVOC();
                  ECO2 = ens160.getECO2();
                  ESP_LOGV("SENSORS", "AQI: %d\tTVOC: %d ppb\tCO2: %d ppm\n", AQI, TVOC, ECO2);
                  switch (ens160.getFlags()) {
                    case 0:
                      ESP_LOGI("INIT", "Operating ok: Standard Operation");
                      break;
                    case 1:
                      ESP_LOGW("INIT", "Warm-up: occurs for 3 minutes after power-on.");
                      break;
                    case 2:
                      ESP_LOGW("INIT", "Initial Start-up: Occurs for the first hour of operation and only once in sensor's lifetime.");
                      break;
                    case 3:
                      ESP_LOGW("INIT", "No valid data available.");
                      break;
                    default:
                      ESP_LOGE("INIT", "Unknown status.");
                      break;
                  }
                }
                else  {
                  ESP_LOGW("SENSORS", "No new data from ENS160");
                  switch (ens160.getFlags()) {
                    case 0:
                      ESP_LOGI("INIT", "Operating ok: Standard Operation");
                      break;
                    case 1:
                      ESP_LOGW("INIT", "Warm-up: occurs for 3 minutes after power-on.");
                      break;
                    case 2:
                      ESP_LOGW("INIT", "Initial Start-up: Occurs for the first hour of operation and only once in sensor's lifetime.");
                      break;
                    case 3:
                      ESP_LOGW("INIT", "No valid data available.");
                      break;
                    default:
                      ESP_LOGE("INIT", "Unknown status.");
                      break;
                  }
                }
                //ens160.setOperatingMode(SFE_ENS160_IDLE);
              ens160ErrorCount = 0;
            }
            else
            {
                ens160ErrorCount++;
                ESP_LOGE("SENSORS", "Error ENS160! Attempt %d of %d\n", ens160ErrorCount, MAX_ERRORS);
            }

            // Проверка доступности AHT20
            if (aht20.begin())
            {
                tempAHT = aht20.getTemperature();
                rH = aht20.getHumidity();
                aht21ErrorCount = 0;
            }
            else
            {
                aht21ErrorCount++;
                ESP_LOGE("SENSORS", "Error AHT20! Attempt %d of %d\n", aht21ErrorCount, MAX_ERRORS);
            }

            xSemaphoreGive(i2cMutex);
        }
        else
        {
            ESP_LOGE("MUTEX", "Failed to acquire i2cMutex! From task: %s | Mutex holder: %s", 
              pcTaskGetTaskName(NULL), 
              xSemaphoreGetMutexHolder(i2cMutex) ? pcTaskGetTaskName(xSemaphoreGetMutexHolder(i2cMutex)) : "None");
            resetI2CBus();
        }

        // Удаление задачи при превышении ошибок
        if (ens160ErrorCount >= MAX_ERRORS || aht21ErrorCount >= MAX_ERRORS)
        {
            ESP_LOGE("SYS", "Sensors ENS160 and AHT21 not responding. Deleting taskTVOCRead...");
            xSemaphoreGive(i2cMutex);
            taskTVOCReadHandle = NULL;
            TVOCReadRunning = false;
            sendTaskStateUpdate();
            vTaskDelete(NULL);
        }

        // Компенсация раз в 2 минуты
        if ((millis() - lastCompensationTime) >= 120000)
        {
          checkMutex();
            if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
            {
                ens160.setTempCompensationCelsius(tempAHT);
                ens160.setRHCompensationFloat(rH);
                ESP_LOGI("SENSORS", "Setting temperature and humidity calibration values:\nTemperature: %.2f °C, Humidity: %.2f %%\n", tempAHT, rH);
                xSemaphoreGive(i2cMutex);
                lastCompensationTime = millis();
                vTaskDelay(5000 / portTICK_PERIOD_MS);
            }
            else
            {
                ESP_LOGE("MUTEX", "Failed to acquire i2cMutex! From task: %s | Mutex holder: %s", 
                  pcTaskGetTaskName(NULL), 
                  xSemaphoreGetMutexHolder(i2cMutex) ? pcTaskGetTaskName(xSemaphoreGetMutexHolder(i2cMutex)) : "None");
                resetI2CBus();
            }
        }

        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

void reconnectWiFi() {
  uint32_t currentCooldown = SHORT_COOLDOWN;
  // Если предыдущая попытка была последней в цикле (3-я, 6-я, 9-я), установить длительный перерыв
  if (wifi_attempts > 0 && (wifi_attempts % MAX_ATTEMPTS_PER_CYCLE) == 0) {
    currentCooldown = LONG_COOLDOWN;
  }
  
  // Если не прошло нужное время, выходим
  if(last_reconnect_time != 0 && (millis() - last_reconnect_time < currentCooldown)) {
    return;
  }
  
  wifi_attempts++;
  last_reconnect_time = millis();
  
  ESP_LOGW("WIFI", "Reconnect attempt %d/%d", wifi_attempts, MAX_ATTEMPTS_PER_CYCLE * MAX_CYCLES);
  
  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.config(
    IPAddress(192,168,1,230),  // static IP (опционально)
    IPAddress(192,168,1,254),     // gateway
    IPAddress(255,255,255,0),   // subnet
    IPAddress(192,168,1,254)          // DNS (опционально)
  );
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  uint32_t start = millis();
  // Ждём до 20 сек для подключения
  while(WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    // Сброс попыток при успешном подключении
    wifi_attempts = 0;
    ESP_LOGI("WIFI", "Successfully reconnected! RSSI: %d", WiFi.RSSI());
  }
}

void wifi_timer_callback(TimerHandle_t xTimer) {
  // Если ещё не подключены
  if(WiFi.status() != WL_CONNECTED) {
    if(wifi_attempts < MAX_ATTEMPTS_PER_CYCLE * MAX_CYCLES) {
      reconnectWiFi();
      
      // Перезапускаем таймер с нужным интервалом
      uint32_t nextInterval = SHORT_COOLDOWN;
      if (wifi_attempts > 0 && (wifi_attempts % MAX_ATTEMPTS_PER_CYCLE) == 0) {
        nextInterval = LONG_COOLDOWN;
      }
      xTimerChangePeriod(wifiTimer, pdMS_TO_TICKS(nextInterval), 0);
      xTimerStart(wifiTimer, 0);
    } else {
      ESP_LOGE("WIFI", "Critical WiFi failure after %d attempts! Rebooting...", wifi_attempts);
      ESP.restart();
    }
  }
}

void wifi_monitor_task(void* pvParams) {
  // Create timer with short interval (30 seconds)
  wifiTimer = xTimerCreate("WiFiTimer", pdMS_TO_TICKS(SHORT_COOLDOWN), pdFALSE, 0, wifi_timer_callback);
  
  if (wifiTimer == NULL) {
    ESP_LOGE("WIFI", "Failed to create WiFi timer!");
    vTaskDelete(NULL); // Delete task if timer creation fails
    return;
  }

  // Subscribe to WiFi events
  WiFiEventId_t eventHandler = WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch(event) {
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        ESP_LOGW("WIFI", "Disconnected!");
        last_reconnect_time = 0;
        
        // Critical section for thread-safe timer operation
        UBaseType_t uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        
        if(!xTimerIsTimerActive(wifiTimer)) {
          xTimerStartFromISR(wifiTimer, &xHigherPriorityTaskWoken);
        }
        
        taskEXIT_CRITICAL_FROM_ISR(uxSavedInterruptStatus);
        if(xHigherPriorityTaskWoken) {
          portYIELD_FROM_ISR();
        }
        break;
      }
      case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTimerStopFromISR(wifiTimer, &xHigherPriorityTaskWoken);
        
        wifi_attempts = 0;  // reset on IP acquisition
        ESP_LOGI("WIFI", "Obtained IP: %s", WiFi.localIP().toString().c_str());
        
        if(xHigherPriorityTaskWoken) {
          portYIELD_FROM_ISR();
        }
        break;
      }
      default:
        break;
    }
  });

  if (!eventHandler) {
    ESP_LOGE("WIFI", "Failed to register WiFi event handler!");
    xTimerDelete(wifiTimer, 0); // Clean up timer
    vTaskDelete(NULL); // Delete task
    return;
  }

  // Основной цикл мониторинга
  while(1) {
    if(WiFi.status() != WL_CONNECTED && !xTimerIsTimerActive(wifiTimer)) {
      // Если таймер не активен, запустим его
      xTimerStart(wifiTimer, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

// void memory_task(void* pvParams) {
//   while(1) {
//     ESP_LOGD("MEM", "Free: %6d | Min Free Block: %6d | Frag: %.2f%%\n", 
//                   ESP.getFreeHeap(), 
//                   heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
//                   (100.0f - (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) * 100.0f) / ESP.getFreeHeap()));
//     vTaskDelay(pdMS_TO_TICKS(30000)); // Каждые 30 сек
//   }
// }


// ----------------------------- Setup -----------------------------
void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  esp_log_level_set("*", ESP_LOG_VERBOSE);
  ESP_LOGI("TEST", "ESP_LOG is working!");
  nextion.begin(115200, SERIAL_8N1, RX2, TX2); // Инициализация Serial2  
  ESP_LOGI("INIT", "ESP32 + Nextion Initialized");


  // Настройка пина для управления PWR_UP nRF905
  pinMode(NRF905_PWR_UP_PIN, OUTPUT);
  digitalWrite(NRF905_PWR_UP_PIN, HIGH);  // В нормальном режиме модуль должен получать питание (HIGH)
  ESP_LOGI("INIT", "Powering on nRF905...");

  SPI.begin(NRF905_SPI_SCK, NRF905_SPI_MISO, NRF_SPI_MOSI);
  ESP_LOGI("INIT", "SPI Initialized");


  // Инициализация файловой системы
  setupFileSystem();

  mh19.begin(9600, SERIAL_8N1, RX1, TX1);

  //Режим клиента

  WiFi.mode(WIFI_STA);

  // Подключение к Wi-Fi
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.config(
    IPAddress(192,168,1,230),  // static IP (опционально)
    IPAddress(192,168,1,254),     // gateway
    IPAddress(255,255,255,0),   // subnet
    IPAddress(192,168,1,254)          // DNS (опционально)
  );

  if (WiFi.status() != WL_CONNECTED)
  {
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI("WIFI", "Power saving mode: OFF");
    delay(1000);
    ESP_LOGI("WIFI", "Connecting to Wi-Fi...");
  }
  ESP_LOGI("WIFI", "Wi-Fi connected!");
  ESP_LOGI("WIFI", "Local IP: %s", WiFi.localIP().toString().c_str());

  // Настройка сервера
  server.serveStatic("/", LittleFS, "/");
  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request){
    handleRoot(request);
  });
  server.on("/REMOVED", HTTP_POST, [](AsyncWebServerRequest *request){
    handleAdmin(request);
  });
  server.on("/about", HTTP_POST, [](AsyncWebServerRequest *request){
    handleAbout(request);
  });
  server.on("/updateform", HTTP_POST, [](AsyncWebServerRequest *request){
    handleUpdateForm(request);
  });
  server.on("/update", HTTP_POST, handleUpdateEnd, handleUpdateUpload);
  server.onFileUpload(handleUpdateUpload);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/graph-data", HTTP_GET, [](AsyncWebServerRequest *request){
    handleGraphData(request);
  });
  server.on("/getTasksState", HTTP_GET, [](AsyncWebServerRequest *request){
    handleGetTasksState(request);
  });
  server.on("/toggleTask", HTTP_ANY, handleTaskControl);
  server.on("/sysinfo", HTTP_GET, handleSysInfo);
  server.on("/bmeinfo", HTTP_GET, handleBMEInfo);
  server.on("/nrf905Status", HTTP_GET, handlenRFInfo);
  server.on("/setNRF905", HTTP_ANY, handleSetNRF905);
  server.on("/nrfreset", HTTP_POST, handleNRFReset);  
  server.begin();
 
  server.addHandler(&webSocket);
  server.addHandler(&webSocket1);
  webSocket.onEvent(onWsEvent);
  webSocket1.onEvent(onWsEvent1);

  webServerRunning = true;
  //Serial.setDebugOutput(true);

  nextionRestart();

  // Инициализация радиомодуля
  if (!driver.init())
  {
    ESP_LOGE("INIT", "Failed initialize nRF905!");
  } else {
    ESP_LOGI("INIT", "nRF905 initialized");
  }

  // Настройка канала и диапазона
  driver.setChannel(175, false); // Канал 175 = 439.9 МГц
  driver.setRF(RH_NRF905::TransmitPower10dBm);
  ESP_LOGI("INIT", "nRF905 configured and ready!");


  // Инициализация датчиков i2c
  if (!bme.begin(0x76)) {
      ESP_LOGE("INIT", "BME280 not detected. Please check wiring!");
    } else {
      ESP_LOGI("INIT", "BME280 detected");
  }

  // if (!mag.begin()) {
  //     ESP_LOGE("INIT", "HMC5883L not detected. Please check wiring!");
  //   } else {
  //     ESP_LOGI("INIT", "HMC5883L detected. Starting GeomagneticTask");
  //     xTaskCreatePinnedToCore(taskGeomagnetic, "GeomagneticTask", 2048, NULL, 2, NULL, 1);
  // }

  if (!ens160.begin()) {
      ESP_LOGE("INIT", "ENS160 not detected. Please check wiring!");
    } else {
      ens160.setOperatingMode(SFE_ENS160_RESET);
      delay(100);
      ens160.setOperatingMode(SFE_ENS160_STANDARD);
      ESP_LOGI("INIT", "ENS160 detected. Standart mode set");
      ensStatus = ens160.getFlags();      
      switch (ensStatus) {
        case 0:
          ESP_LOGI("INIT", "Operating ok: Standard Operation");
          break;
        case 1:
          ESP_LOGI("INIT", "Warm-up: occurs for 3 minutes after power-on.");
          break;
        case 2:
          ESP_LOGI("INIT", "Initial Start-up: Occurs for the first hour of operation and only once in sensor's lifetime.");
          break;
        case 3:
          ESP_LOGI("INIT", "No valid data available.");
          break;
        default:
          ESP_LOGI("INIT", "Unknown status.");
          break;
      }
  }

  if (!aht20.begin()) {
    ESP_LOGE("INIT", "AHT21 not detected. Please check wiring!");
  } else {
    ESP_LOGI("INIT", "AHT21 detected");
  }

  if (ens160.isConnected()) {
    ESP_LOGI("INIT", "ENS160 connected.");
    ESP_LOGI("INIT", "App Ver: %u", ens160.getAppVer());
} else {
    ESP_LOGE("INIT", "ENS160 failed!");
}


  // Настройка времени через NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Установка высоты для Forecaster
  cond.begin();
  cond.setH(61);

  // Создание мьютексов
  i2cMutex = xSemaphoreCreateMutex();
    if (i2cMutex == NULL) {
    ESP_LOGE("MUTEX", "Failed to create mutex for i2c!");
  }

  driverMutex = xSemaphoreCreateMutex();
    if (driverMutex == NULL) {
    ESP_LOGE("MUTEX", "Failed to create mutex for nRF905!");
  }

  // Создание задач FreeRTOS

  xTaskCreate(taskNRF905, "NRF905 Receiver", 4096, NULL, 5, &taskNRF905Handle);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 4, &taskBMP280Handle);
  //xTaskCreatePinnedToCore(taskGeomagnetic, "GeomagneticTask", 2048, NULL, 2, NULL, 1);
  xTaskCreate(taskCO2Read, "CO2 read task", 2048, NULL, 3, &taskCO2ReadHandle);
  xTaskCreate(taskTVOCRead, "ENS160 read task", 4096, NULL, 2, &taskTVOCReadHandle);
  xTaskCreate(taskGetTime, "Get NTP Time", 4096, NULL, 2, &taskGetTimeHandle);
  xTaskCreate(taskSendDataToInfluxDB, "InfluxDBTask", 4096, NULL, 6, &taskSendDataToInfluxDBHandle);
  xTaskCreate(taskForecast, "Forecast task", 2048, NULL, 1, &taskForecasterHandle);
  xTaskCreate(processNextionTask, "Nextion", 4096, NULL, 3, &processNextionTaskHandle);
  //xTaskCreate(memory_task, "Memory Monitor", 4096, NULL, 1, NULL);
  xTaskCreatePinnedToCore(wifi_monitor_task, "WiFiMonitor", 2048, NULL, 2, NULL, 0);
}

// ----------------------------- Main loop -----------------------------
void loop()
{
  vTaskDelete(NULL);
}