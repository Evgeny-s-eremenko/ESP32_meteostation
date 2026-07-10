#include "i2c_recovery.h"
#include "../state.h"
#include <board_config.h>
#include <Adafruit_BME280.h>
#include <SparkFun_ENS160.h>
#include <SparkFun_Qwiic_Humidity_AHT20.h>

extern Adafruit_BME280 bme;
extern SparkFun_ENS160 ens160;
extern AHT20 aht20;

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

void resetI2CBus() {
  i2cResetCount++;
  if (i2cMutex != NULL) {
    vSemaphoreDelete(i2cMutex);
    i2cMutex = NULL;
  }
  ESP_LOGE("SYS", "Сброс шины I2C (сброс #%u)...", i2cResetCount);

  // 1. Принудительно освобождаем шину
  pinMode(I2C_SCL, OUTPUT);
  pinMode(I2C_SDA, OUTPUT);

  // Принудительный STOP-сигнал на шине
  digitalWrite(I2C_SDA, LOW);
  vTaskDelay(pdMS_TO_TICKS(1));
  digitalWrite(I2C_SCL, HIGH);
  vTaskDelay(pdMS_TO_TICKS(1));
  digitalWrite(I2C_SDA, HIGH);
  vTaskDelay(pdMS_TO_TICKS(5));

  // Генерируем 10 импульсов такта
  pinMode(I2C_SDA, INPUT_PULLUP);
  for (int i = 0; i < 10; i++) {
    digitalWrite(I2C_SCL, LOW);  delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
  }

  Wire.end();
  vTaskDelay(pdMS_TO_TICKS(50));

  // 2. Перезапускаем Wire с явным указанием частоты
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  checkMutex();

  // 3. Пошаговая инициализация с паузами
  if (bme.begin(0x76)) {
    ESP_LOGI("SYS", "BME280 восстановлен");
  } else {
    ESP_LOGE("SYS", "BME280 не найден!");
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  if (aht20.begin()) {
    ESP_LOGI("SYS", "AHT20 восстановлен");
  } else {
    ESP_LOGE("SYS", "AHT20 не найден!");
  }
  vTaskDelay(pdMS_TO_TICKS(20));

  // Для ENS160: сначала инициализируем базовое состояние
  if (ens160.begin()) {
    ESP_LOGI("SYS", "ENS160 обнаружен, выводим из DEEP_SLEEP...");
    ens160.setOperatingMode(SFE_ENS160_STANDARD);
    // ENS160 после выхода из сна/сброса требует до 1 секунды
    vTaskDelay(pdMS_TO_TICKS(1000));
  } else {
    ESP_LOGE("SYS", "ENS160 не найден после сброса!");
  }
}
