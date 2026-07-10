#include "wifi.h"
#include "../state.h"
#include "../config.h"
#include <WiFi.h>
#include <esp_wifi.h>

void applyWiFiConfig() {
    if (useStaticIP && staticIP[0] != '\0') {
        IPAddress ip, gw, sn, dns;
        ip.fromString(staticIP);
        gw.fromString(staticGateway);
        sn.fromString(staticSubnet);
        dns.fromString(staticDNS);
        WiFi.config(ip, gw, sn, dns);
        ESP_LOGI("WIFI", "Static IP: %s", staticIP);
    } else {
        ESP_LOGI("WIFI", "Using DHCP");
    }
}

void reconnectWiFi() {
  uint32_t cooldown = (wifi_attempts > 0 && (wifi_attempts % MAX_ATTEMPTS_PER_CYCLE) == 0)
                          ? LONG_COOLDOWN
                          : SHORT_COOLDOWN;

  if (last_reconnect_time != 0 && (millis() - last_reconnect_time < cooldown)) return;

  wifi_attempts++;
  last_reconnect_time = millis();
  ESP_LOGW("WIFI", "Попытка переподключения %d/%d",
           wifi_attempts, MAX_ATTEMPTS_PER_CYCLE * MAX_CYCLES);

  WiFi.disconnect(true);
  vTaskDelay(pdMS_TO_TICKS(100));
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  applyWiFiConfig();
  esp_wifi_set_ps(WIFI_PS_NONE);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_attempts = 0;
    IPAddress ip = WiFi.localIP();
    ESP_LOGI("WIFI", "Переподключение успешно. RSSI: %d IP: %d.%d.%d.%d",
             WiFi.RSSI(), ip[0], ip[1], ip[2], ip[3]);
  }
}

void wifi_timer_callback(TimerHandle_t xTimer) {
  if (WiFi.status() == WL_CONNECTED) return;

  if (wifi_attempts < MAX_ATTEMPTS_PER_CYCLE * MAX_CYCLES) {
    reconnectWiFi();
    uint32_t nextInterval = (wifi_attempts > 0 && (wifi_attempts % MAX_ATTEMPTS_PER_CYCLE) == 0)
                                ? LONG_COOLDOWN : SHORT_COOLDOWN;
    xTimerChangePeriod(wifiTimer, pdMS_TO_TICKS(nextInterval), 0);
    xTimerStart(wifiTimer, 0);
  } else {
    ESP_LOGE("WIFI", "Исчерпаны %d попыток подключения. Перезагрузка...", wifi_attempts);
    ESP.restart();
  }
}

void wifi_monitor_task(void *pvParams) {
  wifiTimer = xTimerCreate("WiFiTimer", pdMS_TO_TICKS(SHORT_COOLDOWN),
                            pdFALSE, 0, wifi_timer_callback);
  if (wifiTimer == NULL) {
    ESP_LOGE("WIFI", "Не удалось создать таймер WiFi!");
    vTaskDelete(NULL);
    return;
  }

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      ESP_LOGW("WIFI", "Отключение WiFi!");
      last_reconnect_time = 0;
      UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
      if (!xTimerIsTimerActive(wifiTimer)) {
        xTimerStartFromISR(wifiTimer, &xHigherPriorityTaskWoken);
      }
      taskEXIT_CRITICAL_FROM_ISR(saved);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      xTimerStopFromISR(wifiTimer, &xHigherPriorityTaskWoken);
      wifi_attempts = 0;
      IPAddress ip = WiFi.localIP();
      ESP_LOGI("WIFI", "IP получен: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    }

    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
  });

  while (true) {
    if (WiFi.status() != WL_CONNECTED && !xTimerIsTimerActive(wifiTimer)) {
      xTimerStart(wifiTimer, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
