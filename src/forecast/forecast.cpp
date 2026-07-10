#include "forecast.h"
#include "../state.h"
#include "../config.h"
#include <Forecaster.h>

extern Forecaster cond;

void taskForecast(void *pvParameters) {
  for (;;) {
    int   m;
    float p_hpa, t;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      m     = month;
      p_hpa = stationPressure;
      t     = temperature;
      xSemaphoreGive(dataMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (m != -1) {
      cond.setMonth(m);
    }

    if (!isnan(p_hpa) && p_hpa > 0.0f) {
      cond.addP((long)(p_hpa * 100.0f), t);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    float newForecast = cond.getCast();
    float newTrend    = cond.getTrend() / 100.0f;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      forecast = newForecast;
      trend    = newTrend;
      xSemaphoreGive(dataMutex);
    }
    ESP_LOGD("FORECAST", "Прогноз: %.1f, тренд: %.2f hPa/3ч", newForecast, newTrend);

    vTaskDelay(pdMS_TO_TICKS(30UL * 60UL * 1000UL));
  }
}
