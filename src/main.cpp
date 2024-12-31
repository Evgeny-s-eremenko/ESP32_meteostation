#include <Arduino.h>
#include <RH_NRF905.h>
#include <NTPClient.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <WiFiUdp.h>
#include "LittleFS.h"
#include <stdint.h>

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

Adafruit_BMP280 bmp;                                             // Датчик давления BMP280
RH_NRF905 driver(NRF905_CE, NRF905_TX_EN, NRF905_CS);            // Радиомодуль nRF905
WebServer server(80);                                            // Веб-сервер на порту 80
WiFiUDP ntpUDP;                                                  // NTP для синхронизации времени
NTPClient timeClient(ntpUDP, "91.206.16.3", 10 * 3600, 60000);  // Клиент NTP (UTC+10)

TaskHandle_t updateHistoryTaskHandle = NULL; // Хендл задачи updateHistoryTask
SemaphoreHandle_t wifiSemaphore; //Семафор на запуск taskNTP при подключении к WiFi
SemaphoreHandle_t ntpSemaphore; // Семафор на запуск updateHistoryTask при синхронизации NTP
unsigned long initialEpoch = 0; // Переменная для хранения начального Unix timestamp

// Структура для параметров updateHistoryTask
struct UpdateHistoryParams {
    unsigned long initialEpoch;
};

// --------------------------- Глобальные переменные ---------------------------
float temperature = 0.0f;
float humidity = 0.0f;
float dewPoint = 0.0f;
float pressure = 0.0f;


// История значений
#define MAX_VALUES 50
float temperatureHistory[MAX_VALUES] = { 0 };
float humidityHistory[MAX_VALUES] = { 0 };
float dewPointHistory[MAX_VALUES] = { 0 };
float pressureHistory[MAX_VALUES] = { 0 };
uint64_t timeHistory[MAX_VALUES] = { 0 };  // Инициализация массива нулями
int currentIndex = 0;
bool historyFull = false;  // Флаг, указывающий, что история заполнена


// -------------------------- Функции для работы с историей --------------------------
void addValue(float *history, float value) {
  history[currentIndex] = value;
}

float calculateDewPoint(float temperature, float humidity) {
  float a = 17.27;
  float b = 237.7;
  float alpha = ((a * temperature) / (b + temperature)) + log(humidity / 100.0);
  return (b * alpha) / (a - alpha);
}

// ---------------------------- Обработчики HTTP запросов -----------------------------
void handleGraphData() {
  char json[4096];
  String jsonString = "{";

  int dataCount = 0;
  if (historyFull) {
    dataCount = MAX_VALUES;
  } else {
    dataCount = currentIndex;
  }

  // Отправляем данные только если есть что отправлять
  if (dataCount > 0) {
    jsonString += "\"temperature\":[";
    for (int i = 0; i < dataCount; i++) {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(temperatureHistory[index], 2);
      if (i < dataCount - 1) jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"humidity\":[";
    for (int i = 0; i < dataCount; i++) {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(humidityHistory[index], 2);
      if (i < dataCount - 1) jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"dewPoint\":[";
    for (int i = 0; i < dataCount; i++) {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(dewPointHistory[index], 2);
      if (i < dataCount - 1) jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"pressure\":[";
    for (int i = 0; i < dataCount; i++) {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      jsonString += String(pressureHistory[index], 2);
      if (i < dataCount - 1) jsonString += ",";
    }
    jsonString += "],";

    jsonString += "\"time\":[";
    for (int i = 0; i < dataCount; i++) {
      int index = (currentIndex - dataCount + i + MAX_VALUES) % MAX_VALUES;
      unsigned long currentTime = timeHistory[index];
      jsonString += String(currentTime);  // Отправляем *полное* время
      if (i < dataCount - 1) jsonString += ",";
    }
    jsonString += "]";

    jsonString += "}";
  } else {
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

void handleRoot() {
  if (LittleFS.exists("/index.html")) {
    File file = LittleFS.open("/index.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
    } else {
      server.send(500, "text/plain", "Failed to open index.html");
    }
  } else {
    server.send(404, "text/plain", "index.html not found");
  }
}


// --------------------------- Задачи FreeRTOS ---------------------------


void taskWebServer(void *pvParameters) {
  while (true) {
    server.handleClient();                // Обработка запросов
    vTaskDelay(10 / portTICK_PERIOD_MS);  // Задержка, чтобы избежать перегрузки процессора
  }
}

void taskNTP(void *pvParameters) {
    // Ждем семафор WiFi
    if (xSemaphoreTake(wifiSemaphore, portMAX_DELAY) == pdTRUE) {
        Serial.println("taskNTP started after wifi connection.");
        while (true) {
            synchronizeWithNTP();
            vTaskDelay(3600000 / portTICK_PERIOD_MS); // Ждем 1 час
        }
    }
    vTaskDelete(NULL);
}


void taskNRF905(void *pvParameters) {
  while (true) {
    if (driver.available()) {
      uint8_t buf[RH_NRF905_MAX_MESSAGE_LEN];
      uint8_t len = sizeof(buf);
      if (driver.recv(buf, &len)) {
        buf[len] = '\0';  // Завершаем строку

        float temp, hum;
        if (sscanf((char *)buf, "T:%f H:%f", &temp, &hum) == 2) {
          temperature = temp;  // Обновляем глобальную переменную
          humidity = hum;      // Обновляем глобальную переменную
          dewPoint = calculateDewPoint(temperature, humidity);
        }
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // Задержка 1 сек между проверками
  }
}

void taskBMP280(void *pvParameters) {
  while (true) {
    if (bmp.begin()) {
      pressure = bmp.readPressure() / 100.0f;  // Получаем давление
    } else {
      pressure = 0.0f;  // Если ошибка, устанавливаем 0
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);  // Задержка 5 секунд
  }
}

void updateHistoryTask(void *pvParameters) {
    // Ждем семафор, прежде чем начать работу
    if (xSemaphoreTake(ntpSemaphore, portMAX_DELAY) == pdTRUE) {
        UpdateHistoryParams *params = (UpdateHistoryParams *)pvParameters;
        unsigned long initialEpoch = params->initialEpoch;
        unsigned long previousMillis = millis();
        Serial.print("initialEpoch in updateHistoryTask: ");
        Serial.println(initialEpoch);

        while (true) {
            unsigned long currentMillis = millis();
            unsigned long timeDifferenceMillis;

            if (currentMillis >= previousMillis) {
                timeDifferenceMillis = currentMillis - previousMillis;
            } else {
                timeDifferenceMillis = (ULONG_MAX - previousMillis) + currentMillis + 1;
            }

            uint32_t timeDifferenceSeconds = timeDifferenceMillis / 1000;

                if (temperature != 0.0f && humidity != 0.0f && pressure != 0.0f) {
                    
                    addValue(temperatureHistory, temperature);
                    addValue(humidityHistory, humidity);
                    addValue(dewPointHistory, dewPoint);
                    addValue(pressureHistory, pressure);

                    timeHistory[currentIndex] = initialEpoch + timeDifferenceSeconds;

                    Serial.print("initialEpoch in updateHistoryTask: ");
                    Serial.println(initialEpoch);
                    Serial.print("timeDifferenceMillis: ");
                    Serial.println(timeDifferenceMillis);
                    Serial.print("timeDifferenceSeconds: ");
                    Serial.println(timeDifferenceSeconds);
                    Serial.print("timeHistory[currentIndex]: ");
                    Serial.println(timeHistory[currentIndex]);

                    currentIndex = (currentIndex + 1) % MAX_VALUES;

                if (currentIndex == 0 && !historyFull) {
                    historyFull = true;
                }
            }

            previousMillis = currentMillis;
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
    }
    vTaskDelete(NULL); 
}

void synchronizeWithNTP() {
    if (WiFi.status() == WL_CONNECTED) {
        if (timeClient.update()) {
            initialEpoch = timeClient.getEpochTime();
            Serial.print("NTP time updated: ");
            Serial.println(initialEpoch);

            // Даем семафор ТОЛЬКО после успешного обновления NTP
            xSemaphoreGive(ntpSemaphore);

            // Пересоздаем задачу updateHistoryTask с новым initialEpoch
            if (updateHistoryTaskHandle != NULL) {
                vTaskDelete(updateHistoryTaskHandle);
            }
            UpdateHistoryParams params;
            params.initialEpoch = initialEpoch;
            BaseType_t xReturned;
            xReturned = xTaskCreate(updateHistoryTask, "Update History Task", 16384, &params, 2, &updateHistoryTaskHandle);
            if( xReturned == pdPASS ) {
                Serial.println("Task created");
            }
            else {
                Serial.println("Task not created");
            }
        } else {
            Serial.println("NTP update failed.");
        }
    } else {
        Serial.println("WiFi not connected. Cannot synchronize with NTP.");
    }
}

void taskSerialPrint(void *pvParameters) {
  while (true) {
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

void taskWifiMonitor(void *pvParameters) {
    while (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(ssid, password);
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        Serial.println("Connecting to WiFi...");
    }
    xSemaphoreGive(wifiSemaphore); // Отдаем семафор после подключения
    Serial.println("WiFi Connected");
    vTaskDelete(NULL);
}
// ----------------------------- Setup -----------------------------
void setup() {
  Serial.begin(115200);
  SPI.begin(NRF905_SPI_SCK, NRF905_SPI_MISO, NRF_SPI_MOSI);

  // Инициализация файловой системы
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  Serial.println("LittleFS mounted");

  // Подключение к Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Подключение к Wi-Fi...");
  }
  Serial.println("Wi-Fi подключен");
  Serial.println(WiFi.localIP());
  wifiSemaphore = xSemaphoreCreateBinary();
  xSemaphoreTake(wifiSemaphore, 0); // Забираем семафор WiFi
  
  timeClient.begin();
      
  int ntpRetries = 0;
  while (!timeClient.update() && ntpRetries < 5) { // Пробуем несколько раз
        Serial.print("Waiting for NTP time... (Attempt ");
        Serial.print(ntpRetries + 1);
        Serial.println(")");
        delay(1000);
        ntpRetries++;
  }

  if (timeClient.isTimeSet()) { // Проверяем, удалось ли установить время
        initialEpoch = timeClient.getEpochTime();
        Serial.print("Initial Epoch: ");
        Serial.println(initialEpoch);
  } else {
        Serial.println("Failed to get NTP time after multiple retries. Using millis() as fallback (time will be incorrect).");
        initialEpoch = millis(); // Плохой вариант, но позволит избежать зависания
  }

  ntpSemaphore = xSemaphoreCreateBinary();
  if (ntpSemaphore == NULL) {
      Serial.println("Failed to create NTP semaphore!");
      while (1);
  }
  xSemaphoreTake(ntpSemaphore, 0); // Забираем семафор сразу после создания


  // Настройка сервера
  server.on("/", handleRoot);
  server.on("/graph-data", handleGraphData);
  server.serveStatic("/", LittleFS, "/");
  server.begin();

  // Инициализация радиомодуля
  if (!driver.init()) {
    Serial.println("Ошибка инициализации NRF905!");
    while (1)
      ;
  }

  // Настройка канала и диапазона
  driver.setChannel(175, false);  // Канал 175 = 439.9 МГц
  Serial.println("Приемник настроен и готов к работе!");

  // Инициализация BMP280
  if (!bmp.begin()) {
    Serial.println("BMP280 не обнаружен!");
  } else {
    Serial.println("BMP280 обнаружен");
  }
  bmp.setSampling(Adafruit_BMP280::MODE_FORCED,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */
  Serial.println("BMP280 in forced mode");

  // Создание задач FreeRTOS

  xTaskCreate(taskWifiMonitor, "WiFi Monitor", 2048, NULL, 5, NULL);
  xTaskCreate(taskNTP, "NTP Client", 16384, NULL, 4, NULL);
  xTaskCreate(taskNRF905, "NRF905 Receiver", 2048, NULL, 1, NULL);
  xTaskCreate(taskBMP280, "BMP280 Sensor", 2048, NULL, 1, NULL);
  xTaskCreate(updateHistoryTask, "Update History Task", 16384, NULL, 2, NULL);
  xTaskCreate(taskWebServer, "Web Server", 16384, NULL, 3, NULL);
  xTaskCreate(taskSerialPrint, "Serial Print", 2048, NULL, 5, NULL);
}

// ----------------------------- Main loop -----------------------------
void loop() {
  // Основной цикл пустой, так как задачи обрабатываются в FreeRTOS
}