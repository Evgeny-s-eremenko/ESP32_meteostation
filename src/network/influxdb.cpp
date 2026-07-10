#include "influxdb.h"
#include "../state.h"
#include "../config.h"
#include <HTTPClient.h>

void sendDataToInfluxDB() {
  WiFiClient wifiClient;
  HTTPClient http;

  char influxDBLine[512];

  uint8_t curHeater, curFan;
  float   locTemperature, locHumidity, locDewPoint;
  float   locPressure, locHomeTemp, locHomeHum, locHomeDP;
  float   locForecast, locTrend;
  int     locPpm, locTVOC, locAQI, locECO2;
  float   locPm25Level, locPm10Level, locLuxLevel, locUvIndex;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    curHeater    = heaterStatus;
    curFan       = fanStatus;
    locTemperature = temperature;
    locHumidity    = humidity;
    locDewPoint    = dewPoint;
    locPressure    = pressure;
    locHomeTemp    = homeTemp;
    locHomeHum     = homeHum;
    locHomeDP      = homeDP;
    locForecast    = forecast;
    locTrend       = trend;
    locPpm         = ppm;
    locTVOC        = TVOC;
    locAQI         = AQI;
    locECO2        = ECO2;
    locPm25Level   = pm25Level;
    locPm10Level   = pm10Level;
    locLuxLevel    = luxLevel;
    locUvIndex     = uvIndex;
    xSemaphoreGive(dataMutex);
  } else {
    ESP_LOGW("INFLUX", "Таймаут dataMutex — пропуск отправки");
    return;
  }

  bool sendClimate = (curHeater != ST_HEATER && curHeater != ST_COOLING);

  int         offset     = snprintf(influxDBLine, sizeof(influxDBLine), "weather,location=home ");
  int         baseOffset = offset;
  const char *sep        = "";

  if (sendClimate) {
    if (locTemperature != 0.0f) {
      offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                         "%stemperature=%.2f", sep, locTemperature);
      sep = ",";
    }
    if (locHumidity != 0.0f) {
      offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                         "%shumidity=%.2f", sep, locHumidity);
      sep = ",";
    }
    if (!isnan(locDewPoint) && locDewPoint != 0.0f) {
      offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                         "%sdewPoint=%.2f", sep, locDewPoint);
      sep = ",";
    }
  }

  if (locPressure != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%spressure=%.2f", sep, locPressure);
    sep = ",";
  }

  if (locForecast >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sforecast=%.2f", sep, locForecast);
    sep = ",";
  }
  if (locTrend >= -30.0f && locTrend <= 30.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%strend=%.2f", sep, locTrend);
    sep = ",";
  }

  if (locHomeTemp != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%shomeTemp=%.2f", sep, locHomeTemp);
    sep = ",";
  }
  if (locHomeHum != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%shomeHum=%.2f", sep, locHomeHum);
    sep = ",";
  }
  if (!isnan(locHomeDP) && locHomeDP != 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%shomeDP=%.2f", sep, locHomeDP);
    sep = ",";
  }

  if (locPpm != 0) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sCO2=%.0f", sep, (float)locPpm);
    sep = ",";
  }
  if (locTVOC != -1) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sTVOC=%.0f", sep, (float)locTVOC);
    sep = ",";
  }
  if (locAQI != -1) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sAQI=%.0f", sep, (float)locAQI);
    sep = ",";
  }
  if (locECO2 != -1) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sECO2=%.0f", sep, (float)locECO2);
    sep = ",";
  }

  if (locPm25Level >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%spm25Level=%.2f", sep, locPm25Level);
    sep = ",";
  }
  if (locPm10Level >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%spm10Level=%.2f", sep, locPm10Level);
    sep = ",";
  }

  if (locLuxLevel >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sluxLevel=%.2f", sep, locLuxLevel);
    sep = ",";
  }
  if (locUvIndex >= 0.0f) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%suvIndex=%.2f", sep, locUvIndex);
    sep = ",";
  }

  if (curHeater >= 1 && curHeater <= 3) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sheaterStatus=%.0f", sep, (float)curHeater);
    sep = ",";
  }
  if (curFan == ST_FAN_OFF || curFan == ST_FAN_ON) {
    offset += snprintf(influxDBLine + offset, sizeof(influxDBLine) - offset,
                       "%sfanStatus=%.0f", sep, (float)curFan);
    sep = ",";
  }

  if (offset == baseOffset) return;

  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d/write?db=%s",
           influxDBHost, influxDBPort, influxDBDatabase);

  http.begin(wifiClient, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(influxDBLine);

  if (code > 0) {
    ESP_LOGI("InfluxDB", "HTTP %d", code);
    if (code != 204) {
      String resp = http.getString();
      ESP_LOGD("InfluxDB", "Ответ: %s", resp.c_str());
    }
  } else {
    ESP_LOGE("InfluxDB", "Ошибка: %s", http.errorToString(code).c_str());
  }
  http.end();
}

void taskSendDataToInfluxDB(void *pvParameters) {
  while (true) {
    sendDataToInfluxDB();
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}
