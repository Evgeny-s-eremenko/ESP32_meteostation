#include <Arduino.h>
#include <RH_NRF905.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include "LittleFS.h"

// put function declarations here:
// ----------------------------- Wi-Fi и сервер -----------------------------
const char *ssid = "Lucky Devil";
const char *password = "evgen850517";

// ---------------------------- Пины и устройства ----------------------------
#define NRF905_SPI_SCK 14
#define NRF905_SPI_MISO 12
#define NRF_SPI_MOSI 13
#define NRF905_CE 27
#define NRF905_TX_EN 25
#define NRF905_CS 15

Adafruit_BMP280 bmp;                                  // Датчик давления BMP280
RH_NRF905 driver(NRF905_CE, NRF905_TX_EN, NRF905_CS); // Радиомодуль nRF905
WebServer server(80);                                 // Веб-сервер на порту 80

// -------------------------- Объявления функций (прототипы) --------------------------
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
float temperature = 0.0f;
float humidity = 0.0f;
float dewPoint = 0.0f;
float pressure = 0.0f;

// История значений
#define MAX_VALUES 50
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

float calculateDewPoint(float temperature, float humidity)
{
  float a = 17.27;
  float b = 237.7;
  float alpha = ((a * temperature) / (b + temperature)) + log(humidity / 100.0);
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

// --------------------------- Задачи FreeRTOS ---------------------------

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
    if (bmp.begin())
    {
      pressure = bmp.readPressure() / 100.0f; // Получаем давление
    }
    else
    {
      pressure = 0.0f; // Если ошибка, устанавливаем 0
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS); // Задержка 5 секунд
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
    vTaskDelay(60000 / portTICK_PERIOD_MS);
  }
}

// ----------------------------- Setup -----------------------------
void setup()
{
  Serial.begin(115200);
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
  if (!bmp.begin())
  {
    Serial.println("BMP280 не обнаружен!");
  }
  else
  {
    Serial.println("BMP280 обнаружен");
  }
  bmp.setSampling(Adafruit_BMP280::MODE_FORCED,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */
  Serial.println("BMP280 in forced mode");

  // Создание задач FreeRTOS

  xTaskCreate(taskNRF905, "NRF905 Receiver", 2048, NULL, 1, NULL);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 1, NULL);
  xTaskCreate(updateHistoryTask, "Update History Task", 16384, NULL, 2, NULL);
  xTaskCreate(taskWebServer, "Web Server", 16384, NULL, 3, NULL);
  xTaskCreate(taskSerialPrint, "Serial Print", 2048, NULL, 5, NULL);
}

// ----------------------------- Main loop -----------------------------
void loop()
{
  // Основной цикл пустой, так как задачи обрабатываются в FreeRTOS
}