#include "bme280.h"
#include "../state.h"
#include "../config.h"
#include "../utils/i2c_recovery.h"
#include <Adafruit_BME280.h>

extern Adafruit_BME280 bme;

float es(float tempC) {
  return 6.112f * expf((17.67f * tempC) / (tempC + 243.5f));
}

float calculatehomeDP(float temp, float hum) {
  const float a = 17.27f, b = 237.7f;
  float alpha = ((a * temp) / (b + temp)) + logf(hum / 100.0f);
  return (b * alpha) / (a - alpha);
}

void taskBMP280(void *pvParameters) {
  while (true) {
    checkMutex();
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      float rawTemp = bme.readTemperature();
      float rawHum  = bme.readHumidity();
      float rawPres = bme.readPressure() / 100.0f;

      // Кэшируем volatile-значения в локальные переменные
      float localTCorr = tCorr;
      float localAltM  = altitude_m;

      // Коррекция влажности: учитываем температуру размещения датчика
      float eRaw  = es(rawTemp);
      float eCorr = es(rawTemp + localTCorr);
      float rawHomeHum = rawHum * (eRaw / eCorr);
      if (rawHomeHum > 100.0f) rawHomeHum = 100.0f;

      // Приведение давления к уровню моря
      float seaLevelPres = bme.seaLevelForAltitude(localAltM, rawPres * 100.0f) / 100.0f;

      float correctedTemp = rawTemp + localTCorr;
      float rawHomeDP = calculatehomeDP(correctedTemp, rawHomeHum);

      xSemaphoreGive(i2cMutex);

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        pressure        = seaLevelPres;
        stationPressure = rawPres;
        homeTemp        = correctedTemp;
        homeHum         = rawHomeHum;
        homeDP          = rawHomeDP;
        xSemaphoreGive(dataMutex);
      }
    } else {
      ESP_LOGE("MUTEX", "Таймаут i2cMutex (задача: %s, держатель: %s)",
               pcTaskGetTaskName(NULL),
               xSemaphoreGetMutexHolder(i2cMutex)
                   ? pcTaskGetTaskName(xSemaphoreGetMutexHolder(i2cMutex))
                   : "none");
      resetI2CBus();
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
