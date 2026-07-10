#include "ota.h"
#include "../state.h"
#include <Update.h>

void handleUpdateUpload(AsyncWebServerRequest *request, String filename,
                        size_t index, uint8_t *data, size_t len, bool final) {
  if (!request->authenticate(http_username, http_password)) {
    request->send(401, "text/plain", "Unauthorized");
    return;
  }

  if (index == 0) {
    ESP_LOGW("OTA", "Начало обновления: %s", filename.c_str());
    int updateType = (filename.indexOf("littlefs") >= 0) ? U_SPIFFS : U_FLASH;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateType)) {
      Update.printError(Serial);
      request->send(500, "text/plain", "Failed to start update");
      return;
    }
  }

  if (len > 0 && Update.write(data, len) != len) {
    Update.printError(Serial);
    request->send(500, "text/plain", "Write error");
    return;
  }

  if (final) {
    if (!Update.end(true)) {
      Update.printError(Serial);
      request->send(500, "text/plain", "Update failed");
      return;
    }
    ESP_LOGW("OTA", "Обновление завершено: %u байт", index + len);
  }
}

void handleUpdateEnd(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
  if (!Update.hasError()) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP.restart();
  }
}
