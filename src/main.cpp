#include <Arduino.h>
#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
#include <SparkFun_ENS160.h>
#include <SparkFun_Qwiic_Humidity_AHT20.h>
#include <WiFi.h>
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




// ----------------------------- Wi-Fi и сервер -----------------------------
const char *ssid = "Lucky Devil";
const char *password = "evgen850517";
const char* http_username = "evgen";  // Логин для доступа
const char* http_password = "Sergeevich850517!";   // Пароль для доступа

// Настройка NTP-сервера
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600 * 10; // Смещение для Komsomolsk-on-Amur (GMT+10)
const int   daylightOffset_sec = 0;

// Параметры InfluxDB
const char* influxDBHost = "192.168.1.214";
const int influxDBPort = 8086;
const char* influxDBDatabase = "weather_data";

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
    // Serial.println("Failed to initialize LittleFS");
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


void handleGetTasksState(AsyncWebServerRequest *request) {
  String stateJson = "{";
  stateJson += "\"webServer\":" + String(isTaskActive(taskWebServerHandle) ? "true" : "false") + ",";
  stateJson += "\"nRF905\":" + String(isTaskActive(taskNRF905Handle) ? "true" : "false") + ",";
  stateJson += "\"CO2\":" + String(isTaskActive(taskCO2ReadHandle) ? "true" : "false") + ",";
  stateJson += "\"nextion\":" + String(isTaskActive(processNextionTaskHandle) ? "true" : "false") + ",";
  stateJson += "\"BMP280\":" + String(isTaskActive(taskBMP280Handle) ? "true" : "false") + ",";
  stateJson += "\"InfluxDB\":" + String(isTaskActive(taskSendDataToInfluxDBHandle) ? "true" : "false") + ",";
  stateJson += "\"Forecaster\":" + String(isTaskActive(taskForecasterHandle) ? "true" : "false") + ",";
  stateJson += "\"NTP\":" + String(isTaskActive(taskGetTimeHandle) ? "true" : "false") + ",";
  stateJson += "\"TVOC\":" + String(isTaskActive(taskTVOCReadHandle) ? "true" : "false");
  stateJson += "}";

  request->send(200, "application/json", stateJson);
}

void sendTaskStateUpdate() {
  String stateJson = "{";
  stateJson += "\"webServer\":" + String(isTaskActive(taskWebServerHandle) ? "true" : "false") + ",";
  stateJson += "\"nRF905\":" + String(isTaskActive(taskSendDataToInfluxDBHandle) ? "true" : "false") + ",";
  stateJson += "\"CO2\":" + String(isTaskActive(taskCO2ReadHandle) ? "true" : "false") + ",";
  stateJson += "\"nextion\":" + String(isTaskActive(processNextionTaskHandle) ? "true" : "false") + ",";
  stateJson += "\"BMP280\":" + String(isTaskActive(taskBMP280Handle) ? "true" : "false") + ",";
  stateJson += "\"InfluxDB\":" + String(isTaskActive(taskSendDataToInfluxDBHandle) ? "true" : "false") + ",";
  stateJson += "\"Forecaster\":" + String(isTaskActive(taskForecasterHandle) ? "true" : "false") + ",";
  stateJson += "\"NTP\":" + String(isTaskActive(taskGetTimeHandle) ? "true" : "false") + ",";
  stateJson += "\"TVOC\":" + String(isTaskActive(taskTVOCReadHandle) ? "true" : "false");
  stateJson += "}";

  webSocket.textAll(stateJson); // Асинхронная отправка данных всем клиентам
}

void sendTimeData() {
  if (webSocket1.count() > 0) {  // Проверяем, есть ли клиенты
      StaticJsonDocument<256> json;
      json["nowTime"] = time(nullptr);  // Функция получения текущего времени
      json["sunriseTime"] = sunriseTime * 60; // Перевод из минут в секунды
      json["sunsetTime"] = sunsetTime * 60;

      String jsonString;
      serializeJson(json, jsonString);
      webSocket1.textAll(jsonString);
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
      //Serial.println("Authentication failed");
      ESP_LOGW("WEB", "Authentication failed");
      request->send(401, "text/plain", "Unauthorized");
      return;
  }

  if (index == 0) {
      //Serial.printf("Update started: %s\n", filename.c_str());
      ESP_LOGW("UPDATE", "Update started: %s\n", filename.c_str());

      if (filename.indexOf("littlefs") >= 0) {
          //Serial.println("Updating filesystem");
          ESP_LOGW("UPDATE", "Updating filesystem");
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
              Update.printError(Serial);
              request->send(500, "text/plain", "Failed to start filesystem update");
              return;
          }
      } else {
          Serial.println("Updating firmware");
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
          //Serial.printf("Update completed: %u bytes\n", index + len);
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
      Serial.println("Update successful, restarting...");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      ESP.restart();
  } else {
      Serial.println("Update failed");
  }
}


void handleAdmin(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(LittleFS, "/admin.html", "text/html");
}

void handleAbout(AsyncWebServerRequest *request) {
  Serial.println("Attempting to load /about.html");
  if (LittleFS.exists("/about.html")) {
      Serial.println("/about.html found, sending...");
      request->send(LittleFS, "/about.html", "text/html");
  } else {
      Serial.println("/about.html not found");
      request->send(404, "text/plain", "about.html not found");
  }
}

void handleRestart(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
    request->send(200, "text/plain", "ESP32 is restarting...");
    nextionRestart();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
}

void handleRestartFromNextion() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  ESP.restart();
}

void resetNRF905() {
  Serial.println("Performing nRF905 reset...");
  digitalWrite(NRF905_PWR_UP_PIN, LOW);  // Выключаем питание (пин LOW)
  vTaskDelay(200 / portTICK_PERIOD_MS);                           // Задержка (100 мс – можно настроить по datasheet)
  digitalWrite(NRF905_PWR_UP_PIN, HIGH); // Включаем питание (пин HIGH)
  Serial.println("nRF905 reset complete.");
  vTaskDelay(100 / portTICK_PERIOD_MS);
   // Переинициализация и настройка модуля
  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    if (driver.init()) {
        Serial.println("nRF905 reinitialized successfully.");
        driver.setChannel(175, false); // Канал 175 = 439.9 МГц
        driver.setRF(RH_NRF905::TransmitPowerm2dBm);
        // Другие необходимые настройки...
      } else {
        Serial.println("Failed to reinitialize nRF905.");
      }
    xSemaphoreGive(driverMutex);
    }
}

void resetI2CBus()
{
  i2cResetCount++;
  Serial.println("Resetting I2C bus...");
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

  ens160.setOperatingMode(SFE_ENS160_RESET);

  if (!bme.begin(0x76))
  {
    Serial.println("Could not find a valid BME280 sensor, check wiring!");
  }
  if (!ens160.begin())
  {
    Serial.println("Could not find a valid ENS160 sensor, check wiring!");
  }
  if (!aht20.begin())
  {
    Serial.println("Could not find a valid AHT20 sensor, check wiring!");
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
  } else {
    snprintf(buffer, len, "Failed to acquire i2cMutex! From task - %s\n", pcTaskGetTaskName(NULL));
    Serial.print("Failed to acquire i2cMutex! From task - ");
    Serial.println(pcTaskGetTaskName(NULL));
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

      Serial.printf("Settings received: channel = %d, band = %s, power = %s\n",
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

  // Формируем строку InfluxDB Line Protocol
  String influxDBLine = "weather,location=home ";
    if (temperature != 0.0f)
    {
      influxDBLine += "temperature=" + String(temperature, 2) + ",";
    }

    if (humidity != 0.0f)
    {
      influxDBLine += "humidity=" + String(humidity, 2) + ",";
    }

    if (dewPoint != 0.0f)
    {
      influxDBLine += "dewPoint=" + String(dewPoint, 2) + ",";
    }

    if (pressure != 0.0f)
    {
      influxDBLine += "pressure=" + String(pressure, 2) + ",";
    }
    
    if (forecast >= 0)
    {
      influxDBLine += "forecast=" + String(forecast, 2) + ",";
    }

    if (trend >= -30 && trend <= 30) 
    {
      influxDBLine += "trend=" + String(trend, 2) + ",";
    }
    else influxDBLine += "trend=" + String(0, 2) + ",";

    if (homeTemp != 0.0f)
    {
      influxDBLine += "homeTemp=" + String(homeTemp, 2) + ",";
    }

    if (homeHum != 0.0f)
    {
      influxDBLine += "homeHum=" + String(homeHum, 2) + ",";
    }

    if (homeDP != 0.0f)
    {
      influxDBLine += "homeDP=" + String(homeDP, 2) + ",";
    }

    if (ppm != 0)
    {
      influxDBLine += "CO2=" + String(ppm) + ",";
    }

    if (TVOC != -1)
    {
      influxDBLine += "TVOC=" + String(TVOC) + ",";
    }

    if (AQI != -1)
    {
      influxDBLine += "AQI=" + String(AQI) + ",";
    }

    if (ECO2 != -1)
    {
      influxDBLine += "ECO2=" + String(ECO2);
    }
  String url = "http://" + String(influxDBHost) + ":" + String(influxDBPort) + "/write?db=" + String(influxDBDatabase);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int httpResponseCode = http.POST(influxDBLine);

  if (httpResponseCode > 0)
  {
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    if (httpResponseCode != 204)
    {
      Serial.println(http.getString()); // Выводим ответ сервера для отладки
    }
  }
  else
  {
    Serial.print("Error sending data to InfluxDB: ");
    Serial.println(http.errorToString(httpResponseCode));
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
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
          String msg = (char *)data;
          Serial.printf("Received message: %s\n", msg.c_str());
      }
  }
}

void onWsEvent1(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    Serial.println("Client connected to /ws1");
  }
  else if (type == WS_EVT_DATA)
  {
    String message = (char *)data;
    if (message == "getTime")
    {
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
    Serial.println("Отправка в базу данных: остановлена");
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
      Serial.println("Отправка в базу данных: created and running");
    } else {
      vTaskResume(taskSendDataToInfluxDBHandle);
      Serial.println("Отправка в базу данных: возобновлёна");
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
    Serial.println("Чтение с датчика CO2: остановлено");
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
      Serial.println("Чтение с датчика CO2: created and running");
    } else {
      vTaskResume(taskCO2ReadHandle);
      Serial.println("Чтение с датчика CO2: возобновлёно");
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
    Serial.println("Прием с nRF905: остановлен");
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
      Serial.println("Прием с nRF905: created and running");
    } else {
      vTaskResume(taskNRF905Handle);
      Serial.println("Прием с nRF905: возобновлён");
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
    Serial.println("Обновление дисплея: остановлено");
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
      Serial.println("Обновление дисплея: created and running");
    } else {
      vTaskResume(processNextionTaskHandle);
      nextionWakeUP();
      Serial.println("Обновление дисплея: возобновлёно");
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
    Serial.println("Чтение с BME280: остановлено");
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
      Serial.println("Чтение с BME280: created and running");
    } else {
      vTaskResume(taskBMP280Handle);
      Serial.println("Чтение с BME280: возобновлёно");
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
    Serial.println("Forecast: stopped");
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
      Serial.println("Forecast: created and running");
    } else {
      vTaskResume(taskForecasterHandle);
      Serial.println("Forecast: resumed");
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
    Serial.println("NTP: stopped");
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
      Serial.println("NTP: created and running");
    } else {
      vTaskResume(taskGetTimeHandle);
      Serial.println("NTP: resumed");
    }
    getTimeRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
}

void switchTaskTVOCRead() {
  if (isTaskActive(taskTVOCReadHandle)) {
    // Если задача активна - останавливаем
    vTaskSuspend(taskTVOCReadHandle);
    TVOCReadRunning = false;
    Serial.println("TVOC reading: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskTVOCReadHandle == NULL) {
      if (!ens160.begin()) {
        Serial.println("Air Quality Sensor did not begin.");
      } else {
        Serial.println("ENS160 detected");
  
      }
  
      if (!aht20.begin()) {
        Serial.println("AHT21 not detected. Please check wiring!.");
      } else {
        Serial.println("AHT21 detected");
    }
      xTaskCreate(
        taskTVOCRead,
        "ENS160 read task", 
        4096,
        NULL,
        2,
        &taskTVOCReadHandle
      );
      Serial.println("TVOC reading: created and running");
    } else {
      vTaskResume(taskTVOCReadHandle);
      Serial.println("TVOC reading: resumed");
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
  Serial.printf("Получен запрос на переключение задачи: %s\n", task.c_str());

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
        uint8_t buf[RH_NRF905_MAX_MESSAGE_LEN];
        uint8_t len = sizeof(buf);
        if (driver.recv(buf, &len))
        {
          buf[len] = '\0'; // Завершаем строку

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
      Serial.println("No data available for more than 10 minutes. Resetting nRF905...");
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
    if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
    {
      pressure = bme.readPressure() / 100.0f; // Получаем давление
      homeTemp = bme.readTemperature();
      homeHum = bme.readHumidity();
      xSemaphoreGive(i2cMutex);
      homeDP = calculatehomeDP(homeTemp, homeHum);
    }
    else
    {
      // Ошибка: превышено время ожидания мьютекса
      Serial.println("Failed to acquire i2cMutex! From task - ");
      Serial.println(pcTaskGetTaskName(NULL));
      resetI2CBus();
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); // Задержка 5 секунд
  }
}

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
      Serial.println("Failed to get time via NTP.");
    }

    vTaskDelay(pdMS_TO_TICKS(60000)); // Проверка времени раз в минуту
  }

  configASSERT(NULL); // Если задача выйдет из цикла, это вызовет ошибку
}

void taskForecast(void *pvParameters) {
  for (;;) {
    int month = getMonth();
    if (month != -1) {
      cond.setMonth(month);  // Устанавливаем текущий месяц в Forecaster
    }
    int p = pressure * 100;
    if (p !=0 && !isnan(p)) {
    
    cond.addP(p, temperature);
    } else  {
      vTaskDelay((5000) / portTICK_PERIOD_MS);
    }
    //Serial.print("Parameters sent to forecast: ");
    //Serial.println(p);
    ESP_LOGD("FORECAST", "Parameters sent to forecast: %i Pa", p);
    forecast = cond.getCast();
    trend = cond.getTrend() / 100.0f;
    // Serial.print("Barometric tendence: ");
    // Serial.print(trend);
    // Serial.println(" hPa");
    ESP_LOGD("FORECAST", "Barometric trend: %f hPa", trend);

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
    Serial.print("Page changed: ");
    Serial.println(currentPage);
  }
  else if (msg[0] == 0x65) {
    // Событие от компонента
    // Формат: 65, <PageID>, <ComponentID>, <EventValue>, FF, FF, FF
    uint8_t compID = msg[2];
    if (compID == 0x03) {
      Serial.println("Button pressed: b2");
      handleRestartFromNextion();
    }
    else if (compID == 0x04) {
      Serial.println("Button pressed: b3");
      resetNRF905();
    }
    else if (compID == 0x05)  {
      Serial.println("Button pressed: b4");
      ESP.restart();
    }
    else if (compID == 0x07) {
      Serial.println("Button pressed: bt1");
      // Переключаем веб-сервер
      switchTaskNRF905();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(1, taskNRF905Handle);
    }
    else if (compID == 0x08) {
      Serial.println("Button pressed: bt2");
      // Переключаем веб-сервер
      switchTaskCO2Read();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(2, taskCO2ReadHandle);
    }
    else if (compID == 0x09) {
      Serial.println("Button pressed: bt3");
      // Переключаем веб-сервер
      switchTaskNextion();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(3, processNextionTaskHandle);
    }
    else if (compID == 0x0A) {
      Serial.println("Button pressed: bt4");
      // Переключаем веб-сервер
      switchTaskBMP280();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(4, taskBMP280Handle);
    }
    else if (compID == 0x0B) {
      Serial.println("Button pressed: bt5");
      // Переключаем веб-сервер
      switchTaskInfluxDB();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(5, taskSendDataToInfluxDBHandle);
    }
    else if (compID == 0x0C) {
      Serial.println("Button pressed: bt6");
      // Переключаем веб-сервер
      switchTaskForecaster();
      // Синхронизируем состояние dual-state кнопки
      syncButtonState(6, taskForecasterHandle);
    }
    else if (compID == 0x0D) {
      Serial.println("Button pressed: bt7");
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
        //Serial.printf("CO2: %d ppm\n", ppm);
        errorCount = 0; // Сброс ошибок при успехе
      } else {
        errorCount++;
        Serial.printf("MH-Z1911A CRC Error! Counter: %d/%d\n", errorCount, MAX_ERRORS);
      }
    } else {
      errorCount++;
      Serial.printf("Read error from MH-Z1911A! Counter: %d/%d\n", errorCount, MAX_ERRORS);
    }

    // Проверка на превышение ошибок
    if (errorCount >= MAX_ERRORS) {
      Serial.print("The sensor MH-Z1911A is not connected");
      Serial.println("Deleting the reading task from the sensor MH-Z1911A...");
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
        // Получаем доступ к I²C
        if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
        {
            // Проверка доступности ENS160
            if (ens160.begin())
            {
                ens160.setOperatingMode(SFE_ENS160_STANDARD);
                AQI = ens160.getAQI();
                TVOC = ens160.getTVOC();
                ECO2 = ens160.getECO2();
                //Serial.printf("AQI: %d\tTVOC: %d ppb\tCO2: %d ppm\n", AQI, TVOC, ECO2);
                ens160ErrorCount = 0;
            }
            else
            {
                ens160ErrorCount++;
                Serial.printf("Error ENS160! Attempt %d of %d\n", ens160ErrorCount, MAX_ERRORS);
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
                Serial.printf("Error AHT20! Attempt %d of %d\n", aht21ErrorCount, MAX_ERRORS);
            }

            xSemaphoreGive(i2cMutex);
        }
        else
        {
            Serial.println("Failed to acquire i2cMutex! From task - ");
            Serial.println(pcTaskGetTaskName(NULL));
            resetI2CBus();
        }

        // Удаление задачи при превышении ошибок
        if (ens160ErrorCount >= MAX_ERRORS || aht21ErrorCount >= MAX_ERRORS)
        {
            Serial.println("Sensors not responding. Deleting taskTVOCRead...");
            xSemaphoreGive(i2cMutex);
            taskTVOCReadHandle = NULL;
            TVOCReadRunning = false;
            sendTaskStateUpdate();
            vTaskDelete(NULL);
        }

        // Компенсация раз в минуту
        if ((millis() - lastCompensationTime) >= 60000)
        {
            if (xSemaphoreTake(i2cMutex, 1000 / portTICK_PERIOD_MS) == pdTRUE)
            {
                ens160.setTempCompensationCelsius(tempAHT);
                ens160.setRHCompensationFloat(rH);
                //Serial.printf("Setting temperature and humidity calibration values:\nTemperature: %.2f °C, Humidity: %.2f %%\n", tempAHT, rH);
                xSemaphoreGive(i2cMutex);
                lastCompensationTime = millis();
                vTaskDelay(2000 / portTICK_PERIOD_MS);
            }
            else
            {
                Serial.println("Failed to acquire i2cMutex! From task - ");
                Serial.println(pcTaskGetTaskName(NULL));
                resetI2CBus();
            }
        }

        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}



// ----------------------------- Setup -----------------------------
void setup()
{
  Serial.begin(115200);
  nextion.begin(115200, SERIAL_8N1, RX2, TX2); // Инициализация Serial2  
  //Serial.println("ESP32 + Nextion Initialized");
  ESP_LOGI("INIT", "ESP32 + Nextion Initialized");


  // Настройка пина для управления PWR_UP nRF905
  pinMode(NRF905_PWR_UP_PIN, OUTPUT);
  digitalWrite(NRF905_PWR_UP_PIN, HIGH);  // В нормальном режиме модуль должен получать питание (HIGH)
  //Serial.println("Powering on nRF905...");
  ESP_LOGI("INIT", "Powering on nRF905...");

  SPI.begin(NRF905_SPI_SCK, NRF905_SPI_MISO, NRF_SPI_MOSI);
  //Serial.println("SPI Initialized");
  ESP_LOGI("INIT", "SPI Initialized");


  // Инициализация файловой системы
  setupFileSystem();

  mh19.begin(9600, SERIAL_8N1, RX1, TX1);

  //Режим клиента

  WiFi.mode(WIFI_STA);

  // Подключение к Wi-Fi
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    //Serial.println("Connecting to Wi-Fi...");
    ESP_LOGI("INIT", "Connecting to Wi-Fi...");
  }
  //Serial.println("Wi-Fi connected!");
  //Serial.println(WiFi.localIP());
  ESP_LOGI("INIT", "Wi-Fi connected!");
  ESP_LOGI("INIT", "Local IP: " IPSTR, IP2STR(&WiFi.localIP()));

  // Настройка сервера
  server.serveStatic("/", LittleFS, "/");
  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request){
    handleRoot(request);
  });
  server.on("/admin", HTTP_POST, [](AsyncWebServerRequest *request){
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
  Serial.setDebugOutput(false);

  nextionRestart();

  // Инициализация радиомодуля
  if (!driver.init())
  {
    //Serial.println("Failed initialize nRF905!");
    ESP_LOGE("INIT", "Failed initialize nRF905!");
  } else {
    //Serial.println("nRF905 initialized");
    ESP_LOGI("INIT", "nRF905 initialized");
  }

  // Настройка канала и диапазона
  driver.setChannel(175, false); // Канал 175 = 439.9 МГц
  driver.setRF(RH_NRF905::TransmitPower10dBm);
  //Serial.println("nRF905 configured and ready!");
  ESP_LOGI("INIT", "nRF905 configured and ready!");


  // Инициализация датчиков i2c
  if (!bme.begin(0x76)) {
      //Serial.println("BME280 not detected. Please check wiring!");
      ESP_LOGE("INIT", "BME280 not detected. Please check wiring!");
    } else {
      //Serial.println("BME280 detected");
      ESP_LOGI("INIT", "BME280 detected");
  }

  if (!ens160.begin()) {
      //Serial.println("ENS160 not detected. Please check wiring!");
      ESP_LOGE("INIT", "ENS160 not detected. Please check wiring!");
    } else {
      //Serial.println("ENS160 detected");
      ESP_LOGI("INIT", "ENS160 detected");
  }

  if (!aht20.begin()) {
    //Serial.println("AHT21 not detected. Please check wiring!.");
    ESP_LOGE("INIT", "AHT21 not detected. Please check wiring!");
  } else {
    //Serial.println("AHT21 detected");
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
    //Serial.println("Failed to create mutex for i2c!");
    ESP_LOGE("MUTEX", "Failed to create mutex for i2c!");
  }

  driverMutex = xSemaphoreCreateMutex();
    if (driverMutex == NULL) {
    //Serial.println("Failed to create mutex for nRF905!");
    ESP_LOGE("MUTEX", "Failed to create mutex for nRF905!");
  }

  // Создание задач FreeRTOS

  xTaskCreate(taskNRF905, "NRF905 Receiver", 4096, NULL, 5, &taskNRF905Handle);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 4, &taskBMP280Handle);
  xTaskCreate(taskCO2Read, "CO2 read task", 2048, NULL, 3, &taskCO2ReadHandle);
  xTaskCreate(taskTVOCRead, "ENS160 read task", 4096, NULL, 2, &taskTVOCReadHandle);
  xTaskCreate(taskGetTime, "Get NTP Time", 4096, NULL, 2, &taskGetTimeHandle);
  xTaskCreate(taskSendDataToInfluxDB, "InfluxDBTask", 4096, NULL, 6, &taskSendDataToInfluxDBHandle);
  xTaskCreate(taskForecast, "Forecast task", 2048, NULL, 1, &taskForecasterHandle);
  xTaskCreate(processNextionTask, "Nextion", 4096, NULL, 3, &processNextionTaskHandle);
}

// ----------------------------- Main loop -----------------------------
void loop()
{
  // Основной цикл пустой, так как задачи обрабатываются в FreeRTOS
}