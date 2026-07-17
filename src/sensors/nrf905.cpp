#include "nrf905.h"
#include "../state.h"
#include "../config.h"
#include <board_config.h>
#include <RH_NRF905.h>
#include "hamming_secded.h"
#include "block_interleave.h"

extern RH_NRF905 driver;

static uint8_t calcChecksum(const uint8_t *buf, size_t len) {
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) cs ^= buf[i];
    return cs;
}

float calculateDewPoint(float temp, float hum) {
  const float a = 17.27f, b = 237.7f;
  float alpha = ((a * temp) / (b + temp)) + logf(hum / 100.0f);
  return (b * alpha) / (a - alpha);
}

void resetNRF905() {
  ESP_LOGW("NRF905", "Сброс nRF905...");
  digitalWrite(NRF905_PWR_UP_PIN, LOW);
  vTaskDelay(pdMS_TO_TICKS(200));
  digitalWrite(NRF905_PWR_UP_PIN, HIGH);
  nRF905ResetCount++;
  vTaskDelay(pdMS_TO_TICKS(100));

  if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
    if (driver.init()) {
      driver.setChannel(175, false);
      driver.setRF(RH_NRF905::TransmitPower10dBm);
      ESP_LOGW("NRF905", "nRF905 переинициализирован (сброс #%u)", nRF905ResetCount);
    } else {
      ESP_LOGE("NRF905", "Ошибка переинициализации nRF905!");
    }
    xSemaphoreGive(driverMutex);
  }
}

void queueStmCommand(const char* cmd) {
    char buf[NRF905_CMD_LEN] = {};
    strncpy(buf, cmd, NRF905_CMD_LEN - 1);
    xQueueSend(nrf905CmdQueue, buf, pdMS_TO_TICKS(500));
}

void taskNRF905(void *pvParameters) {
  unsigned long lastReceived        = millis();
  const uint8_t EXPECTED_LEN        = HAMMING_PACKET_SIZE;  // 26
  uint8_t       last_burst_id       = 0xFF;
  uint8_t       recoveryState       = 0;

  while (true) {
#ifdef ESP32S3
    nrf905DataReady = false;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
#endif

    if (xSemaphoreTake(driverMutex, portMAX_DELAY) == pdTRUE) {
      if (driver.available()) {
        uint8_t buf[RH_NRF905_MAX_MESSAGE_LEN];
        uint8_t len = sizeof(buf);

        if (driver.recv(buf, &len)) {
          if (len != EXPECTED_LEN) {
            ESP_LOGW("NRF905", "Неверная длина пакета: ожидалось %d, получено %d",
                     EXPECTED_LEN, len);
            nrf905RxFail++;
            xSemaphoreGive(driverMutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
          }

          // Проверка XOR-CRC
          uint8_t crc_calc = 0;
          for (uint8_t i = 0; i < len - 1; i++) crc_calc ^= buf[i];
          if (buf[len - 1] != crc_calc) {
            ESP_LOGW("NRF905", "Ошибка CRC");
            nrf905RxFail++;
            xSemaphoreGive(driverMutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
          }

          // Дедупликация по burst_id
          uint8_t burst_id = buf[0];
          if (burst_id == last_burst_id) {
            ESP_LOGD("NRF905", "Дубликат burst (id=%u) — пропущен", burst_id);
            xSemaphoreGive(driverMutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
          }
          last_burst_id = burst_id;

          // Депемежение: 24 байта → восстановление порядка Hamming-групп
          uint8_t coded[HAMMING_CODED_SIZE];
          memcpy(coded, buf + 1, HAMMING_CODED_SIZE);
          block_interleave(coded);

          // Декодирование Хэмминга SECDED: 24 байта → 16 байт данных
          uint8_t decoded[HAMMING_DATA_SIZE];
          int corrected = hamming_decode(coded, decoded);

          if (corrected < 0) {
            ESP_LOGW("NRF905", "Хэмминг: неисправимая ошибка (>1 бита в байте)");
            nrf905RxFail++;
            xSemaphoreGive(driverMutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
          }
          if (corrected > 0) {
            ESP_LOGI("NRF905", "Хэмминг: исправлено %d бит", corrected);
            nrf905RxCorrected++;
          } else {
            nrf905RxOK++;
          }

          // Парсинг декодированных данных
          const uint8_t *p = decoded;
          uint8_t  rawHeater = *p++;
          uint8_t  rawFan    = *p++;

          int16_t  rawT   = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));  p += 2;
          uint16_t rawH   = (uint16_t)p[0] | ((uint16_t)p[1] << 8);              p += 2;
          uint16_t rawUV  = (uint16_t)p[0] | ((uint16_t)p[1] << 8);              p += 2;
          uint32_t rawLux = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                          | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);     p += 4;
          uint16_t rawPM25 = (uint16_t)p[0] | ((uint16_t)p[1] << 8);            p += 2;
          uint16_t rawPM10 = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

          if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            heaterStatus = rawHeater;
            fanStatus    = rawFan;
            temperature  = rawT    / 100.0f;
            humidity     = rawH    / 100.0f;
            uvIndex      = rawUV   / 100.0f;
            luxLevel     = rawLux  / 100.0f;
            pm25Level    = rawPM25 / 10.0f;
            pm10Level    = rawPM10 / 10.0f;
            dewPoint     = calculateDewPoint(temperature, humidity);
            xSemaphoreGive(dataMutex);
          }

          lastReceived = millis();
          if (recoveryState > 0) {
            ESP_LOGI("NRF905", "Связь восстановлена (состояние %d → 0)", recoveryState);
            recoveryState = 0;
          }
          ESP_LOGI("NRF905",
                   "OK [burst=%u] HEAT=%u FAN=%u T=%.2f H=%.2f "
                   "UV=%.2f LUX=%.1f PM2.5=%.1f PM10=%.1f",
                   burst_id, heaterStatus, fanStatus,
                   temperature, humidity, uvIndex,
                   luxLevel, pm25Level, pm10Level);
        }
      }
      xSemaphoreGive(driverMutex);
    }

    if (millis() - lastReceived >= 300000UL) {
      recoveryState++;

      if (recoveryState <= 2) {
        ESP_LOGE("NRF905", "Нет данных >%d мин, NRF_REST (попытка %d)...",
                 recoveryState * 5, recoveryState);
        resetNRF905();
        char cmd[] = "NRF_REST";
        xQueueSend(nrf905CmdQueue, cmd, pdMS_TO_TICKS(500));
      } else {
        ESP_LOGE("NRF905", "NRF_REST не помог, отправляю REST...");
        char cmd[] = "REST";
        xQueueSend(nrf905CmdQueue, cmd, pdMS_TO_TICKS(500));
        recoveryState = 0;
      }

      lastReceived = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static uint8_t cmdStringToId(const char *cmd) {
    if (strcmp(cmd, "HEATER")   == 0) return CMD_HEATER;
    if (strcmp(cmd, "NRF_REST") == 0) return CMD_NRF_REST;
    if (strcmp(cmd, "REST")     == 0) return CMD_REST;
    return 0;
}

void taskNRF905Tx(void *pvParameters) {
    char cmd[NRF905_CMD_LEN];

    while (true) {
        if (xQueueReceive(nrf905CmdQueue, cmd, portMAX_DELAY) != pdTRUE) continue;

        uint8_t cmd_id = cmdStringToId(cmd);
        if (cmd_id == 0) {
            ESP_LOGE("NRF905_TX", "Неизвестная команда: %s", cmd);
            continue;
        }

        ESP_LOGI("NRF905_TX", "Отправка команды: %s (id=0x%02X)", cmd, cmd_id);

        if (xSemaphoreTake(driverMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            // 1. Собираем 16-байтный payload
            uint8_t payload[HAMMING_DATA_SIZE];
            memset(payload, 0, HAMMING_DATA_SIZE);
            payload[0] = cmd_id;

            // 2. Hamming encode: 16 → 24 байта
            uint8_t coded[HAMMING_CODED_SIZE];
            hamming_encode(payload, coded);

            // 3. Сборка пакета (26 байт)
            uint8_t buf[HAMMING_PACKET_SIZE];
            buf[0] = CMD_BURST_ID_BASE + cmd_id;
            memcpy(buf + 1, coded, HAMMING_CODED_SIZE);
            buf[1 + HAMMING_CODED_SIZE] = calcChecksum(buf, 1 + HAMMING_CODED_SIZE);

            // 5. Burst x6
            for (uint8_t i = 0; i < BURST_COUNT; i++) {
                driver.send(buf, HAMMING_PACKET_SIZE);
                driver.waitPacketSent();
                if (i < BURST_COUNT - 1) {
                    vTaskDelay(pdMS_TO_TICKS(BURST_PAUSE_MS));
                }
            }

            xSemaphoreGive(driverMutex);
            ESP_LOGI("NRF905_TX", "Команда '%s' отправлена (burst x%d)", cmd, BURST_COUNT);
        } else {
            ESP_LOGE("NRF905_TX", "Таймаут driverMutex! Команда '%s' потеряна", cmd);
        }
    }
}
