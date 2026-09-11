# ESP32 Meteostation

## Read First

- `README.MD` is the detailed source for the HTTP API, NVS keys, recovery logic, and project layout.
- `platformio.ini` is the executable source for the two PlatformIO environments and pinned libraries.
- This is an Arduino/FreeRTOS ESP32 firmware with a LittleFS web UI, Nextion display, nRF905 link to an STM32 outdoor node, and optional InfluxDB. There is no MQTT.

## Build And Verify

- `pio` is not on PATH in the usual Windows setup. Use:
  `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"`
- Build every environment affected by a source change:
  ```powershell
  & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev
  & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3
  ```
- `esp32dev` is the classic ESP32 with 4 MB flash; `esp32s3` is `esp32-s3-devkitc-1` with 16 MB flash, OPI PSRAM, and `default_16MB.csv`.
- Build only the web filesystem with `... run -e <env> --target buildfs`; it produces `.pio/build/<env>/littlefs.bin` from `data/`.
- There are no unit tests. Verification is a clean build for each affected environment; do not ignore new compiler warnings.
- Serial monitor is 115200 baud and writes logs through the configured `log2file` filter.

## Project Boundaries

- `src/main.cpp` is the entry point, defines shared globals, initializes hardware, and creates FreeRTOS tasks.
- `src/state.h` contains `extern` declarations for shared state. Declare new shared variables there and define them in `main.cpp`.
- `src/web/server.cpp` owns HTTP endpoints and WebSockets (`/ws` task state, `/ws1` time/sun data); keep existing endpoint names, DOM IDs, and message formats stable.
- `data/` is the LittleFS web application, not a C++ build artifact. `index.html` and `mobile.html` share page behavior through files such as `sunset.js`.
- `include/board_config.h` is the only source of GPIO assignments. Select pins with the existing `ESP32S3` conditional; never hardcode them elsewhere.
- `src/config.h` contains nRF905 protocol constants that must remain compatible with the STM32 transmitter.
- `lib/Forecaster` is vendored; do not modify it unless explicitly requested.

## Cross-Project References

- The related STM32 outdoor-node project is available through the OpenCode reference `@stm32`.
- Its local path is `../STM32_Transmitter`; it is a separate Git repository and must be committed independently.
- Use `@stm32/src/...`, `@stm32/include/...`, or the STM32 project's `README.MD` when checking the radio protocol or outdoor sensor implementation.

## Safety And State

- `src/secrets.h` is local and gitignored. Create it from `secrets.h.example` when needed; never edit, print, or commit credentials.
- Do not change GPIO mappings, NVS namespaces/layouts, or the nRF905/STM32 wire protocol without explicit approval.
- Shared task data is volatile and protected by the existing `i2cMutex`, `driverMutex`, `dataMutex`, or `mutexMux` as appropriate. Preserve the FreeRTOS task architecture and avoid blocking task code.
- Heap thresholds matter: the HeapMonitor warns below 80 KB and reboots below 64 KB.
- Settings persist in NVS namespaces `settings` and `nrf905`; firmware changes do not reset them.
- Keep library versions and build flags in `platformio.ini`; do not bump dependencies casually.

## OTA And Hardware Workflow

- For a web-only change, build `littlefs.bin` and upload only the filesystem. Do not upload firmware when `src/` is unchanged.
- The station is normally reached over Wi-Fi. With the `meteostation` MCP tool, use `meteostation_meteostation_upload_filesystem` and pass a `.bin` path whose filename contains `littlefs`.
- The OTA handler restarts the ESP32 after sending the update response. The MCP upload may report `timed out` because the station reboots before the response is delivered; treat that as inconclusive, wait, then verify the station reconnects and the new UI is served.
- For a real device diagnostic, call `meteostation_meteostation_get_settings` first so timezone and coordinates are known. Then collect read-only graph, task, sensor, radio, system, and time data as needed.
- Mutating MCP operations (settings, task toggles, resets, commands, or OTA) require an explicit user request.

## Web UI Checks

- Use the Playwright workflow `navigate -> snapshot -> screenshot`; obtain the station IP from device diagnostics first.
- Prefer DOM snapshots or element evaluation for values. Save screenshots under `.playwright-mcp/screenshots/`; do not commit generated browser artifacts.
- After a filesystem OTA, include a cache-busting query when checking JavaScript/CSS and verify the station has rebooted before judging the result.

## Scope

- Skip `.pio/`, `logs/`, `.mimocode/`, images, and generated files during code search unless the task explicitly targets them.
- Make the smallest localized change; do not rewrite the web UI or refactor unrelated firmware.
- Do not commit, amend, push, or change remote state unless explicitly requested.
