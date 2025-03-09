#include <Arduino.h>
#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
#include <Adafruit_AHTX0.h>
#include "ScioSense_ENS160.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "LittleFS.h"
#include <HTTPClient.h>
#include <time.h>
#include <Forecaster.h>
#include <WebSocketsServer.h>




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

// ---------------------- Определение пинов serial2 --------------------------
HardwareSerial nextion(2); // Используем Serial2 для связи с дисплеем
#define RX2 16  // RX пин ESP32
#define TX2 17  // TX пин ESP32


//----------------------- Определение пинов Serial1 --------------------------

HardwareSerial mh19(1); //Serial1 для датчика CO2
#define RX1 32
#define TX1 33


Adafruit_BME280 bme;                                  // Датчик давления BMP280
ScioSense_ENS160      ens160(ENS160_I2CADDR_1);       // Датчик качества воздуха ENS160
Adafruit_AHTX0 aht;                                   // Датчик компенсации T и H AHT21
RH_NRF905 driver(NRF905_CE, NRF905_TX_EN, NRF905_CS); // Радиомодуль nRF905
WebServer server(80);                                 // Веб-сервер на порту 80
WebSocketsServer webSocket(81);                       // WebSocket сервер на порту 81
Forecaster cond;


// -------------------------- Объявления функций (прототипы) --------------------------
void sendGraphData();
void sendCommand();
void syncWebServerButtonState();
void processNextionTask();
void handleGraphData();
void handleRoot();
void nextionRestart();
void taskWebServer(void *pvParameters);
void processNextionTask(void *pvParameters);
void taskForecast(void *pvParameters);
void taskCO2Read(void *pvParameters);
void taskGetTime(void *pvParameters);
void taskNRF905(void *pvParameters);
void taskBMP280(void *pvParameters);
void taskTVOCRead(void *pvParameters);
void taskSendDataToInfluxDB(void *pvParameters);
//void taskSerialPrint(void *pvParameters);
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

// ---------------------------- Обработчики HTTP запросов -----------------------------
void handleGraphData() {
  DynamicJsonDocument doc(256);
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["dewPoint"] = dewPoint;
  doc["pressure"] = pressure;
  doc["homeTemp"] = homeTemp;
  doc["homeHum"] = homeHum;
  doc["dewPoint"] = dewPoint;
  doc["homeDP"] = homeDP;
  doc["forecast"] = forecast;
  doc["trend"] = trend;
  doc["CO2"] = ppm;
  doc["TVOC"] = TVOC;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleGetTasksState() {
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

  server.send(200, "application/json", stateJson);
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

  webSocket.broadcastTXT(stateJson);  // Отправляем обновлённые данные всем клиентам
}


void handleRoot()
{
  if (LittleFS.exists("/index.html"))
  {
    File file = LittleFS.open("/index.html", "r");
    if (file)
    {
      server.streamFile(file, "text/html");
      file.close();
    }
    else
    {
      server.send(500, "text/plain", "Failed to open index.html");
    }
  }
  else
  {
    server.send(404, "text/plain", "index.html not found");
  }
}

void handleUpdateform() {
  if (!server.authenticate(http_username, http_password)) {
        return server.requestAuthentication();
    }
  if (LittleFS.exists("/updateform.html"))
  {
    File file = LittleFS.open("/updateform.html", "r");
    if (file)
    {
      server.streamFile(file, "text/html");
      file.close();
    }
    else
    {
      server.send(500, "text/plain", "Failed to open updateform.html");
    }
  }
  else
  {
    server.send(404, "text/plain", "updateform.html not found");
  }
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update has begun: %s\n", upload.filename.c_str());

    // Простейшая логика: если имя файла содержит "littlefs", обновляем файловую систему,
    // иначе считаем, что это обновление прошивки.
    if (upload.filename.indexOf("littlefs") >= 0) {
      Serial.println("Updating filesystem");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) { // или U_LITTLEFS, если настроено
        Update.printError(Serial);
      }
    } else {
      Serial.println("Updating firmware");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        Update.printError(Serial);
      }
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("Update completed, bytes written: %u\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    Serial.println("The update was interrupted");
  }
}


void handleUpdateEnd() {
  // Отправляем ответ клиенту
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
  
  // Небольшая задержка для корректного завершения отправки данных,
  // затем перезагрузка устройства для применения обновления.
  delay(1000);
  ESP.restart();
}

void handleAdmin() {
    if (!server.authenticate(http_username, http_password)) {
        return server.requestAuthentication();
    }
      if (LittleFS.exists("/admin.html"))
  {
    File file = LittleFS.open("/admin.html", "r");
    if (file)
    {
      server.streamFile(file, "text/html");
      file.close();
    }
    else
    {
      server.send(500, "text/plain", "Failed to open admin.html");
    }
  }
  else
  {
    server.send(404, "text/plain", "admin.html not found");
  }
}

void handleAbout() {
    if (LittleFS.exists("/about.html"))
{
  File file = LittleFS.open("/about.html", "r");
  if (file)
  {
    server.streamFile(file, "text/html");
    file.close();
  }
  else
  {
    server.send(500, "text/plain", "Failed to open about.html");
  }
}
else
{
  server.send(404, "text/plain", "about.html not found");
}
}

void handleRestart() {
    if (!server.authenticate(http_username, http_password)) {
        return server.requestAuthentication();
    }
    server.send(200, "text/plain", "ESP32 is restarting...");
    nextionRestart();
    delay(1000);
    ESP.restart();
}

void handleRestartFromNextion() {
  
  server.send(200, "text/plain", "ESP32 is restarting...");
  delay(1000);
  ESP.restart();
}

void resetNRF905() {
  Serial.println("Performing nRF905 reset...");
  digitalWrite(NRF905_PWR_UP_PIN, LOW);  // Выключаем питание (пин LOW)
  delay(200);                            // Задержка (100 мс – можно настроить по datasheet)
  digitalWrite(NRF905_PWR_UP_PIN, HIGH); // Включаем питание (пин HIGH)
  Serial.println("nRF905 reset complete.");
  delay(100);
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

void handleNRFReset() {
  resetNRF905();
  server.send(200, "text/plain", "nRF905 has been reset.");
}
String getSystemInfo() {
  String info = "";
  
  // Время работы в секундах (uptime)
  unsigned long uptime = millis() / 1000;
  info += "Uptime: " + String(uptime) + " seconds\n";
  
  // WiFi RSSI
  info += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n";

  // IP address
  info += "IP address: " + String(WiFi.localIP().toString()) + " \n";
  
  // Свободная heap-память
  info += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
  
  // Свободный объем стека для текущей задачи
  info += "Current task free stack: " + String(uxTaskGetStackHighWaterMark(NULL)) + " bytes\n";
  
  // Замечание: Измерение температуры процессора на ESP32 стандартными средствами отсутствует,
  // поэтому это поле можно добавить, если реализуете внешнее измерение.
  
  return info;
}

// Функция получения статуса датчика BME280 по I2C
String getBME280Status() {
  String status = "";
  // Пытаемся прочитать с датчика — если не найден, выводим сообщение об ошибке
  if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
    if (!bme.begin(0x76)) {
      status = "BME280: Not Found";
    } else {
      status = "BME280: OK\n";
      // Дополнительно можно вывести текущие показания
      status += "Temp: " + String(bme.readTemperature(), 2) + " °C\n";
      status += "Humidity: " + String(bme.readHumidity(), 2) + " %\n";
      status += "Pressure: " + String(bme.readPressure() / 100.0F, 2) + " hPa\n";
    }
    xSemaphoreGive(i2cMutex);
  }  
  return status;
}

String getNRF905Status() {
    String status = "";
    uint8_t config[10];
    
    // 1. Чтение регистров. Возвращаемое значение — статусный регистр!
    uint8_t status_reg = driver.spiBurstReadRegister(RH_NRF905_REG_W_CONFIG, config, 10);
    
    // 2. Вывод расшифрованного статуса
    status += "Status Register: 0x" + String(status_reg, HEX) + "\n";
    status += "Decoded Status:\n";
    status += (status_reg & 0x20) ? "[DR] Data Ready\n" : "";
    status += (status_reg & 0x80) ? "[AM] Address Match\n" : "";
    status += (status_reg & 0x40) ? "[CRC_ERR] Error\n" : "[CRC_OK]\n";

    // 3. Вывод конфигурации (остается без изменений)
    status += "Channel: " + String(config[0]) + "\n";

    uint8_t band_bit = config[1] & RH_NRF905_CONFIG_1_HFREQ_PLL;  // Определяем, работает ли модуль в диапазоне 868/915 МГц
    float freq = 422.4 + (config[0] / 10.0);                      // Рассчитываем базовую частоту для 433 МГц
    if (band_bit) freq *= 2;                                      // Если band_bit = 1, умножаем на 2 для 868/915 МГц

    status += "Frequency: " + String(freq, 3) + " MHz\n";

    uint8_t pwr = (config[1] & RH_NRF905_CONFIG_1_PA_PWR) >> 2;
    const char* pwr_str[] = {"-10 dBm", "-2 dBm", "+6 dBm", "+10 dBm"};
    status += "TX Power: " + String(pwr_str[pwr]) + "\n";

    status += "RAW Config: ";
    for (int i = 0; i < 10; i++) {
        status += String(config[i], HEX) + " ";
    }
    status += "\n";

    return status;
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

void handleSysInfo() {
  String info = getSystemInfo();
  server.send(200, "text/plain", info);
}

void handleBMEInfo() {
  String status = getBME280Status();
  server.send(200, "text/plain", status);
}

void handlenRFInfo() {
  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    String status = getNRF905Status();
    server.send(200, "text/plain", status);
  xSemaphoreGive(driverMutex);
  }
}

void handleSetNRF905() {
  int channel = server.arg("channel").toInt();
  bool band = (server.arg("band") == "true");
  String powerStr = server.arg("power");
  
  // Преобразование строки в значение TransmitPower
  RH_NRF905::TransmitPower txPower = getTransmitPowerFromString(powerStr);
  
  // Вывод для отладки
  Serial.printf("Settings received: channel = %d, band = %s, power = %s\n",
                channel, (band ? "hiband" : "lowband"), powerStr.c_str());
  
  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    // Применяем настройки через драйвер
    driver.setChannel(channel, band);
    driver.setRF(txPower);
  xSemaphoreGive(driverMutex);
  }


  // Отправляем ответ клиенту
  server.send(200, "text/plain", "Settings nRF905 applied");
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

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("Client %u connected\n", num);
      break;
    case WStype_DISCONNECTED:
      Serial.printf("Client %u disconnected\n", num);
      break;
    case WStype_TEXT:
      Serial.printf("Message received: %s\n", payload);
      break;
  }
}

// ----------------------- Функции запуска и остановки задач -------------
void nextionWakeUP()  {
  nextion.print("sleep=0");
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void nextionSleep()  {
  nextion.print("sleep=1");
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void nextionRestart() {
  nextion.print("rest");
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void switchTaskWebServer() {
  if (isTaskActive(taskWebServerHandle)) {
    // Если задача активна - останавливаем
    vTaskSuspend(taskWebServerHandle);
    server.stop();
    webServerRunning = false;
    Serial.println("WebServer: stopped");
  } else {
    // Если задача неактивна - запускаем/возобновляем
    if (taskWebServerHandle == NULL) {
      xTaskCreate(
        taskWebServer,
        "WebServerTask", 
        20480,
        NULL,
        5,
        &taskWebServerHandle
      );
      Serial.println("WebServer: created and running");
    } else {
      vTaskResume(taskWebServerHandle);
      Serial.println("WebServer: resumed");
    }
    server.begin();
    webServerRunning = true;
  }
  sendTaskStateUpdate(); // Отправляем обновление статуса
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
        10240,
        NULL,
        4,
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
        2,
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
        4,
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
        3,
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
      xTaskCreate(
        taskTVOCRead,
        "ENS160 read task", 
        4096,
        NULL,
        1,
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

void handleTaskControl() {
  if (!server.hasArg("task")) {
      server.send(400, "text/plain", "Bad Request: Missing task parameter");
      return;
  }

  String task = server.arg("task");

  if (task == "CO2") {
      switchTaskCO2Read();
  } else if (task == "webServer") {
      switchTaskWebServer();
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
      server.send(400, "text/plain", "Bad Request: Unknown task");
      return;
  }

  // Возвращаем новый JSON со статусами
  sendTaskStateUpdate();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// --------------------------- Задачи FreeRTOS ---------------------------

void taskSendDataToInfluxDB(void *pvParameters) {
    while (1) {
        // Здесь больше не нужно получать данные, они уже в глобальных переменных

        sendDataToInfluxDB(); // Вызываем функцию без аргументов

        vTaskDelay(36000 / portTICK_PERIOD_MS); // Отправка данных раз в минуту
    }
}


void taskWebServer(void *pvParameters)
{
  while (true)
  {
    server.handleClient();  // Обработка HTTP запросов
    vTaskDelay(10 / portTICK_PERIOD_MS); // Небольшая задержка

    
  }
}

void taskWebSocket(void *pvParameters)
{
  while (true)
  {
    webSocket.loop();       // Обработка WebSocket событий
    vTaskDelay(10 / portTICK_PERIOD_MS);
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

void taskBMP280(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
      pressure = bme.readPressure() / 100.0f; // Получаем давление
      homeTemp = bme.readTemperature();
      homeHum = bme.readHumidity();
      homeDP = calculatehomeDP(homeTemp, homeHum);
    xSemaphoreGive(i2cMutex);
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); // Задержка 5 секунд
  }
}

void taskGetTime(void *pvParameters) {
  struct tm timeinfo;
  for (;;) {  // Бесконечный цикл в задаче
    if (getLocalTime(&timeinfo)) {
      int currentMonth = timeinfo.tm_mon + 1;  // tm_mon возвращает месяц от 0 до 11
      //Serial.printf("Текущий месяц: %d\n", currentMonth);
    } else {
      Serial.println("Failed to get time via NTP.");
    }
    vTaskDelay(60000 / portTICK_PERIOD_MS);  // Проверка времени раз в минуту
  }
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
    Serial.print("Parameters sent to forecast: ");
    Serial.println(p);
    forecast = cond.getCast();
    trend = cond.getTrend() / 100.0f;
    Serial.print("Barometric tendence: ");
    Serial.print(trend);
    Serial.println(" hPa");

    vTaskDelay((30 * 60 * 1000) / portTICK_PERIOD_MS);  // 30 минут задержки
  }
}

// Функция отправки данных на экран для page0
void sendPage0Data() {
  String cmd;
  
  // t0: humidity
  cmd = "t0.txt=\"" + String(humidity) + "%\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
  
  // t1: temperature
  cmd = "t1.txt=\"" + String(temperature) + "°C\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
  
  // t3: dewpoint
  cmd = "t3.txt=\"" + String(dewPoint) + "°C\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
  
  // t2: pressure
  cmd = "t2.txt=\"" + String(pressure) + " hPa\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
  
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
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
}

// Функция отправки данных на экран для page1
void sendPage1Data() {
  String cmd;
  
  // t0: homeHum
  cmd = "t0.txt=\"" + String(homeHum) + "%\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
  
  // t1: homeTemp
  cmd = "t1.txt=\"" + String(homeTemp) + "°C\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
  
  // t3: homeDP
  cmd = "t3.txt=\"" + String(homeDP) + "°C\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
  
  // t2: pressure (используем ту же переменную)
  cmd = "t2.txt=\"" + String(pressure) + " hPa\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);

  // t4: CO2
  cmd = "t4.txt=\"" + String(ppm) + " ppm\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);

  // t5: TVOC
  cmd = "t5.txt=\"" + String(TVOC) + " ppb\"";
  nextion.print(cmd);
  nextion.write(0xFF); nextion.write(0xFF); nextion.write(0xFF);
}

// Функция синхронизации состояния кнопок Nextion
void syncWebServerButtonState() {
  if (isTaskActive(taskWebServerHandle)) {
    nextion.print("bt0.val=1");
  } else {
    nextion.print("bt0.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncnRF905ButtonState() {
  if (isTaskActive(taskNRF905Handle)) {
    nextion.print("bt1.val=1");
  } else {
    nextion.print("bt1.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncCO2ButtonState() {
  if (isTaskActive(taskCO2ReadHandle)) {
    nextion.print("bt2.val=1");
  } else {
    nextion.print("bt2.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncNextionButtonState() {
  if (isTaskActive(processNextionTaskHandle)) {
    nextion.print("bt3.val=1");
  } else {
    nextion.print("bt3.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncBMP280ButtonState() {
  if (isTaskActive(taskBMP280Handle)) {
    nextion.print("bt4.val=1");
  } else {
    nextion.print("bt4.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncInfluxDBButtonState() {
  if (isTaskActive(taskSendDataToInfluxDBHandle)) {
    nextion.print("bt5.val=1");
  } else {
    nextion.print("bt5.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncForecastButtonState() {
  if (isTaskActive(taskForecasterHandle)) {
    nextion.print("bt6.val=1");
  } else {
    nextion.print("bt6.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncNTPButtonState() {
  if (isTaskActive(taskGetTimeHandle)) {
    nextion.print("bt7.val=1");
  } else {
    nextion.print("bt7.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

// Отправка данных о состоянии кнопок на Nextion
void sendPage2Data() {
  syncWebServerButtonState();
  syncnRF905ButtonState();
  syncCO2ButtonState();
  syncNextionButtonState();
  syncBMP280ButtonState();
  syncInfluxDBButtonState();
  syncForecastButtonState();
  syncNTPButtonState();
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
    else if (compID == 0x06) {
      Serial.println("Button pressed: bt0");
      // Переключаем веб-сервер
      switchTaskWebServer();
      // Синхронизируем состояние dual-state кнопки
      syncWebServerButtonState();
    }
    else if (compID == 0x07) {
      Serial.println("Button pressed: bt1");
      // Переключаем веб-сервер
      switchTaskNRF905();
      // Синхронизируем состояние dual-state кнопки
      syncnRF905ButtonState();
    }
    else if (compID == 0x08) {
      Serial.println("Button pressed: bt2");
      // Переключаем веб-сервер
      switchTaskCO2Read();
      // Синхронизируем состояние dual-state кнопки
      syncCO2ButtonState();
    }
    else if (compID == 0x09) {
      Serial.println("Button pressed: bt3");
      // Переключаем веб-сервер
      switchTaskNextion();
      // Синхронизируем состояние dual-state кнопки
      syncNextionButtonState();
    }
    else if (compID == 0x0A) {
      Serial.println("Button pressed: bt4");
      // Переключаем веб-сервер
      switchTaskBMP280();
      // Синхронизируем состояние dual-state кнопки
      syncBMP280ButtonState();
    }
    else if (compID == 0x0B) {
      Serial.println("Button pressed: bt5");
      // Переключаем веб-сервер
      switchTaskInfluxDB();
      // Синхронизируем состояние dual-state кнопки
      syncInfluxDBButtonState();
    }
    else if (compID == 0x0C) {
      Serial.println("Button pressed: bt6");
      // Переключаем веб-сервер
      switchTaskForecaster();
      // Синхронизируем состояние dual-state кнопки
      syncForecastButtonState();
    }
    else if (compID == 0x0D) {
      Serial.println("Button pressed: bt7");
      // Переключаем веб-сервер
      switchTaskNTP();
      // Синхронизируем состояние dual-state кнопки
      syncNTPButtonState();
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
        Serial.printf("CO2: %d ppm\n", ppm);
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

void taskTVOCRead(void *pvParameters) {
  unsigned long lastCompensationTime = millis();
  sensors_event_t humidity, temp;
  float rH;
  float tempAHT;

  const int MAX_ERRORS = 3;
  int ens160ErrorCount = 0;
  int aht21ErrorCount = 0;

  while (true) {
      // Получаем доступ к I²C
      if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {

          // Проверка доступности ENS160
          if (ens160.available()) {
              ens160.measure(true);
              AQI = ens160.getAQI();
              TVOC = ens160.getTVOC(); 
              ECO2 = ens160.geteCO2();
              Serial.print("AQI: ");
              Serial.print(AQI);
              Serial.print("\t");
              Serial.print("TVOC: ");
              Serial.print(TVOC);
              Serial.println("ppb\t");
              ens160ErrorCount = 0;  // Сброс счётчика ошибок
          } else {
              ens160ErrorCount++;
              Serial.printf("Error ENS160! Attempt %d of %d\n", ens160ErrorCount, MAX_ERRORS);
          }

          vTaskDelay(50 / portTICK_PERIOD_MS);

          // Проверка доступности AHT21
          if (aht.getEvent(&humidity, &temp)) {
              rH = humidity.relative_humidity;
              tempAHT = temp.temperature;
              aht21ErrorCount = 0;  // Сброс счётчика ошибок
          } else {
              aht21ErrorCount++;
              Serial.printf("Error AHT21! Attempt %d of %d\n", aht21ErrorCount, MAX_ERRORS);
          }

          xSemaphoreGive(i2cMutex);
      } else {
          Serial.println("Failed to get i2cMutex!");
      }

      // Удаление задачи при превышении ошибок
      if (ens160ErrorCount >= MAX_ERRORS || aht21ErrorCount >= MAX_ERRORS) {
          Serial.println("Sensors not responce. Deleting taskTVOCRead...");
          taskTVOCReadHandle = NULL;
          TVOCReadRunning = false;
          sendTaskStateUpdate();
          vTaskDelete(NULL);  // Удаляем текущую задачу
      }

      // Компенсация раз в минуту
      if ((millis() - lastCompensationTime) >= 60000) {
          if (xSemaphoreTake(i2cMutex, portMAX_DELAY) == pdTRUE) {
              ens160.set_envdata(tempAHT, rH);
              Serial.println("Setting temperature and humidity calibration values:");
              Serial.printf("Temperature: %.2f °C, humidity: %.2f %%\n", tempAHT, rH);
              xSemaphoreGive(i2cMutex);
              lastCompensationTime = millis();  // Обновляем время
          }
      }

      vTaskDelay(1000 / portTICK_PERIOD_MS);  // Задержка 1 секунда между циклами
  }
}


// void taskSerialPrint(void *pvParameters)
// {
//   while (true)
//   {
//     Serial.print("Температура дома: ");
//     Serial.print(homeTemp);
//     Serial.println(" °C");
//     Serial.print("Влажность дома: ");
//     Serial.print(homeHum);
//     Serial.println(" %");
//     Serial.print("Точка росы дома: ");
//     Serial.print(homeDP);
//     Serial.println(" °C");
//     Serial.print("Температура: ");
//     Serial.print(temperature);
//     Serial.println(" °C");
//     Serial.print("Влажность: ");
//     Serial.print(humidity);
//     Serial.println(" %");
//     Serial.print("Точка росы: ");
//     Serial.print(dewPoint);
//     Serial.println(" °C");
//     Serial.print("Давление: ");
//     Serial.print(pressure);
//     Serial.println(" hPa");
//     vTaskDelay(10000 / portTICK_PERIOD_MS);
//   }
// }

// ----------------------------- Setup -----------------------------
void setup()
{
  Serial.begin(115200);
  nextion.begin(115200, SERIAL_8N1, RX2, TX2); // Инициализация Serial2  
  Serial.println("ESP32 + Nextion Initialized");


  // Настройка пина для управления PWR_UP nRF905
  pinMode(NRF905_PWR_UP_PIN, OUTPUT);
  digitalWrite(NRF905_PWR_UP_PIN, HIGH);  // В нормальном режиме модуль должен получать питание (HIGH)
  Serial.println("Powering on nRF905...");

  SPI.begin(NRF905_SPI_SCK, NRF905_SPI_MISO, NRF_SPI_MOSI);
  Serial.println("SPI Initialized");


  // Инициализация файловой системы
  if (!LittleFS.begin())
  {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  Serial.println("LittleFS mounted");

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
    Serial.println("Connecting to Wi-Fi...");
  }
  Serial.println("Wi-Fi connected!");
  Serial.println(WiFi.localIP());

  // Настройка сервера
  server.on("/", handleRoot);
  server.on("/admin", handleAdmin);
  server.on("/about", handleAbout);
  server.on("/updateform", handleUpdateform);
  server.on("/update", HTTP_POST, handleUpdateEnd, handleUpdateUpload);
  server.on("/restart", handleRestart);
  server.on("/graph-data", handleGraphData);
  server.on("/getTasksState", handleGetTasksState);
  server.on("/toggleTask", HTTP_POST, handleTaskControl);
  server.on("/sysinfo", HTTP_GET, handleSysInfo);
  server.on("/bmeinfo", HTTP_GET, handleBMEInfo);
  server.on("/nrf905Status", HTTP_GET, handlenRFInfo);
  server.on("/setNRF905", HTTP_POST, handleSetNRF905);
  server.on("/nrfreset", HTTP_POST, handleNRFReset);
  server.serveStatic("/", LittleFS, "/");
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  webServerRunning = true;

  nextionRestart();

  // Инициализация радиомодуля
  if (!driver.init())
  {
    Serial.println("Failed initialize nRF905!");
  } else {
    Serial.println("nRF905 initialized");
  }

  // Настройка канала и диапазона
  driver.setChannel(175, false); // Канал 175 = 439.9 МГц
  driver.setRF(RH_NRF905::TransmitPower10dBm);
  Serial.println("nRF905 configured and ready!");


  // Инициализация датчиков i2c
  if (!bme.begin(0x76)) {
      Serial.println("BME280 not detected. Please check wiring!");
    } else {
      Serial.println("BME280 detected");
  }

  if (!ens160.begin()) {
      Serial.println("Air Quality Sensor did not begin.");
    } else {
      Serial.println("ENS160 detected");

  }

  if (!aht.begin()) {
    Serial.println("AHT21 not detected. Please check wiring!.");
  } else {
    Serial.println("AHT21 detected");
  }

  Serial.println(ens160.available() ? "done." : "failed!");
  if (ens160.available()) {
    // Print ENS160 versions
    Serial.print("\tRev: "); Serial.print(ens160.getMajorRev());
    Serial.print("."); Serial.print(ens160.getMinorRev());
    Serial.print("."); Serial.println(ens160.getBuild());
  
    Serial.print("\tStandard mode ");
    Serial.println(ens160.setMode(ENS160_OPMODE_STD) ? "done." : "failed!");
  }


    
  // bme.setSampling(Adafruit_BME280::MODE_FORCED,     /* Operating Mode. */
  //                Adafruit_BME280::SAMPLING_X1,     /* Temp. oversampling */
  //                Adafruit_BME280::SAMPLING_X8,    /* Pressure oversampling */
  //                Adafruit_BME280::SAMPLING_X1,    /* Humidity oversampling */
  //                Adafruit_BME280::FILTER_X8,      /* Filtering. */
  //                Adafruit_BME280::STANDBY_MS_500); /* Standby time. */
  // Serial.println("BMP280 in forced mode");

  // Настройка времени через NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Установка высоты для Forecaster
  cond.begin();
  cond.setH(61);

  // Создание мьютексов
  i2cMutex = xSemaphoreCreateMutex();
    if (i2cMutex == NULL) {
    Serial.println("Failed to create mutex for i2c!");
  }

  driverMutex = xSemaphoreCreateMutex();
    if (driverMutex == NULL) {
    Serial.println("Failed to create mutex for nRF905!");
  }

  // Создание задач FreeRTOS

  xTaskCreate(taskNRF905, "NRF905 Receiver", 4096, NULL, 4, &taskNRF905Handle);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 3, &taskBMP280Handle);
  xTaskCreate(taskCO2Read, "CO2 read task", 2048, NULL, 2, &taskCO2ReadHandle);
  xTaskCreate(taskTVOCRead, "ENS160 read task", 4096, NULL, 1, &taskTVOCReadHandle);
  xTaskCreate(taskGetTime, "Get NTP Time", 4096, NULL, 2, &taskGetTimeHandle);
  xTaskCreate(taskSendDataToInfluxDB, "InfluxDBTask", 10240, NULL, 4, &taskSendDataToInfluxDBHandle);
  xTaskCreatePinnedToCore(taskWebServer, "Web Server", 20480, NULL, 5, &taskWebServerHandle, 1);
  xTaskCreate(taskForecast, "Forecast task", 2048, NULL, 1, &taskForecasterHandle);
  xTaskCreate(processNextionTask, "Nextion", 4096, NULL, 3, &processNextionTaskHandle);
  //xTaskCreate(taskSerialPrint, "Serial Print", 2048, NULL, 1, NULL);
  xTaskCreate(taskWebSocket, "WebSocket", 4096, NULL, 6, NULL);
}

// ----------------------------- Main loop -----------------------------
void loop()
{
  // Основной цикл пустой, так как задачи обрабатываются в FreeRTOS
}