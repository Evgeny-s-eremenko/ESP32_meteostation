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

// put function declarations here:
// ----------------------------- Wi-Fi и сервер -----------------------------
const char *ssid = "REMOVED";
const char *password = "REMOVED";
const char* http_username = "evgen";  // Логин для доступа
const char* http_password = "REMOVED";   // Пароль для доступа

// Параметры InfluxDB
const char* influxDBHost = "REMOVED";
const int influxDBPort = 8086;
const char* influxDBDatabase = "REMOVED";

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


Adafruit_BME280 bme;                                  // Датчик давления BMP280
RH_NRF905 driver(NRF905_CE, NRF905_TX_EN, NRF905_CS); // Радиомодуль nRF905
WebServer server(80);                                 // Веб-сервер на порту 80


// -------------------------- Объявления функций (прототипы) --------------------------
void sendGraphData();
void sendCommand();
void taskSendDataToNextion();
void handleGraphData();
void handleRoot();
void taskWebServer(void *pvParameters);
void taskNRF905(void *pvParameters);
void taskBMP280(void *pvParameters);
void taskSerialPrint(void *pvParameters);
float calculateDewPoint(float temperature, float humidity);

// --------------------------- Глобальные переменные ---------------------------
volatile float temperature = 0.0f;
volatile float humidity = 0.0f;
volatile float dewPoint = 0.0f;
volatile float pressure = 0.0f;
volatile float homeTemp = 0.0f;
volatile float homeHum = 0.0f;
volatile float homeDP = 0.0f;

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

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
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
      if (LittleFS.exists("/REMOVED.html"))
  {
    File file = LittleFS.open("/REMOVED.html", "r");
    if (file)
    {
      server.streamFile(file, "text/html");
      file.close();
    }
    else
    {
      server.send(500, "text/plain", "Failed to open REMOVED.html");
    }
  }
  else
  {
    server.send(404, "text/plain", "REMOVED.html not found");
  }
}

void handleRestart() {
    if (!server.authenticate(http_username, http_password)) {
        return server.requestAuthentication();
    }
    server.send(200, "text/plain", "ESP32 is restarting...");
    delay(1000);
    ESP.restart();
}

void resetNRF905() {
  Serial.println("Performing nRF905 reset...");
  digitalWrite(NRF905_PWR_UP_PIN, LOW);  // Выключаем питание (пин LOW)
  delay(100);                            // Задержка (100 мс – можно настроить по datasheet)
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
      influxDBLine += "pressure=" + String(pressure, 2);
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
    server.handleClient();               // Обработка запросов
    vTaskDelay(10 / portTICK_PERIOD_MS); // Задержка, чтобы избежать перегрузки процессора
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

void taskBMP280(void *pvParameters)
{
  while (true)
  {
    {
      pressure = bme.readPressure() / 100.0f; // Получаем давление
      homeTemp = bme.readTemperature();
      homeHum = bme.readHumidity();
      vTaskDelay(70 / portTICK_PERIOD_MS);
      homeDP = calculatehomeDP(homeTemp, homeHum);
    }

    vTaskDelay(5000 / portTICK_PERIOD_MS); // Задержка 5 секунд
  }
}

void taskSerialPrint(void *pvParameters)
{
  while (true)
  {
    Serial.print("Температура дома: ");
    Serial.print(homeTemp);
    Serial.println(" °C");
    Serial.print("Влажность дома: ");
    Serial.print(homeHum);
    Serial.println(" %");
    Serial.print("Точка росы дома: ");
    Serial.print(homeDP);
    Serial.println(" °C");
    Serial.print("Температура: ");
    Serial.print(temperature);
    Serial.println(" °C");
    Serial.print("Влажность: ");
    Serial.print(humidity);
    Serial.println(" %");
    Serial.print("Точка росы: ");
    Serial.print(dewPoint);
    Serial.println(" °C");
    Serial.print("Давление: ");
    Serial.print(pressure);
    Serial.println(" hPa");
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

void sendGraphData(const char* waveformID, int channel, int value) {
  nextion.print("add ");
  nextion.print(waveformID); // id графика
  nextion.print(",");
  nextion.print(channel);
  nextion.print(",");
  nextion.print(value);
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void sendCommand(const char* command, int value) {
    nextion.print(command); // Отправляем команду (например, x0.val=)
    nextion.print(value);   // Отправляем значение
    nextion.write(0xFF);    // Конец команды
    nextion.write(0xFF);
    nextion.write(0xFF);
}

void taskSendDataToNextion(void *pvParameters) {
    while (1) {
      // Масштабируем значения до целых чисел
      int temp_int = temperature * 100;
      int dew_int = dewPoint * 100;
      int hum_int = humidity * 100;
      int press_int = pressure * 100;
      // Отправляем на Nextion
      sendCommand("x0.val=", temp_int);
      sendCommand("x1.val=", dew_int);
      sendCommand("x3.val=", hum_int);
      sendCommand("x2.val=", press_int);
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void taskSendGraphToNextion(void *pvParameters) {
    while (1) {
        int scaledTemperature = map(temperature, -40, 40, 0, 255); // Масштабируем
        int scaledDewPoint = map(dewPoint, -40, 40, 0, 255); 
        int scaledHumidity = map(humidity, 0, 100, 0, 255);
        int scaledPressure = map(pressure, 980, 1025, 0, 255);

        sendGraphData("1", 0, scaledTemperature);        
        sendGraphData("4", 0, scaledHumidity);
        sendGraphData("3", 0, scaledPressure);
        sendGraphData("1", 1, scaledDewPoint);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
// ----------------------------- Setup -----------------------------
void setup()
{
  Serial.begin(115200);
  nextion.begin(9600, SERIAL_8N1, RX2, TX2); // Инициализация Serial2
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

  // Подключение к Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Подключение к Wi-Fi...");
  }
  Serial.println("Wi-Fi подключен");
  Serial.println(WiFi.localIP());

  // Настройка сервера
  server.on("/", handleRoot);
  server.on("/REMOVED", handleAdmin);
  server.on("/updateform", handleUpdateform);
  server.on("/update", HTTP_POST, handleUpdateEnd, handleUpdateUpload);
  server.on("/restart", handleRestart);
  server.on("/graph-data", handleGraphData);
  server.on("/sysinfo", HTTP_GET, handleSysInfo);
  server.on("/bmeinfo", HTTP_GET, handleBMEInfo);
  server.on("/nrf905Status", HTTP_GET, handlenRFInfo);
  server.on("/setNRF905", HTTP_POST, handleSetNRF905);
  server.on("/nrfreset", HTTP_POST, handleNRFReset);
  server.serveStatic("/", LittleFS, "/");
  server.begin();

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

  // Создание задач FreeRTOS

  xTaskCreate(taskNRF905, "NRF905 Receiver", 2048, NULL, 5, NULL);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 4, NULL);
  xTaskCreate(taskSendDataToNextion, "Send Data to Nextion Task", 4096, NULL, 2, NULL);
  xTaskCreate(taskSendGraphToNextion, "Send Graph Data", 4096, NULL, 2, NULL);
  xTaskCreatePinnedToCore(taskSendDataToInfluxDB, "InfluxDBTask", 10000, NULL, 1, NULL, 1);
  xTaskCreate(taskWebServer, "Web Server", 16384, NULL, 5, NULL);
  xTaskCreate(taskSerialPrint, "Serial Print", 2048, NULL, 1, NULL);
}

// ----------------------------- Main loop -----------------------------
void loop()
{
  // Основной цикл пустой, так как задачи обрабатываются в FreeRTOS
}