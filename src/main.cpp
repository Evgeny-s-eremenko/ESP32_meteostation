#include <Arduino.h>
#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
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
// Флаги задач
volatile bool webServerRunning = false;
volatile bool nRF905Running = false;
volatile bool CO2ReadRunning = false;
volatile bool processNextionRunning = false;
volatile bool BMP280Running = false;
volatile bool sendDataToInfluxDBRunning = false;
volatile bool forecasterRunning = false;
volatile bool getTimeRunning = false;

Forecaster cond;

// ---------------------------- Пины и устройства ----------------------------
#define NRF905_SPI_SCK 14
#define NRF905_SPI_MISO 12
#define NRF_SPI_MOSI 13
#define NRF905_CE 27
#define NRF905_TX_EN 25
#define NRF905_CS 15
#define NRF905_PWR_UP_PIN 26 // пин принудительного сброса nRF905
// ---------------------- Определение пинов serial2 --------------------------
HardwareSerial nextion(2); // Используем Serial2 для связи с дисплеем
#define RX2 16  // RX пин ESP32
#define TX2 17  // TX пин ESP32


//----------------------- Определение пинов Serial1 --------------------------

HardwareSerial mh19(1); //Serial1 для датчика CO2
#define RX1 32
#define TX1 33


Adafruit_BME280 bme;                                  // Датчик давления BMP280
RH_NRF905 driver(NRF905_CE, NRF905_TX_EN, NRF905_CS); // Радиомодуль nRF905
WebServer server(80);                                 // Веб-сервер на порту 80
WebSocketsServer webSocket(81);                       // WebSocket сервер на порту 81


// -------------------------- Объявления функций (прототипы) --------------------------
void sendGraphData();
void sendCommand();
void syncWebServerButtonState();
void processNextionTask();
void handleGraphData();
void handleRoot();
void taskWebServer(void *pvParameters);
void processNextionTask(void *pvParameters);
void taskForecast(void *pvParameters);
void taskCO2Read(void *pvParameters);
void taskGetTime(void *pvParameters);
void taskNRF905(void *pvParameters);
void taskBMP280(void *pvParameters);
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
int ppm = 400;

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

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleGetTasksState() {
  String stateJson = "{";
  stateJson += "\"webServer\":" + String(webServerRunning ? "true" : "false") + ",";
  stateJson += "\"nRF905\":" + String(nRF905Running ? "true" : "false") + ",";
  stateJson += "\"CO2\":" + String(CO2ReadRunning ? "true" : "false") + ",";
  stateJson += "\"nextion\":" + String(processNextionRunning ? "true" : "false") + ",";
  stateJson += "\"BMP280\":" + String(BMP280Running ? "true" : "false") + ",";
  stateJson += "\"InfluxDB\":" + String(sendDataToInfluxDBRunning ? "true" : "false") + ",";
  stateJson += "\"Forecaster\":" + String(forecasterRunning ? "true" : "false") + ",";
  stateJson += "\"NTP\":" + String(getTimeRunning ? "true" : "false");
  stateJson += "}";

  server.send(200, "application/json", stateJson);
}

void sendTaskStateUpdate() {
  String stateJson = "{";
  stateJson += "\"webServer\":" + String(webServerRunning ? "true" : "false") + ",";
  stateJson += "\"nRF905\":" + String(nRF905Running ? "true" : "false") + ",";
  stateJson += "\"CO2\":" + String(CO2ReadRunning ? "true" : "false") + ",";
  stateJson += "\"nextion\":" + String(processNextionRunning ? "true" : "false") + ",";
  stateJson += "\"BMP280\":" + String(BMP280Running ? "true" : "false") + ",";
  stateJson += "\"InfluxDB\":" + String(sendDataToInfluxDBRunning ? "true" : "false") + ",";
  stateJson += "\"Forecaster\":" + String(forecasterRunning ? "true" : "false") + ",";
  stateJson += "\"NTP\":" + String(getTimeRunning ? "true" : "false");
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
    Serial.printf("Начало обновления: %s\n", upload.filename.c_str());

    // Простейшая логика: если имя файла содержит "littlefs", обновляем файловую систему,
    // иначе считаем, что это обновление прошивки.
    if (upload.filename.indexOf("littlefs") >= 0) {
      Serial.println("Обновление файловой системы");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) { // или U_LITTLEFS, если настроено
        Update.printError(Serial);
      }
    } else {
      Serial.println("Обновление прошивки");
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
      Serial.printf("Обновление завершено, записано байт: %u\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    Serial.println("Обновление прервано");
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
    nextion.print("rest");
    nextion.write(0xFF);
    nextion.write(0xFF);
    nextion.write(0xFF);
    delay(1000);
    ESP.restart();
}

void handleNextionRestart() {
  nextion.print("rest");
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
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
  if (driver.init()) {
    Serial.println("nRF905 reinitialized successfully.");
    driver.setChannel(175, false); // Канал 175 = 439.9 МГц
    driver.setRF(RH_NRF905::TransmitPowerm2dBm);
    // Другие необходимые настройки...
  } else {
    Serial.println("Failed to reinitialize nRF905.");
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
  if (!bme.begin(0x76)) {
    status = "BME280: Not Found";
  } else {
    status = "BME280: OK\n";
    // Дополнительно можно вывести текущие показания
    status += "Temp: " + String(bme.readTemperature(), 2) + " °C\n";
    status += "Humidity: " + String(bme.readHumidity(), 2) + " %\n";
    status += "Pressure: " + String(bme.readPressure() / 100.0F, 2) + " hPa\n";
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
  String status = getNRF905Status();
  server.send(200, "text/plain", status);
}

void handleSetNRF905() {
  int channel = server.arg("channel").toInt();
  bool band = (server.arg("band") == "true");
  String powerStr = server.arg("power");
  
  // Преобразование строки в значение TransmitPower
  RH_NRF905::TransmitPower txPower = getTransmitPowerFromString(powerStr);
  
  // Вывод для отладки
  Serial.printf("Получены настройки: channel = %d, band = %s, power = %s\n",
                channel, (band ? "hiband" : "lowband"), powerStr.c_str());
                
  // Применяем настройки через драйвер
  driver.setChannel(channel, band);
  driver.setRF(txPower);

  // Отправляем ответ клиенту
  server.send(200, "text/plain", "Настройки nRF905 приняты");
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
      influxDBLine += "trend=" + String(trend, 2);
    }
    else influxDBLine += "trend=" + String(0, 2);
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
      Serial.printf("Клиент %u подключился\n", num);
      break;
    case WStype_DISCONNECTED:
      Serial.printf("Клиент %u отключился\n", num);
      break;
    case WStype_TEXT:
      Serial.printf("Получено сообщение: %s\n", payload);
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

void switchTaskWebServer() {
  if (taskWebServerHandle == NULL) {
    Serial.println("Запуск задачи WEB-сервера...");

    server.begin();  // Инициализация HTTP-сервера
    webSocket.begin();
    webSocket.onEvent(webSocketEvent); // Назначаем обработчик событий WebSocket

    xTaskCreatePinnedToCore(taskWebServer, "Web Server", 20480, NULL, 5, &taskWebServerHandle, 1);
    webServerRunning = true;
  } else {
    Serial.println("Остановка задачи WEB-сервера...");

    server.stop();  // Завершаем HTTP-сервер
    webSocket.disconnect(); // Закрываем WebSocket
    webSocket.loop();  // Принудительно обновляем WebSocket-состояние

    vTaskDelay(50 / portTICK_PERIOD_MS); // Даем время завершить соединения

    vTaskDelete(taskWebServerHandle);
    taskWebServerHandle = NULL;
    webServerRunning = false;
  }

  sendTaskStateUpdate(); // Отправка обновленного состояния на веб-страницу
}

void switchTaskInfluxDB() {
    if (taskSendDataToInfluxDBHandle == NULL) {
      xTaskCreate(taskSendDataToInfluxDB, "InfluxDBTask", 10240, NULL, 4, &taskSendDataToInfluxDBHandle);
      sendDataToInfluxDBRunning = true;
    } else {
      vTaskDelete(taskSendDataToInfluxDBHandle);
      taskSendDataToInfluxDBHandle = NULL;
      sendDataToInfluxDBRunning = false;
    }
    sendTaskStateUpdate();
 }

void switchTaskCO2Read() {
    if(taskCO2ReadHandle == NULL) {
      mh19.begin(9600, SERIAL_8N1, RX1, TX1);
      xTaskCreate(taskCO2Read, "CO2 read task", 2048, NULL, 2, &taskCO2ReadHandle);
      CO2ReadRunning = true;
    } else {
      mh19.end();
      vTaskDelete(taskCO2ReadHandle);
      taskCO2ReadHandle = NULL;
      CO2ReadRunning = false;
    }
    sendTaskStateUpdate();
}

void switchTaskNRF905() {
    if(taskNRF905Handle == NULL)  {
      xTaskCreate(taskNRF905, "NRF905 Receiver", 4096, NULL, 4, &taskNRF905Handle);
      nRF905Running = true;
    } else  {
      vTaskDelete(taskNRF905Handle);
      taskNRF905Handle = NULL;
      nRF905Running = false;
    }
    sendTaskStateUpdate();
}

void switchTaskNextion()  {
  if(processNextionTaskHandle == NULL)  {
    nextion.begin(115200, SERIAL_8N1, RX2, TX2);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    handleNextionRestart();
    xTaskCreate(processNextionTask, "Nextion", 4096, NULL, 3, &processNextionTaskHandle);
    processNextionRunning = true;
    Serial.println("Nextion task started, display awakened.");
  } else  {
    nextionSleep();
    vTaskDelay(200 / portTICK_PERIOD_MS);
    nextion.end();
    vTaskDelete(processNextionTaskHandle);
    processNextionTaskHandle = NULL;
    processNextionRunning = false;
    Serial.println("Nextion task stopped, display asleep.");    
  }
  vTaskDelay(50 / portTICK_PERIOD_MS);
  sendTaskStateUpdate();
}

void switchTaskBMP280() {
  if(taskBMP280Handle == NULL)  {
    Serial.println("Запуск задачи BMP280...");
    xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 3, &taskBMP280Handle);
    BMP280Running = true;
  } else  {
    Serial.println("Остановка задачи BMP280...");
    Wire.end();
    vTaskDelay(50 / portTICK_PERIOD_MS);
    vTaskDelete(taskBMP280Handle);
    taskBMP280Handle = NULL;
    BMP280Running = false;
  }
  sendTaskStateUpdate();
}

void switchTaskForecaster() {
  if(taskForecasterHandle == NULL)  {
    cond.begin();
    cond.setH(61);
    xTaskCreate(taskForecast, "Forecast task", 2048, NULL, 1, &taskForecasterHandle);
    forecasterRunning = true;
  } else  {
    vTaskDelete(taskForecasterHandle);
    taskForecasterHandle = NULL;
    forecasterRunning = false;
  }
  sendTaskStateUpdate();
}

void switchTaskNTP()  {
  if(taskGetTimeHandle == NULL) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    xTaskCreate(taskGetTime, "Get NTP Time", 4096, NULL, 2, &taskGetTimeHandle);
    getTimeRunning = true;
  } else  {
    vTaskDelete(taskGetTimeHandle);
    taskGetTimeHandle = NULL;
    getTimeRunning = false;
  }
  sendTaskStateUpdate();
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
    vTaskDelay(5 / portTICK_PERIOD_MS); // Небольшая задержка

    webSocket.loop();       // Обработка WebSocket событий
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}
void taskNRF905(void *pvParameters)
{
  // Запоминаем время последнего успешного получения данных
  unsigned long lastReceived = millis();

  while (true)
  {
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
    
    // Проверяем, прошло ли больше 10 минут без получения данных
    if ((millis() - lastReceived) >= 600000)  // 600000 мс = 10 минут
    {
      Serial.println("Нет данных более 10 минут. Выполняется сброс nRF905...");
      resetNRF905();  // Вызываем функцию сброса nRF905
      // Обновляем lastReceived, чтобы не вызывать сброс повторно сразу же
      lastReceived = millis();
    }
    
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Задержка 1 секунда
  }
}

void taskBMP280(void *pvParameters) {
  while (true) {
    if (!bme.begin(0x76)) { // Проверка датчика (I2C-адрес 0x76)
      Serial.println("Ошибка связи с BMP280!");
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    pressure = bme.readPressure() / 100.0f; // Получаем давление
    homeTemp = bme.readTemperature();
    homeHum = bme.readHumidity();
    homeDP = calculatehomeDP(homeTemp, homeHum);

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
      Serial.println("Не удалось получить время через NTP.");
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
    Serial.print("Отправлены параметры в forecast: ");
    Serial.println(p);
    forecast = cond.getCast();
    trend = cond.getTrend() / 100.0f;
    Serial.print("Baric tendence: ");
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
}

// Функция синхронизации состояния кнопок Nextion
void syncWebServerButtonState() {
  if (webServerRunning) {
    nextion.print("bt0.val=1");
  } else {
    nextion.print("bt0.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncnRF905ButtonState() {
  if (nRF905Running) {
    nextion.print("bt1.val=1");
  } else {
    nextion.print("bt1.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncCO2ButtonState() {
  if (CO2ReadRunning) {
    nextion.print("bt2.val=1");
  } else {
    nextion.print("bt2.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncNextionButtonState() {
  if (processNextionRunning) {
    nextion.print("bt3.val=1");
  } else {
    nextion.print("bt3.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncBMP280ButtonState() {
  if (BMP280Running) {
    nextion.print("bt4.val=1");
  } else {
    nextion.print("bt4.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncInfluxDBButtonState() {
  if (sendDataToInfluxDBRunning) {
    nextion.print("bt5.val=1");
  } else {
    nextion.print("bt5.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncForecastButtonState() {
  if (forecasterRunning) {
    nextion.print("bt6.val=1");
  } else {
    nextion.print("bt6.val=0");
  }
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void syncNTPButtonState() {
  if (getTimeRunning) {
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
    Serial.print("Смена страницы: ");
    Serial.println(currentPage);
  }
  else if (msg[0] == 0x65) {
    // Событие от компонента
    // Формат: 65, <PageID>, <ComponentID>, <EventValue>, FF, FF, FF
    uint8_t compID = msg[2];
    if (compID == 0x03) {
      Serial.println("Нажата кнопка b2");
      handleRestartFromNextion();
    }
    else if (compID == 0x04) {
      Serial.println("Нажата кнопка b3");
      resetNRF905();
    }
    else if (compID == 0x05)  {
      Serial.println("Нажата кнопка b4");
      ESP.restart();
    }
    else if (compID == 0x06) {
      Serial.println("Нажата кнопка bt0");
      // Переключаем веб-сервер
      switchTaskWebServer();
      // Синхронизируем состояние dual-state кнопки
      syncWebServerButtonState();
    }
    else if (compID == 0x07) {
      Serial.println("Нажата кнопка bt1");
      // Переключаем веб-сервер
      switchTaskNRF905();
      // Синхронизируем состояние dual-state кнопки
      syncnRF905ButtonState();
    }
    else if (compID == 0x08) {
      Serial.println("Нажата кнопка bt2");
      // Переключаем веб-сервер
      switchTaskCO2Read();
      // Синхронизируем состояние dual-state кнопки
      syncCO2ButtonState();
    }
    else if (compID == 0x09) {
      Serial.println("Нажата кнопка bt3");
      // Переключаем веб-сервер
      switchTaskNextion();
      // Синхронизируем состояние dual-state кнопки
      syncNextionButtonState();
    }
    else if (compID == 0x0A) {
      Serial.println("Нажата кнопка bt4");
      // Переключаем веб-сервер
      switchTaskBMP280();
      // Синхронизируем состояние dual-state кнопки
      syncBMP280ButtonState();
    }
    else if (compID == 0x0B) {
      Serial.println("Нажата кнопка bt5");
      // Переключаем веб-сервер
      switchTaskInfluxDB();
      // Синхронизируем состояние dual-state кнопки
      syncInfluxDBButtonState();
    }
    else if (compID == 0x0C) {
      Serial.println("Нажата кнопка bt6");
      // Переключаем веб-сервер
      switchTaskForecaster();
      // Синхронизируем состояние dual-state кнопки
      syncForecastButtonState();
    }
    else if (compID == 0x0D) {
      Serial.println("Нажата кнопка bt7");
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
        int ppm = (256 * response[2]) + response[3];
        Serial.printf("CO2: %d ppm\n", ppm);
        errorCount = 0; // Сброс ошибок при успехе
      } else {
        errorCount++;
        Serial.printf("Ошибка CRC! Счётчик: %d/%d\n", errorCount, MAX_ERRORS);
      }
    } else {
      errorCount++;
      Serial.printf("Ошибка чтения! Счётчик: %d/%d\n", errorCount, MAX_ERRORS);
    }

    // Проверка на превышение ошибок
    if (errorCount >= MAX_ERRORS) {
      Serial.println("Удаляю задачу...");
      mh19.end();
      taskCO2ReadHandle = NULL;
      CO2ReadRunning = false;
      sendTaskStateUpdate();
      vTaskDelete(NULL);
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void taskMonitor(void *pvParameters) {
  while (1) {
      // Проверяем состояние задач и обновляем флаги
      webServerRunning = (taskWebServerHandle != NULL);
      nRF905Running = (taskNRF905Handle != NULL);
      CO2ReadRunning = (taskCO2ReadHandle != NULL);
      processNextionRunning = (processNextionTaskHandle != NULL);
      BMP280Running = (taskBMP280Handle != NULL);
      sendDataToInfluxDBRunning = (taskSendDataToInfluxDBHandle != NULL);
      forecasterRunning = (taskForecasterHandle != NULL);
      getTimeRunning = (taskGetTimeHandle != NULL);


      // Ждем 5 секунд перед следующим обновлением
      vTaskDelay(pdMS_TO_TICKS(600));
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
  nextionWakeUP();
  Serial.println("ESP32 + Nextion Initialized");


  // Настройка пина для управления PWR_UP nRF905
  pinMode(NRF905_PWR_UP_PIN, OUTPUT);
  digitalWrite(NRF905_PWR_UP_PIN, HIGH);  // В нормальном режиме модуль должен получать питание (HIGH)
  Serial.println("Установка PWR_UP в состояние HIGH");

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
    Serial.println("Подключение к Wi-Fi...");
  }
  Serial.println("Wi-Fi подключен");
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

  // Инициализация радиомодуля
  if (!driver.init())
  {
    Serial.println("Ошибка инициализации NRF905!");
    while (1)
      ;
  }

  // Настройка канала и диапазона
  driver.setChannel(175, false); // Канал 175 = 439.9 МГц
  driver.setRF(RH_NRF905::TransmitPower10dBm);
  Serial.println("Приемник настроен и готов к работе!");

  // Инициализация BMP280
  if (!bme.begin(0x76))
  {
    Serial.println("BMP280 не обнаружен!");
  }
  else
  {
    Serial.println("BMP280 обнаружен");
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

  // Создание задач FreeRTOS

  xTaskCreate(taskNRF905, "NRF905 Receiver", 4096, NULL, 4, &taskNRF905Handle);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 3, &taskBMP280Handle);
  xTaskCreate(taskCO2Read, "CO2 read task", 2048, NULL, 2, &taskCO2ReadHandle);
  xTaskCreate(taskGetTime, "Get NTP Time", 4096, NULL, 2, &taskGetTimeHandle);
  xTaskCreate(taskSendDataToInfluxDB, "InfluxDBTask", 10240, NULL, 4, &taskSendDataToInfluxDBHandle);
  xTaskCreatePinnedToCore(taskWebServer, "Web Server", 20480, NULL, 5, &taskWebServerHandle, 1);
  xTaskCreate(taskForecast, "Forecast task", 2048, NULL, 1, &taskForecasterHandle);
  xTaskCreate(processNextionTask, "Nextion", 4096, NULL, 3, &processNextionTaskHandle);
  //xTaskCreate(taskSerialPrint, "Serial Print", 2048, NULL, 1, NULL);
  xTaskCreate(taskMonitor, "Task Status", 4096, NULL, 2, NULL);
}

// ----------------------------- Main loop -----------------------------
void loop()
{
  // Основной цикл пустой, так как задачи обрабатываются в FreeRTOS
}