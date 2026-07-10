#include "server.h"
#include "ota.h"
#include "../state.h"
#include "../config.h"
#include "../sensors/bme280.h"
#include "../sensors/ens160.h"
#include "../sensors/co2.h"
#include "../sensors/nrf905.h"
#include "../display/nextion.h"
#include "../network/influxdb.h"
#include "../network/time_sync.h"
#include "../network/wifi.h"
#include "../utils/settings.h"
#include "../utils/i2c_recovery.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SparkFun_ENS160.h>
#include <SparkFun_Qwiic_Humidity_AHT20.h>
#include <RH_NRF905.h>
#include <Adafruit_BME280.h>
#include <Forecaster.h>

extern SparkFun_ENS160 ens160;
extern AHT20 aht20;
extern RH_NRF905 driver;
extern Forecaster cond;
extern Adafruit_BME280 bme;

// Forward declarations for task functions
void taskNRF905(void *pvParameters);
void taskCO2Read(void *pvParameters);
void taskBMP280(void *pvParameters);
void taskForecast(void *pvParameters);
void taskGetTime(void *pvParameters);
void taskTVOCRead(void *pvParameters);
void taskSendDataToInfluxDB(void *pvParameters);

AsyncWebServer server(80);
AsyncWebSocket webSocket("/ws");
AsyncWebSocket webSocket1("/ws1");

bool isAuthenticated(AsyncWebServerRequest *request) {
  if (!request->authenticate(http_username, http_password)) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

bool isTaskActive(TaskHandle_t taskHandle) {
  if (taskHandle == NULL) return false;
  eTaskState state = eTaskGetState(taskHandle);
  return (state == eRunning || state == eReady || state == eBlocked);
}

void buildTaskStateJson(char *buffer, size_t bufferSize) {
  snprintf(buffer, bufferSize,
           "{\"nRF905\":%s,\"CO2\":%s,\"nextion\":%s,"
           "\"BMP280\":%s,\"InfluxDB\":%s,\"Forecaster\":%s,"
           "\"NTP\":%s,\"TVOC\":%s}",
           isTaskActive(taskNRF905Handle)             ? "true" : "false",
           isTaskActive(taskCO2ReadHandle)            ? "true" : "false",
           isTaskActive(processNextionTaskHandle)     ? "true" : "false",
           isTaskActive(taskBMP280Handle)             ? "true" : "false",
           isTaskActive(taskSendDataToInfluxDBHandle) ? "true" : "false",
           isTaskActive(taskForecasterHandle)         ? "true" : "false",
           isTaskActive(taskGetTimeHandle)            ? "true" : "false",
           isTaskActive(taskTVOCReadHandle)           ? "true" : "false");
}

void sendTaskStateUpdate() {
  char jsonBuffer[256];
  buildTaskStateJson(jsonBuffer, sizeof(jsonBuffer));
  webSocket.textAll(jsonBuffer);
}

void handleGraphData(AsyncWebServerRequest *request) {
  StaticJsonDocument<384> doc;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    doc["temperature"] = temperature;
    doc["humidity"]    = humidity;
    doc["dewPoint"]    = dewPoint;
    doc["pressure"]    = pressure;
    doc["homeTemp"]    = homeTemp;
    doc["homeHum"]     = homeHum;
    doc["homeDP"]      = homeDP;
    doc["forecast"]    = forecast;
    doc["trend"]       = trend;
    doc["CO2"]         = ppm;
    doc["TVOC"]        = TVOC;
    doc["LUX"]         = luxLevel;
    doc["PM2.5"]       = pm25Level;
    doc["PM10"]        = pm10Level;
    doc["UV"]          = uvIndex;
    doc["FAN"]         = fanStatus;
    doc["HEAT"]        = heaterStatus;
    xSemaphoreGive(dataMutex);
  }

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->addHeader("Cache-Control", "no-cache");
  serializeJson(doc, *response);
  request->send(response);
}

bool isMobile(AsyncWebServerRequest *request) {
    String ua = request->header("User-Agent");
    ua.toLowerCase();
    return ua.indexOf("mobile") != -1 || ua.indexOf("android") != -1
        || ua.indexOf("iphone") != -1;
}

void handleRoot(AsyncWebServerRequest *request) {
    if (isMobile(request) && LittleFS.exists("/mobile.html")) {
        request->send(LittleFS, "/mobile.html", "text/html");
    } else if (LittleFS.exists("/index.html")) {
        request->send(LittleFS, "/index.html", "text/html");
    } else {
        request->send(404, "text/plain", "index.html not found");
    }
}

void handleAdmin(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(LittleFS, "/admin.html", "text/html");
}

void handleAbout(AsyncWebServerRequest *request) {
  if (LittleFS.exists("/about.html")) {
    request->send(LittleFS, "/about.html", "text/html");
  } else {
    request->send(404, "text/plain", "about.html not found");
  }
}

void handleUpdateForm(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  if (LittleFS.exists("/updateform.html")) {
    request->send(LittleFS, "/updateform.html", "text/html");
  } else {
    request->send(404, "text/plain", "updateform.html not found");
  }
}

void handleRestart(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(200, "text/plain", "ESP32 restarting...");
  ESP_LOGW("SYS", "Перезагрузка по команде из веб-интерфейса");
  nextionRestart();
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  ESP.restart();
}

void handleRestartFromNextion() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  ESP.restart();
}

void getSystemInfo(char *buffer, size_t len) {
  unsigned long uptime  = millis() / 1000;
  int days    = uptime / 86400;
  int hours   = (uptime % 86400) / 3600;
  int minutes = (uptime % 3600) / 60;
  int seconds = uptime % 60;

  char uptimeStr[20];
  snprintf(uptimeStr, sizeof(uptimeStr), "%dd %02dh %02dm %02ds",
           days, hours, minutes, seconds);

  IPAddress ip = WiFi.localIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

  snprintf(buffer, len,
           "Uptime: %s\nChip model: %s\nChip rev.: %d\n"
           "WiFi RSSI: %d dBm\nIP address: %s\n"
           "Free Heap: %u bytes\nMax Alloc: %u bytes\n"
           "%s"
           "WebServer free stack: %d bytes\n",
           uptimeStr,
           ESP.getChipModel(), ESP.getChipRevision(),
           WiFi.RSSI(),
           ipStr,
           ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
           (ESP.getFreeHeap() < 32768) ? "*** LOW MEMORY ***\n" : "",
           uxTaskGetStackHighWaterMark(NULL));
}

void handleSysInfo(AsyncWebServerRequest *request) {
  char info[256];
  getSystemInfo(info, sizeof(info));
  request->send(200, "text/plain", info);
}

const char *modeToString(uint8_t mode) {
  switch (mode) {
    case 0x00: return "DEEP_SLEEP";
    case 0x01: return "IDLE";
    case 0x02: return "STANDARD";
    case 0xF0: return "RESET";
    default:   return "UNKNOWN";
  }
}

void getBME280Status(char *buffer, size_t len) {
  size_t offset = 0;
  checkMutex();

  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    offset += snprintf(buffer + offset, len - offset,
                       bme.begin(0x76) ? "BME280: Found\nTemp: %.2f °C\nHumidity: %.2f %%\nPressure: %.2f hPa\n"
                                       : "BME280: Not Found\n",
                       bme.readTemperature(), bme.readHumidity(),
                       bme.readPressure() / 100.0f);

    if (!ens160.begin()) {
      offset += snprintf(buffer + offset, len - offset, "ENS160: Not Found\n");
    } else {
      offset += snprintf(buffer + offset, len - offset,
                         "ENS160: Found\nMode: %s\nStatus: %s\nI2C Resets: %u\n",
                         modeToString(ens160.getOperatingMode()),
                         ens160.getOperationError() ? "Error" : "OK",
                         i2cResetCount);
    }
    xSemaphoreGive(i2cMutex);
  } else {
    snprintf(buffer, len, "i2cMutex занят (задача: %s)\n", pcTaskGetTaskName(NULL));
    ESP_LOGE("MUTEX", "Таймаут i2cMutex в задаче %s", pcTaskGetTaskName(NULL));
    resetI2CBus();
  }
}

void handleBMEInfo(AsyncWebServerRequest *request) {
  static char statusBuffer[256];
  getBME280Status(statusBuffer, sizeof(statusBuffer));
  request->send_P(200, "text/plain", statusBuffer);
}

void getNRF905Status(char *buffer, size_t bufferSize) {
  char    temp[32];
  uint8_t config[10];
  int     pos = 0;

  uint8_t status_reg = driver.spiBurstReadRegister(RH_NRF905_REG_W_CONFIG, config, 10);
  pos += snprintf(buffer, bufferSize, "Status: 0x%02X\n", status_reg);

  if (status_reg & 0x20) pos += snprintf(buffer + pos, bufferSize - pos, "[DR] Data Ready\n");
  if (status_reg & 0x80) pos += snprintf(buffer + pos, bufferSize - pos, "[AM] Address Match\n");
  pos += snprintf(buffer + pos, bufferSize - pos,
                  (status_reg & 0x40) ? "[CRC_ERR]\n" : "[CRC_OK]\n");

  uint8_t band_bit = config[1] & RH_NRF905_CONFIG_1_HFREQ_PLL;
  float   freq     = 422.4f + (config[0] / 10.0f);
  if (band_bit) freq *= 2.0f;

  const char *pwr_str[] = {"-10 dBm", "-2 dBm", "+6 dBm", "+10 dBm"};
  uint8_t     pwr       = (config[1] & RH_NRF905_CONFIG_1_PA_PWR) >> 2;

  pos += snprintf(buffer + pos, bufferSize - pos,
                  "Channel: %d\nFreq: %.3f MHz\nTX Power: %s\n",
                  config[0], freq, pwr_str[pwr]);

  pos += snprintf(buffer + pos, bufferSize - pos, "RAW Config: ");
  for (int i = 0; i < 10; i++) {
    snprintf(temp, sizeof(temp), "%02X ", config[i]);
    strncat(buffer, temp, bufferSize - strlen(buffer) - 1);
  }
  snprintf(temp, sizeof(temp), "\nnRF905 Resets: %u\n", nRF905ResetCount);
  strncat(buffer, temp, bufferSize - strlen(buffer) - 1);
}

void handlenRFInfo(AsyncWebServerRequest *request) {
  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    static char status[256];
    getNRF905Status(status, sizeof(status));
    request->send_P(200, "text/plain", status);
    xSemaphoreGive(driverMutex);
  }
}

RH_NRF905::TransmitPower getTransmitPowerFromString(const String &s) {
  if (s == "TransmitPowerm10dBm") return RH_NRF905::TransmitPowerm10dBm;
  if (s == "TransmitPowerm2dBm")  return RH_NRF905::TransmitPowerm2dBm;
  if (s == "TransmitPower6dBm")   return RH_NRF905::TransmitPower6dBm;
  return RH_NRF905::TransmitPower10dBm;
}

void handleSetNRF905(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;

  if (request->hasParam("channel", true) &&
      request->hasParam("band",    true) &&
      request->hasParam("power",   true)) {
    int    channel  = request->getParam("channel", true)->value().toInt();
    bool   band     = (request->getParam("band",   true)->value() == "true");
    String powerStr = request->getParam("power",   true)->value();

    ESP_LOGI("NRF905", "Новые настройки: канал=%d band=%s мощность=%s",
             channel, band ? "hiband" : "lowband", powerStr.c_str());

    if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
      driver.setChannel(channel, band);
      driver.setRF(getTransmitPowerFromString(powerStr));
      xSemaphoreGive(driverMutex);
    }
    nrf905SaveSettings(channel, band, powerStr.c_str());
    request->send(200, "text/plain", "nRF905 settings applied");
  } else {
    request->send(400, "text/plain", "Invalid parameters");
  }
}

void handleNRFReset(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  resetNRF905();
  request->send(200, "text/plain", "nRF905 reset done.");
}

void handleResetI2CBus(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  resetI2CBus();
  request->send(200, "text/plain", "I2C bus reset done.");
}

void handleResetNVS(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;
  request->send(200, "text/plain", "NVS cleared.");
  resetNVS();
}

void handleGetSettings(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;

  char json[1536];
  snprintf(json, sizeof(json),
    "{\"wifi_ssid\":\"%s\",\"wifi_pass\":\"****\","
    "\"http_user\":\"%s\",\"http_pass\":\"****\","
    "\"use_static_ip\":%d,\"static_ip\":\"%s\",\"static_gateway\":\"%s\",\"static_subnet\":\"%s\",\"static_dns\":\"%s\","
    "\"influx_host\":\"%s\",\"influx_port\":%d,\"influx_db\":\"%s\","
    "\"ntp_server\":\"%s\","
    "\"latitude\":%.6f,\"longitude\":%.6f,"
    "\"tz_offset\":%d,\"tz_sec\":%ld,"
    "\"tCorr\":%.2f,\"altitude_m\":%.1f}",
    ssid, http_username,
    useStaticIP ? 1 : 0, staticIP, staticGateway, staticSubnet, staticDNS,
    influxDBHost, influxDBPort, influxDBDatabase,
    ntpServer, latitude, longitude,
    tzOffset, gmtOffset_sec,
    tCorr, altitude_m);

  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(response);
}

void handleSetSettings(AsyncWebServerRequest *request) {
  if (!isAuthenticated(request)) return;

  if (request->hasParam("wifi_ssid", true))   strncpy(ssid,          request->getParam("wifi_ssid",  true)->value().c_str(), sizeof(ssid) - 1);
  if (request->hasParam("wifi_pass", true)) {
    const char *p = request->getParam("wifi_pass", true)->value().c_str();
    if (strlen(p) > 0 && strcmp(p, "****") != 0) strncpy(password, p, sizeof(password) - 1);
  }
  if (request->hasParam("http_user", true))   strncpy(http_username, request->getParam("http_user",  true)->value().c_str(), sizeof(http_username) - 1);
  if (request->hasParam("http_pass", true)) {
    const char *p = request->getParam("http_pass", true)->value().c_str();
    if (strlen(p) > 0 && strcmp(p, "****") != 0) strncpy(http_password, p, sizeof(http_password) - 1);
  }
  if (request->hasParam("use_static_ip", true))
    useStaticIP = (request->getParam("use_static_ip", true)->value() == "1");
  if (request->hasParam("static_ip", true))
    strncpy(staticIP, request->getParam("static_ip", true)->value().c_str(), sizeof(staticIP) - 1);
  if (request->hasParam("static_gateway", true))
    strncpy(staticGateway, request->getParam("static_gateway", true)->value().c_str(), sizeof(staticGateway) - 1);
  if (request->hasParam("static_subnet", true))
    strncpy(staticSubnet, request->getParam("static_subnet", true)->value().c_str(), sizeof(staticSubnet) - 1);
  if (request->hasParam("static_dns", true))
    strncpy(staticDNS, request->getParam("static_dns", true)->value().c_str(), sizeof(staticDNS) - 1);
  if (request->hasParam("influx_host", true)) strncpy(influxDBHost,     request->getParam("influx_host", true)->value().c_str(), sizeof(influxDBHost) - 1);
  if (request->hasParam("influx_port", true)) influxDBPort = request->getParam("influx_port", true)->value().toInt();
  if (request->hasParam("influx_db", true))   strncpy(influxDBDatabase, request->getParam("influx_db",   true)->value().c_str(), sizeof(influxDBDatabase) - 1);
  if (request->hasParam("ntp_server", true))  strncpy(ntpServer, request->getParam("ntp_server", true)->value().c_str(), sizeof(ntpServer) - 1);
  if (request->hasParam("latitude", true))    latitude  = request->getParam("latitude",  true)->value().toDouble();
  if (request->hasParam("longitude", true))   longitude = request->getParam("longitude", true)->value().toDouble();
  if (request->hasParam("tz_offset", true))   tzOffset  = request->getParam("tz_offset", true)->value().toInt();
  if (request->hasParam("tz_sec", true))      gmtOffset_sec = request->getParam("tz_sec", true)->value().toInt();
  if (request->hasParam("tCorr", true))       tCorr = request->getParam("tCorr", true)->value().toFloat();
  if (request->hasParam("altitude_m", true)) {
    altitude_m = request->getParam("altitude_m", true)->value().toFloat();
    cond.setH((int)altitude_m);
  }

  settingsSaveAll();
  ESP_LOGI("SETTINGS", "Настройки обновлены из веб-интерфейса");
  request->send(200, "text/plain", "Settings saved. Reboot to apply.");
}

void handleGetTasksState(AsyncWebServerRequest *request) {
  char jsonBuffer[256];
  buildTaskStateJson(jsonBuffer, sizeof(jsonBuffer));
  request->send(200, "application/json", jsonBuffer);
}

void sendTimeData() {
  if (webSocket1.count() == 0) return;

  time_t now = time(nullptr);

  StaticJsonDocument<256> json;
  json["nowTime"]      = (double)now;
  json["sunriseTime"]  = sunriseTime * 60;
  json["sunsetTime"]   = sunsetTime  * 60;
  json["sunElevation"] = calcSunElevation(latitude, longitude, now);
  json["solarNoon"]    = calcSolarNoon(longitude, now);

  char jsonBuffer[320];
  size_t len = serializeJson(json, jsonBuffer, sizeof(jsonBuffer));
  if (len >= sizeof(jsonBuffer) - 1) {
    ESP_LOGE("WS", "JSON-буфер времени переполнен!");
    return;
  }
  webSocket1.textAll(jsonBuffer, len);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    IPAddress rip = client->remoteIP();
    ESP_LOGI("WS", "Клиент #%u подключён (%d.%d.%d.%d)", client->id(),
             rip[0], rip[1], rip[2], rip[3]);
  } else if (type == WS_EVT_DISCONNECT) {
    ESP_LOGI("WS", "Клиент #%u отключён", client->id());
  }
}

void onWsEvent1(AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    ESP_LOGI("WS1", "Клиент подключён к /ws1");
  } else if (type == WS_EVT_DATA) {
    if (strncmp((char *)data, "getTime", len) == 0) {
      sendTimeData();
    }
  }
}

// ── Task switchers (called from web and Nextion) ──────────────

#define TASK_SWITCH_BODY(handle, createFn, name, stackSize, prio)   \
  if (isTaskActive(handle)) {                                         \
    vTaskSuspend(handle);                                             \
    ESP_LOGD("SYS", "%s: остановлена", name);                        \
  } else {                                                            \
    if (handle == NULL) {                                             \
      xTaskCreate(createFn, name, stackSize, NULL, prio, &handle);   \
      ESP_LOGD("SYS", "%s: создана", name);                          \
    } else {                                                          \
      vTaskResume(handle);                                            \
      ESP_LOGD("SYS", "%s: возобновлена", name);                     \
    }                                                                 \
  }                                                                   \
  sendTaskStateUpdate();

void switchTaskNRF905() {
  TASK_SWITCH_BODY(taskNRF905Handle, taskNRF905, "NRF905 Receiver", 4096, 5)
}

void switchTaskBMP280() {
  TASK_SWITCH_BODY(taskBMP280Handle, taskBMP280, "BMP280 Sensor", 2048, 4)
}

void switchTaskForecaster() {
  TASK_SWITCH_BODY(taskForecasterHandle, taskForecast, "Forecast task", 2048, 1)
}

void switchTaskNTP() {
  TASK_SWITCH_BODY(taskGetTimeHandle, taskGetTime, "Get NTP Time", 4096, 2)
}

void switchTaskInfluxDB() {
  TASK_SWITCH_BODY(taskSendDataToInfluxDBHandle, taskSendDataToInfluxDB, "InfluxDBTask", 4096, 6)
}

void switchTaskCO2Read() {
  if (isTaskActive(taskCO2ReadHandle)) {
    mh19.end();
    vTaskSuspend(taskCO2ReadHandle);
    ESP_LOGD("SYS", "CO2 Task: остановлена");
  } else {
    mh19.begin(9600, SERIAL_8N1, RX1, TX1);
    if (taskCO2ReadHandle == NULL) {
      xTaskCreate(taskCO2Read, "CO2 read task", 2048, NULL, 3, &taskCO2ReadHandle);
      ESP_LOGD("SYS", "CO2 Task: создана");
    } else {
      vTaskResume(taskCO2ReadHandle);
      ESP_LOGD("SYS", "CO2 Task: возобновлена");
    }
  }
  sendTaskStateUpdate();
}

void switchTaskNextion() {
  if (isTaskActive(processNextionTaskHandle)) {
    nextionSleep();
    vTaskDelay(pdMS_TO_TICKS(200));
    vTaskSuspend(processNextionTaskHandle);
    ESP_LOGD("SYS", "Nextion Task: остановлена");
  } else {
    if (processNextionTaskHandle == NULL) {
      xTaskCreate(processNextionTask, "Nextion", 4096, NULL, 3, &processNextionTaskHandle);
      ESP_LOGD("SYS", "Nextion Task: создана");
    } else {
      vTaskResume(processNextionTaskHandle);
      nextionWakeUP();
      ESP_LOGD("SYS", "Nextion Task: возобновлена");
    }
  }
  sendTaskStateUpdate();
}

void switchTaskTVOCRead() {
  if (isTaskActive(taskTVOCReadHandle)) {
    checkMutex();
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      ens160.setOperatingMode(SFE_ENS160_DEEP_SLEEP);
      xSemaphoreGive(i2cMutex);
    }
    vTaskSuspend(taskTVOCReadHandle);
    ESP_LOGD("SYS", "TVOC Task: остановлена");
  } else {
    if (taskTVOCReadHandle == NULL) {
      if (!ens160.begin()) ESP_LOGE("SYS", "ENS160 не найден!");
      if (!aht20.begin())  ESP_LOGE("SYS", "AHT20 не найден!");
      xTaskCreate(taskTVOCRead, "ENS160 read task", 4096, NULL, 2, &taskTVOCReadHandle);
      ESP_LOGD("SYS", "TVOC Task: создана");
    } else {
      checkMutex();
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        ens160.setOperatingMode(SFE_ENS160_RESET);
        vTaskDelay(pdMS_TO_TICKS(100));
        ens160.setOperatingMode(SFE_ENS160_STANDARD);
        xSemaphoreGive(i2cMutex);
      }
      vTaskResume(taskTVOCReadHandle);
      ESP_LOGD("SYS", "TVOC Task: возобновлена");
    }
  }
  sendTaskStateUpdate();
}

void handleSendCommand(AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) return;

    if (!request->hasParam("cmd", true)) {
        request->send(400, "text/plain", "Параметр cmd не указан");
        return;
    }

    String cmdStr = request->getParam("cmd", true)->value();
    if (cmdStr.length() == 0 || cmdStr.length() >= NRF905_CMD_LEN) {
        request->send(400, "text/plain", "Недопустимая длина команды");
        return;
    }

    if (cmdStr != "HEATER" && cmdStr != "NRF_REST" && cmdStr != "REST") {
        String resp = "Неизвестная команда: ";
        resp.concat(cmdStr);
        request->send(400, "text/plain", resp);
        return;
    }

    char cmd[NRF905_CMD_LEN] = {};
    strncpy(cmd, cmdStr.c_str(), NRF905_CMD_LEN - 1);

    if (xQueueSend(nrf905CmdQueue, cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
        ESP_LOGI("NRF905_TX", "Команда поставлена в очередь: %s", cmd);
        request->send(200, "text/plain", cmdStr);
    } else {
        ESP_LOGW("NRF905_TX", "Очередь переполнена, команда отброшена: %s", cmd);
        request->send(503, "text/plain", "Очередь переполнена, попробуйте позже");
    }
}

void handleTaskControl(AsyncWebServerRequest *request) {
  if (!request->hasParam("task", true)) {
    request->send(400, "text/plain", "Missing task parameter");
    return;
  }

  String task = request->getParam("task", true)->value();
  ESP_LOGD("SYS", "Запрос переключения задачи: %s", task.c_str());

  if      (task == "nRF905")    switchTaskNRF905();
  else if (task == "CO2")       switchTaskCO2Read();
  else if (task == "nextion")   switchTaskNextion();
  else if (task == "BMP280")    switchTaskBMP280();
  else if (task == "InfluxDB")  switchTaskInfluxDB();
  else if (task == "Forecaster") switchTaskForecaster();
  else if (task == "NTP")       switchTaskNTP();
  else if (task == "TVOC")      switchTaskTVOCRead();
  else {
    request->send(400, "text/plain", "Unknown task");
    return;
  }

  sendTaskStateUpdate();
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

void setupWebServer() {
  server.serveStatic("/", LittleFS, "/");
  server.on("/",          HTTP_ANY, [](AsyncWebServerRequest *r){ handleRoot(r); });
  server.on("/admin",     HTTP_POST, [](AsyncWebServerRequest *r){ handleAdmin(r); });
  server.on("/about",     HTTP_POST, [](AsyncWebServerRequest *r){ handleAbout(r); });
  server.on("/updateform",HTTP_POST, [](AsyncWebServerRequest *r){ handleUpdateForm(r); });
  server.on("/update",    HTTP_POST, handleUpdateEnd, handleUpdateUpload);
  server.onFileUpload(handleUpdateUpload);
  server.on("/restart",       HTTP_POST, handleRestart);
  server.on("/sendCommand", HTTP_POST, handleSendCommand);
  server.on("/graph-data",    HTTP_GET,  [](AsyncWebServerRequest *r){ handleGraphData(r); });
  server.on("/getTasksState", HTTP_GET,  [](AsyncWebServerRequest *r){ handleGetTasksState(r); });
  server.on("/toggleTask",    HTTP_ANY,  handleTaskControl);
  server.on("/sysinfo",       HTTP_GET,  handleSysInfo);
  server.on("/bmeinfo",       HTTP_GET,  handleBMEInfo);
  server.on("/nrf905Status",  HTTP_GET,  handlenRFInfo);
  server.on("/setNRF905",     HTTP_ANY,  handleSetNRF905);
  server.on("/nrfreset",      HTTP_POST, handleNRFReset);
  server.on("/resetI2C",      HTTP_POST, handleResetI2CBus);
  server.on("/resetNVS",      HTTP_POST, handleResetNVS);
  server.on("/getSettings",   HTTP_GET,  handleGetSettings);
  server.on("/setSettings",   HTTP_POST, handleSetSettings);
  server.addHandler(&webSocket);
  server.addHandler(&webSocket1);
  webSocket.onEvent(onWsEvent);
  webSocket1.onEvent(onWsEvent1);
  server.begin();
}
