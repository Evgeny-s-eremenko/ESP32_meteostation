#include "co2.h"
#include "../state.h"
#include "../config.h"
#include <board_config.h>

extern void sendTaskStateUpdate();

void taskCO2Read(void *pvParameters) {
  const byte cmd[9]   = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
  byte       response[9];
  const int  MAX_ERRORS = 3;
  int        errorCount = 0;
  mh19.setTimeout(1000);

  while (true) {
    mh19.write(cmd, 9);
    size_t bytesRead = mh19.readBytes(response, 9);

    if (bytesRead == 9) {
      byte checksum = 0;
      for (int i = 1; i < 8; i++) checksum += response[i];
      checksum = 0xFF - checksum + 1;

      if (response[8] == checksum) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          ppm = (256 * response[2]) + response[3];
          xSemaphoreGive(dataMutex);
        }
        ESP_LOGV("CO2", "%d ppm", (256 * response[2]) + response[3]);
        errorCount = 0;
      } else {
        errorCount++;
        ESP_LOGE("CO2", "Ошибка CRC (%d/%d)", errorCount, MAX_ERRORS);
      }
    } else {
      errorCount++;
      ESP_LOGE("CO2", "Нет ответа (%d/%d)", errorCount, MAX_ERRORS);
    }

    if (errorCount >= MAX_ERRORS) {
      ESP_LOGE("SYS", "MH-Z19 не отвечает — задача удалена");
      mh19.end();
      taskCO2ReadHandle = NULL;
      sendTaskStateUpdate();
      vTaskDelete(NULL);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
