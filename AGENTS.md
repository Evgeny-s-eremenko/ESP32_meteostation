# ESP32 Meteostation — AI Coding Instructions

ESP32 weather station firmware (PlatformIO / Arduino / FreeRTOS) with a LittleFS web UI, Nextion display, nRF905 radio link to an STM32 outdoor node, and InfluxDB upload. There is **no MQTT**. `README.MD` is the main project doc (in Russian, detailed: HTTP API, NVS layout, recovery logic). Code comments are also in Russian.

## Build & verification

- Two envs in `platformio.ini`: `esp32dev` (ESP32, 4 MB) and `esp32s3` (ESP32-S3 N16R8, 16 MB flash, OPI PSRAM). Build each one you touch:
  ```powershell
  & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev
  & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3
  ```
  `pio` / `platformio` are **not on PATH**; use the VSCode-extension core path above (PlatformIO Core 6.x in `~/.platformio/penv`).
- Upload: `--target upload`; web UI: `--target uploadfs` (LittleFS image built from `data/`).
- There are **no unit tests** (`test/` is an empty PlatformIO stub). Verification = a clean build for the affected env(s); fix all errors and new warnings before reporting done.
- Serial monitor is preconfigured in `platformio.ini` (115200, `log2file` filter writes to `logs/`).

## Architecture

- `src/main.cpp` — entry point; creates all FreeRTOS tasks and owns the definitions of every global.
- `src/state.h` — `extern` declarations of all shared state (sensor values, task handles, mutexes). New shared variables follow this pattern: declare in `state.h`, define in `main.cpp`.
- Cross-task data access uses `volatile` globals guarded by `i2cMutex` (I²C bus), `driverMutex` (nRF905 driver), `dataMutex`, or `mutexMux` (portMUX). Use the existing mutex when touching shared data.
- `include/board_config.h` — the only place with GPIO assignments; per-board via `#ifdef ESP32S3`. Never hardcode pins elsewhere. Note S3 strapping/flash pin restrictions are commented there.
- `src/config.h` — nRF905 protocol constants (command IDs, burst count/pause) that must stay in sync with the STM32 transmitter firmware.
- `src/web/server.cpp` — HTTP API + WebSockets (`/ws` task states, `/ws1` time/sunrise); endpoints documented in `README.MD`.
- `data/` — LittleFS web UI (served from flash; not a build artifact).
- `lib/Forecaster` — vendored local library (Zambretti forecast); do not modify unless asked.
- Radio error protection: `include/hamming_secded.h` + `src/utils/hamming_secded.cpp`, `include/block_interleave.h` + `src/utils/block_interleave.cpp` (nRF905 packet coding).
- Tasks: nRF905 RX/TX, CO2 (MH-Z19), BME280, ENS160+AHT20 (TVOC), Nextion, InfluxDB, NTP, Forecaster, WiFi monitor, Heap monitor. Settings persist in NVS (`nrf905`, `settings` namespaces via `src/utils/settings.cpp`).

## Constraints

- `src/secrets.h` is gitignored (create from `secrets.h.example`). Never modify or commit it or any credentials.
- Do not change GPIO assignments, NVS layouts, or the nRF905/STM32 wire protocol without explicit request.
- Preserve the FreeRTOS task architecture; avoid blocking calls in tasks; watch heap (HeapMonitor warns < 80 KB, reboots < 64 KB).
- Web UI: keep existing DOM ids, AJAX endpoints and WebSocket messages stable; prefer localized edits over rewriting `script.js`/HTML pages.
- Library versions are pinned in `platformio.ini` `lib_deps`; don't bump them casually.
- Smallest change wins: don't refactor or rewrite unrelated code, don't duplicate existing utilities, search the repo before assuming a feature is missing.
- Don't push to remote without explicit approval.

## Context management

- Skip `.pio/`, `logs/`, `.mimocode/`, images, and generated files unless explicitly asked.
- Start from the minimum relevant files (`state.h`, `main.cpp`, the module in question); expand only as references require.

## Diagnostics workflow

Порядок действий при диагностике станции через MCP-инструменты:

1. **Первым делом — `get_settings`**: содержит `tz_offset`/`tz_sec` (часовой пояс) и координаты — без них невозможно корректно интерпретировать время, восход/закат и солнечную высоту. Также полезен для сверки настроек WiFi/InfluxDB/NTP.
2. **Сбор данных** (параллельно): `get_graph_data`, `get_tasks_state`, `get_sensor_info`, `get_radio_status`, `get_system_info`, `get_time_data`.
3. **Оценка**:heap < 80 KB — предупреждение, < 64 KB — критично; задача в `false` — проверить, почему остановилась; `get_sensor_info` показывает `Not Found` — проблема с датчиком.
4. **CO2 мониторинг**: 400–600 ppm — норма, 600–1000 ppm — ухудшение (рекомендовать проветривание), >1000 ppm — плохо.
5. **Восстановление ENS160 при `Not Found`**: задача TVOC самоудаляется после 3 подряд ошибок I2C. Аппаратный сбой подтверждается, если BME280 на той же шине работает, а ENS160/AHT20 — нет. Алгоритм: `reset_i2c` → `toggle_task TVOC` → повторить `get_sensor_info`. Если после `restart` ENS160 по-прежнему `Not Found` — аппаратная неисправность, требуется физический осмотр модуля.

## MCP Tools (meteostation)

MCP-сервер `meteostation` предоставляет инструменты для диагностики и управления реальной метеостанцией по HTTP API. Конфигурация в `~/.config/opencode/opencode.jsonc`. Исходник сервера: `mcp_server/server.py`.

### Read-only (permission: allow, без подтверждения)

- `meteostation_get_graph_data` — метеоданные (JSON). Источники: локальные BME280/CO2, уличные STM32/nRF905 (T/H/PM/UV/LUX), TVOC от ENS160. UV=0/LUX=0 ночью — норма.
- `meteostation_get_tasks_state` — состояния FreeRTOS-задач (JSON: nRF905, CO2, nextion, BMP280, InfluxDB, Forecaster, NTP, TVOC)
- `meteostation_get_system_info` — Chip Model, Free Heap, Max Alloc, Uptime, RSSI, стек
- `meteostation_get_sensor_info` — диагностика BME280, ENS160, счётчик сбросов I2C (AHT20 не отображается)
- `meteostation_get_radio_status` — регистры nRF905, канал, частота, мощность, статистика RX/ошибок.
- `meteostation_get_time_data` — локальное время станции, восход/закат, высота солнца (через WebSocket /ws1)
- `meteostation_get_settings` — текущие настройки NVS (пароли маскируются `****`). Рекомендуется как **первый** вызов при диагностике — содержит `tz_offset`, `tz_sec`, координаты и высоту, необходимые для интерпретации данных времени и солнечных параметров.

### Mutating (permission: ask, требуют подтверждения)

- `meteostation_set_settings` — изменение настроек (WiFi, InfluxDB, NTP, координаты, статический IP...). Сохраняется в NVS, применяется после перезагрузки.
- `meteostation_toggle_task` — включение/выключение задачи FreeRTOS (nRF905, CO2, nextion, BMP280, InfluxDB, Forecaster, NTP, TVOC)
- `meteostation_send_command` — команды удалённому STM32 через nRF905:
  - `HEATER` — принудительное включение обогрева на удалённом уличном SHT31
  - `NRF_REST` — сброс модуля nRF905 на удалённом уличном блоке
  - `REST` — полная перезагрузка STM32 уличного блока
- `meteostation_set_nrf905` — настройка канала (0-255), диапазона (430/868/915 МГц), мощности. Сохраняется в NVS.
- `meteostation_reset_nrf905` — аппаратный сброс локального nRF905
- `meteostation_reset_i2c` — программный сброс шины I2C
- `meteostation_reset_nvs` — полный сброс NVS до заводских настроек
- `meteostation_restart` — перезагрузка ESP32
- `meteostation_upload_firmware` — OTA-загрузка прошивки (.bin)
- `meteostation_upload_filesystem` — OTA-загрузка LittleFS (.bin, имя содержит "littlefs")

### Когда использовать

- Диагностика проблем с датчиками, радиоканалом или памятью
- Проверка состояния станции без ручного захода в браузер
- Мониторинг FreeRTOS-задач и потребления памяти
- Восстановление аппаратных сбоев (I2C reset, перезапуск задач) и вывод о необходимости физического вмешательства
- Загрузка прошивки и LittleFS по OTA

### Когда НЕ использовать

- Достаточно прочитать исходный код (эндпоинты описаны в `src/web/server.cpp`)
- Не отправлять mutating-команды без явного запроса пользователя

### Справочник: статусы HEAT/FAN

Определения в `src/config.h`:

| Переменная | Значение | Константа | Описание |
|------------|----------|-----------|----------|
| `heaterStatus` | 1 | `ST_NORMAL` | Обогрев выключен, нормальная работа |
| `heaterStatus` | 2 | `ST_HEATER` | Обогрев включён (подогрев датчика) |
| `heaterStatus` | 3 | `ST_COOLING` | Остывание после обогрева |
| `fanStatus` | 0 | `ST_FAN_OFF` | Вентилятор выключен |
| `fanStatus` | 1 | `ST_FAN_ON` | Вентилятор включён |

При `heaterStatus` = 2 или 3 данные температуры/влажности/точки росы **не пишутся** в InfluxDB (считаются некорректными из-за нагрева датчика). Сами статусы HEAT и FAN пишутся всегда.

### Справочник: AHT20

AHT20 расположен на одном I2C-модуле с ENS160 и используется **только** для температурной компенсации ENS160 (`ens160.setTempCompensationCelsius()` / `ens160.setRHCompensationFloat()`). Значения AHT20 не становятся глобальными переменными, не отображаются на дисплее, не отправляются в InfluxDB и не доступны через HTTP API.

## Playwright (web UI verification)

MCP-сервер `playwright` подключён для визуальной проверки веб-интерфейса метеостанции.

### Назначение

- Открывать страницы веб-интерфейста по IP станции (`http://<ip>`)
- Делать скриншоты для визуальной диагностики UI
- Проверять корректность отображения данных, кнопок, графиков

### Правила

- **Скриншоты** всегда сохранять в `.playwright-mcp/screenshots/` (каталог добавлен в `.gitignore`)
- Перед началом работы получать IP станции через `get_system_info` или `get_settings`
- Использовать `navigate → snapshot → screenshot` workflow
- Для проверки отдельных элементов использовать `snapshot` (text-based) вместо скриншота

### Пример использования

```
1. meteostation_get_system_info → получить IP (например, 192.168.1.230)
2. playwright_browser_navigate → http://192.168.1.230
3. playwright_browser_snapshot → проверить данные в DOM
4. playwright_browser_take_screenshot → сохранить в .playwright-mcp/screenshots/
```
