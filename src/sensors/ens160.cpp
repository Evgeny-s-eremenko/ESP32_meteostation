#include "ens160.h"
#include "../state.h"
#include "../config.h"
#include "../utils/i2c_recovery.h"
#include <SparkFun_ENS160.h>
#include <SparkFun_Qwiic_Humidity_AHT20.h>

extern SparkFun_ENS160 ens160;
extern AHT20 aht20;
extern void sendTaskStateUpdate();

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
      int localAQI = -1, localTVOC = -1, localECO2 = -1;
      if (ens160.begin()) {
        if (ens160.checkDataStatus()) {
          localAQI  = ens160.getAQI();
          localTVOC = ens160.getTVOC();
          localECO2 = ens160.getECO2();
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

      if (localAQI != -1 || localTVOC != -1 || localECO2 != -1) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          if (localAQI != -1)  AQI  = localAQI;
          if (localTVOC != -1) TVOC = localTVOC;
          if (localECO2 != -1) ECO2 = localECO2;
          xSemaphoreGive(dataMutex);
        }
        ESP_LOGV("TVOC", "AQI=%d TVOC=%d ppb eCO2=%d ppm", localAQI, localTVOC, localECO2);
      }
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
