#include <Arduino.h>
#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include "LittleFS.h"
#include <HTTPClient.h>

// put function declarations here:
// ----------------------------- Wi-Fi и сервер -----------------------------
const char *ssid = "REMOVED";
const char *password = "REMOVED";

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
void updateHistoryTask(void *pvParameters);
void taskSerialPrint(void *pvParameters);
void addValue(float *history, float value);
float calculateDewPoint(float temperature, float humidity);

// --------------------------- Глобальные переменные ---------------------------
volatile float temperature = 0.0f;
volatile float humidity = 0.0f;
volatile float dewPoint = 0.0f;
volatile float pressure = 0.0f;
volatile float homeTemp = 0.0f;
volatile float homeHum = 0.0f;
volatile float homeDP = 0.0f;

// История значений
#define MAX_VALUES 100
float temperatureHistory[MAX_VALUES] = {0};
float humidityHistory[MAX_VALUES] = {0};
float dewPointHistory[MAX_VALUES] = {0};
float pressureHistory[MAX_VALUES] = {0};
unsigned long counterHistory[MAX_VALUES] = {0}; // Массив для счетчика
int currentIndex = 0;

bool historyFull = false;

unsigned long counter = 0; // Глобальный счетчик

// -------------------------- Функции для работы с историей --------------------------
void addValue(float *history, float value)
{
  history[currentIndex] = value;
}

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
void handleGraphData()
{
  char json[4096];
  String jsonString = "{";

  int dataCount = 0;
  if (historyFull)
  {
    dataCount = MAX_VALUES;
  }
  else
  {
    dataCount = currentIndex;
  }

  // Отправляем данные только если есть что отправлять
  if (dataCount > 0)
  {
    jsonString += "\"temperature\":[";
    for (int i = 0; i < dataCount; i++)
    {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(temperatureHistory[index], 2);
      if (i < dataCount - 1)
        jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"humidity\":[";
    for (int i = 0; i < dataCount; i++)
    {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(humidityHistory[index], 2);
      if (i < dataCount - 1)
        jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"dewPoint\":[";
    for (int i = 0; i < dataCount; i++)
    {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(dewPointHistory[index], 2);
      if (i < dataCount - 1)
        jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"pressure\":[";
    for (int i = 0; i < dataCount; i++)
    {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(pressureHistory[index], 2);
      if (i < dataCount - 1)
        jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"counter\":["; // Отправляем значения счетчика
    for (int i = 0; i < dataCount; i++)
    {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(counterHistory[index]);
      if (i < dataCount - 1)
        jsonString += ",";
    }
    jsonString += "]";

    jsonString += "}";
  }
  else
  {
    jsonString += "\"temperature\":[],";
    jsonString += "\"humidity\":[],";
    jsonString += "\"dewPoint\":[],";
    jsonString += "\"pressure\":[],";
    jsonString += "\"time\":[]";
    jsonString += "}";
  }

  jsonString.toCharArray(json, sizeof(json));
  server.send(200, "application/json", json);
}

void handleRoot()
{
  if (LittleFS.exists("/HTMLPage1.html"))
  {
    File file = LittleFS.open("/HTMLPage1.html", "r");
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

// Функция для отправки данных в InfluxDB (без аргументов)
void sendDataToInfluxDB() {
    WiFiClient client;
    HTTPClient http;

    // Формируем строку InfluxDB Line Protocol
    String influxDBLine = "weather,location=home ";
    influxDBLine += "temperature=" + String(temperature, 2) + ",";
    influxDBLine += "humidity=" + String(humidity, 2) + ",";
    influxDBLine += "dewPoint=" + String(dewPoint, 2) + ","; // Добавили dewPoint
    influxDBLine += "pressure=" + String(pressure, 2);

    String url = "http://" + String(influxDBHost) + ":" + String(influxDBPort) + "/write?db=" + String(influxDBDatabase);

    http.begin(client, url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int httpResponseCode = http.POST(influxDBLine);

    if (httpResponseCode > 0) {
        Serial.print("HTTP Response code: ");
        Serial.println(httpResponseCode);
        if (httpResponseCode != 204) {
          Serial.println(http.getString()); // Выводим ответ сервера для отладки
        }
    } else {
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

        vTaskDelay(60000 / portTICK_PERIOD_MS); // Отправка данных раз в минуту
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
          temperature = temp; // Обновляем глобальную переменную
          humidity = hum;     // Обновляем глобальную переменную
          dewPoint = calculateDewPoint(temperature, humidity);
        }
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Задержка 1 сек между проверками
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
      homeDP = calculatehomeDP(homeTemp, homeHum);
    }

    vTaskDelay(2000 / portTICK_PERIOD_MS); // Задержка 5 секунд
  }
}

void updateHistoryTask(void *pvParameters)
{
  while (true)
  {
    if (temperature != 0.0f && humidity != 0.0f && pressure != 0.0f)
    {
      addValue(temperatureHistory, temperature);
      addValue(humidityHistory, humidity);
      addValue(dewPointHistory, dewPoint);
      addValue(pressureHistory, pressure);

      counter++;                              // Увеличиваем счетчик
      counterHistory[currentIndex] = counter; // Сохраняем значение счетчика

      currentIndex = (currentIndex + 1) % MAX_VALUES;
      if (currentIndex == 0 && !historyFull)
      {
        historyFull = true;
      }
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

  SPI.begin(NRF905_SPI_SCK, NRF905_SPI_MISO, NRF_SPI_MOSI);

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
  server.on("/graph-data", handleGraphData);
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
  //bme.setSampling(Adafruit_BME280::MODE_FORCED,     /* Operating Mode. */
  //                Adafruit_BME280::SAMPLING_X2,     /* Temp. oversampling */
  //                Adafruit_BME280::SAMPLING_X16,    /* Pressure oversampling */
  //                Adafruit_BME280::SAMPLING_NONE,    /* Humidity oversampling */
  //                Adafruit_BME280::FILTER_X16,      /* Filtering. */
  //                Adafruit_BME280::STANDBY_MS_1000); /* Standby time. */
  //Serial.println("BMP280 in forced mode");

  // Создание задач FreeRTOS

  xTaskCreate(taskNRF905, "NRF905 Receiver", 2048, NULL, 5, NULL);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 4, NULL);
  xTaskCreate(taskSendDataToNextion, "Send Data to Nextion Task", 4096, NULL, 2, NULL);
  xTaskCreate(taskSendGraphToNextion, "Send Graph Data", 4096, NULL, 2, NULL);
  xTaskCreatePinnedToCore(taskSendDataToInfluxDB, "InfluxDBTask", 10000, NULL, 1, NULL, 1);
  xTaskCreate(updateHistoryTask, "Update History Task", 16384, NULL, 6, NULL);
  xTaskCreate(taskWebServer, "Web Server", 16384, NULL, 5, NULL);
  xTaskCreate(taskSerialPrint, "Serial Print", 2048, NULL, 1, NULL);
}

// ----------------------------- Main loop -----------------------------
void loop()
{
  // Основной цикл пустой, так как задачи обрабатываются в FreeRTOS
}