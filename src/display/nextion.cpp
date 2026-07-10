#include "nextion.h"
#include "../state.h"
#include "../config.h"
#include "../sensors/nrf905.h"
#include "../utils/settings.h"
#include "../utils/i2c_recovery.h"
#include <WiFi.h>

extern bool isTaskActive(TaskHandle_t taskHandle);
extern void switchTaskNRF905();
extern void switchTaskCO2Read();
extern void switchTaskNextion();
extern void switchTaskBMP280();
extern void switchTaskInfluxDB();
extern void switchTaskForecaster();
extern void switchTaskNTP();
extern void switchTaskTVOCRead();
extern void sendTaskStateUpdate();
extern void handleRestartFromNextion();

void nextionFin() {
  nextion.write(0xFF);
  nextion.write(0xFF);
  nextion.write(0xFF);
}

void nextionWakeUP()  { nextion.print("sleep=0"); nextionFin(); }
void nextionSleep()   { nextion.print("sleep=1"); nextionFin(); }
void nextionRestart() { nextion.print("rest");    nextionFin(); }

void syncButtonState(int buttonId, TaskHandle_t taskHandle) {
  nextion.printf("bt%d.val=%d", buttonId, isTaskActive(taskHandle) ? 1 : 0);
  nextionFin();
}

void sendPage0Data() {
  nextion.printf("t0.txt=\"%.1f%%\"", humidity);                            nextionFin();
  nextion.printf("t1.txt=\"%.1f\xc2\xb0\x43\"", temperature);              nextionFin();
  nextion.printf("t3.txt=\"%.1f\xc2\xb0\x43\"", dewPoint);                 nextionFin();
  nextion.printf("t4.txt=\"%.1f ug/m3\"", pm25Level);                       nextionFin();
  nextion.printf("t2.txt=\"%.1f hPa\"", pressure);                          nextionFin();

  int pic;
  if      (forecast < 2.0f)  pic = 5;
  else if (forecast < 4.5f)  pic = 3;
  else if (forecast < 7.0f)  pic = 2;
  else                        pic = 4;
  nextion.printf("p0.pic=%d", pic); nextionFin();
}

void sendPage1Data() {
  nextion.printf("t0.txt=\"%.1f%%\"", homeHum);                              nextionFin();
  nextion.printf("t1.txt=\"%.1f\xc2\xb0\x43\"", homeTemp);                  nextionFin();
  nextion.printf("t3.txt=\"%.1f\xc2\xb0\x43\"", homeDP);                    nextionFin();
  nextion.printf("t2.txt=\"%.1f hPa\"", pressure);                           nextionFin();
  nextion.printf("t4.txt=\"%d ppm\"", ppm);                                  nextionFin();
  nextion.printf("t5.txt=\"%d ppb\"", TVOC);                                 nextionFin();
}

void sendPage2Data() {
  syncButtonState(1, taskNRF905Handle);
  syncButtonState(2, taskCO2ReadHandle);
  syncButtonState(3, processNextionTaskHandle);
  syncButtonState(4, taskBMP280Handle);
  syncButtonState(5, taskSendDataToInfluxDBHandle);
  syncButtonState(6, taskForecasterHandle);
  syncButtonState(7, taskGetTimeHandle);
  syncButtonState(0, taskTVOCReadHandle);
}

void sendPage3Data() {
  char info[256];
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    unsigned long sec = millis() / 1000;
    unsigned long h = sec / 3600;
    unsigned long m = (sec % 3600) / 60;
    snprintf(info, sizeof(info),
      "WiFi: Connected\r\n"
      "SSID: %s\r\n"
      "IP: %d.%d.%d.%d\r\n"
      "RSSI: %d dBm\r\n"
      "Uptime: %luh %lum\r\n"
      "Free Heap: %u KB\r\n"
      "Max Alloc: %u KB",
      WiFi.SSID().c_str(),
      ip[0], ip[1], ip[2], ip[3],
      WiFi.RSSI(),
      h, m,
      ESP.getFreeHeap() / 1024,
      ESP.getMaxAllocHeap() / 1024);
  } else {
    snprintf(info, sizeof(info),
      "WiFi: Disconnected\r\n"
      "Free Heap: %u KB\r\n"
      "Max Alloc: %u KB",
      ESP.getFreeHeap() / 1024,
      ESP.getMaxAllocHeap() / 1024);
  }
  nextion.printf("t2.txt=\"%s\"", info);
  nextionFin();
}

void processNextionMessageBinary(const uint8_t *msg, size_t len) {
  if (len < 5) return;
  if (!(msg[len-1] == 0xFF && msg[len-2] == 0xFF && msg[len-3] == 0xFF)) return;

  if (msg[0] == 0x66) {
    switch (msg[1]) {
      case 0x00: currentPage = PAGE0; break;
      case 0x01: currentPage = PAGE1; break;
      case 0x02: currentPage = PAGE2; break;
      case 0x03: currentPage = PAGE3; break;
      default:   currentPage = PAGE_UNKNOWN; break;
    }
    ESP_LOGV("NEXTION", "Страница: %d", currentPage);
  } else if (msg[0] == 0x65) {
    uint8_t page  = msg[1];
    uint8_t compID = msg[2];

    if (page == 0x02) {
      switch (compID) {
        case 0x03: handleRestartFromNextion(); break;
        case 0x04: resetNRF905();              break;
        case 0x05: resetNVS();                  break;
        case 0x06: switchTaskTVOCRead();   syncButtonState(0, taskTVOCReadHandle);           break;
        case 0x07: switchTaskNRF905();     syncButtonState(1, taskNRF905Handle);             break;
        case 0x08: switchTaskCO2Read();    syncButtonState(2, taskCO2ReadHandle);            break;
        case 0x09: switchTaskNextion();    syncButtonState(3, processNextionTaskHandle);     break;
        case 0x0A: switchTaskBMP280();     syncButtonState(4, taskBMP280Handle);             break;
        case 0x0B: switchTaskInfluxDB();   syncButtonState(5, taskSendDataToInfluxDBHandle); break;
        case 0x0C: switchTaskForecaster(); syncButtonState(6, taskForecasterHandle);         break;
        case 0x0D: switchTaskNTP();        syncButtonState(7, taskGetTimeHandle);            break;
        case 0x0F: queueStmCommand("HEATER");  ESP_LOGI("NEXTION", "HEATER → STM32");  break;
        case 0x10: queueStmCommand("NRF_REST"); ESP_LOGI("NEXTION", "NRF_REST → STM32"); break;
        case 0x11: queueStmCommand("REST");     ESP_LOGI("NEXTION", "REST → STM32");     break;
      }
    } else if (page == 0x03 && compID == 0x03) {
      wifiCfgState = WIFI_CFG_WAIT_SSID;
      ESP_LOGI("NEXTION", "WiFi config: ожидание SSID");
    }
  } else if (msg[0] == 0x70 && wifiCfgState != WIFI_CFG_IDLE) {
    size_t strLen = len - 4;
    if (strLen >= sizeof(wifiCfgSSID)) strLen = sizeof(wifiCfgSSID) - 1;

    if (wifiCfgState == WIFI_CFG_WAIT_SSID) {
      memcpy(wifiCfgSSID, &msg[1], strLen);
      wifiCfgSSID[strLen] = '\0';
      wifiCfgState = WIFI_CFG_WAIT_PASS;
      ESP_LOGI("NEXTION", "WiFi SSID: %s", wifiCfgSSID);
    } else if (wifiCfgState == WIFI_CFG_WAIT_PASS) {
      memcpy(wifiCfgPass, &msg[1], strLen);
      wifiCfgPass[strLen] = '\0';
      wifiCfgState = WIFI_CFG_IDLE;
      ESP_LOGI("NEXTION", "WiFi: пробуем подключиться к %s", wifiCfgSSID);

      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(wifiCfgSSID, wifiCfgPass);

      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
      }

      if (WiFi.status() == WL_CONNECTED) {
        strncpy(ssid, wifiCfgSSID, sizeof(ssid) - 1);
        strncpy(password, wifiCfgPass, sizeof(password) - 1);
        settingsSaveAll();
        ESP_LOGI("NEXTION", "WiFi подключён! IP: %s", WiFi.localIP().toString().c_str());
        wifi_attempts = 0;
      } else {
        ESP_LOGW("NEXTION", "WiFi: не удалось подключиться к %s", wifiCfgSSID);
      }
      memset(wifiCfgSSID, 0, sizeof(wifiCfgSSID));
      memset(wifiCfgPass, 0, sizeof(wifiCfgPass));
    }
  }
}

void processNextionTask(void *parameter) {
  const size_t bufSize = 32;
  uint8_t      buffer[bufSize];
  size_t       bufIndex     = 0;
  unsigned long lastUpdate  = millis();
  unsigned long lastPage3Update = millis();

  for (;;) {
    while (nextion.available()) {
      uint8_t b = nextion.read();
      if (bufIndex < bufSize) buffer[bufIndex++] = b;

      if (bufIndex >= 3 &&
          buffer[bufIndex-1] == 0xFF &&
          buffer[bufIndex-2] == 0xFF &&
          buffer[bufIndex-3] == 0xFF) {
        processNextionMessageBinary(buffer, bufIndex);
        bufIndex = 0;
      }
      if (bufIndex >= bufSize) bufIndex = 0;
    }

    if (millis() - lastUpdate > 1000) {
      switch (currentPage) {
        case PAGE0: sendPage0Data(); break;
        case PAGE1: sendPage1Data(); break;
        case PAGE2: sendPage2Data(); break;
        default: break;
      }
      lastUpdate = millis();
    }

    if (currentPage == PAGE3 && millis() - lastPage3Update > 2000) {
      sendPage3Data();
      lastPage3Update = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
