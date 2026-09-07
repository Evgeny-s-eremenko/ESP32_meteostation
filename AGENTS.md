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
