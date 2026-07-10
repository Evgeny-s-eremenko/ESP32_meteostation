#include "heap_monitor.h"
#include "../state.h"
#include "../config.h"
#include <ESPAsyncWebServer.h>

extern AsyncWebSocket webSocket;
extern AsyncWebSocket webSocket1;

void heap_monitor_task(void *pvParameters) {
  while (true) {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();

    ESP_LOGI("HEAP", "free=%u maxAlloc=%u", freeHeap, maxAlloc);

    if (freeHeap < HEAP_CRIT_THRESHOLD) {
      ESP_LOGE("HEAP", "Критический минимум кучи (%u байт)! Перезагрузка...", freeHeap);
      vTaskDelay(pdMS_TO_TICKS(1000));
      ESP.restart();
    }

    if (freeHeap < HEAP_WARN_THRESHOLD) {
      ESP_LOGW("HEAP", "Мало свободной памяти (%u байт)", freeHeap);
    }

    // Очистка мёртвых WebSocket-клиентов
    webSocket.cleanupClients();
    webSocket1.cleanupClients();

    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}
