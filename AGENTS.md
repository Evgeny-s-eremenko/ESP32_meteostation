# ESP32 Meteostation

## Read First

- This is an Arduino/FreeRTOS ESP32 firmware with a LittleFS web UI, Nextion display, nRF905 link to an STM32 outdoor node, and optional InfluxDB. There is no MQTT.
- `platformio.ini` is the source of truth for build environments and pinned libraries.
- `src/` is firmware; `data/` is the LittleFS web application; `.opencode/` is project agent infrastructure; `.mcp/` contains project MCP files.
- Do not read all of `README.MD` for basic project context. Read only the relevant section when human-oriented detail is needed.

## Project Tree

```text
.
├── .mcp/
│   └── meteostation/
│       ├── .env                  # local only, never commit
│       ├── .env.example
│       ├── README.md
│       ├── requirements.txt
│       └── server.py
├── .opencode/
│   ├── agents/
│   │   ├── esp32.md
│   │   └── planner.md
│   ├── commands/
│   │   ├── plan.md
│   │   └── verify.md
│   ├── plans/
│   │   └── agent-infrastructure-migration.md
│   └── skills/
│       └── embedded-systems/
│           ├── SKILL.md
│           └── references/
│               ├── communication-protocols.md
│               ├── memory-optimization.md
│               ├── microcontroller-programming.md
│               ├── power-optimization.md
│               └── rtos-patterns.md
├── data/
│   ├── index.html
│   ├── mobile.html
│   ├── admin.html
│   ├── about.html
│   ├── updateform.html
│   ├── script.js
│   ├── restart.js
│   ├── update.js
│   ├── sunset.js
│   ├── d3-local-maps.js
│   ├── favicon.ico
│   └── *.png                  # UI icons and images
├── include/
│   ├── board_config.h
│   ├── block_interleave.h
│   ├── hamming_secded.h
│   └── README
├── lib/
│   └── Forecaster/
├── Nextion display project/
│   ├── Meteostation.HMI
│   ├── Meteostation.tft
│   └── Preview.png
├── PCB_EasyEDA/
├── scripts/
│   └── verify-agent-structure.ps1
├── src/
│   ├── main.cpp
│   ├── config.h
│   ├── state.h
│   ├── display/nextion.cpp/.h
│   ├── forecast/forecast.cpp/.h
│   ├── network/influxdb.cpp/.h
│   ├── network/time_sync.cpp/.h
│   ├── network/wifi.cpp/.h
│   ├── sensors/bme280.cpp/.h
│   ├── sensors/co2.cpp/.h
│   ├── sensors/ens160.cpp/.h
│   ├── sensors/nrf905.cpp/.h
│   ├── utils/hamming_secded.cpp
│   ├── utils/heap_monitor.cpp/.h
│   ├── utils/i2c_recovery.cpp/.h
│   ├── utils/settings.cpp/.h
│   ├── web/ota.cpp/.h
│   └── web/server.cpp/.h
├── AGENTS.md
├── README.MD
├── opencode.json
├── platformio.ini
├── secrets.h.example
└── skills-lock.json
```

Ignored runtime or generated paths include `.pio/`, `logs/`, `.playwright-mcp/`, `.mimocode/`, Python caches, local secrets, and build artifacts.

## Plans

- Any non-trivial task requires a plan before implementation.
- Non-trivial means multiple files, architecture, HTTP API, NVS, MCP, skills, agent configuration, or multiple verification steps.
- Use project command `/plan`, which runs the restricted `planner` agent.
- Store plans only in `.opencode/plans/`.
- Plan names use `YYYY-MM-DD-short-kebab-case.md`.
- Planner may edit only `.opencode/plans/*.md`; it must not edit source, configuration, documentation, MCP, skills, or Git files.
- After creating a plan, planner must print the exact path on a separate line:
  `PLAN: .opencode/plans/YYYY-MM-DD-name.md`
- There is no automatic VSCode opening and no `/open-plan` command. Open the reported path manually.
- `.mimocode/` is an unused historical artifact. Do not create new OpenCode plans there, and do not delete the directory automatically.

## OpenCode Scope

- Project MCP registration belongs in project `opencode.json`.
- Project MCP files belong in `.mcp/`.
- Global MCP registration belongs only in `C:\Users\evgen\.config\opencode\opencode.jsonc`.
- `meteostation` is project-only; `context7` is global; global `playwright` is disabled by default.
- Project skills belong in `.opencode/skills/<name>/SKILL.md`.
- `skills-lock.json` is source metadata for the copied skill, not OpenCode discovery or permissions.
- Never duplicate a skill in `.agents/skills`, `.opencode/skills`, or another project directory.
- Tracked OpenCode configs must not contain passwords, tokens, or other secrets. Local MCP secrets belong in `.mcp/meteostation/.env`.
- Use `/verify` or `scripts/verify-agent-structure.ps1` for structural checks. The checker is read-only and must not auto-fix files.

## Build And Verify

- `pio` is normally not on PATH. Use:
  `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"`
- Build affected environments:
  `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32dev`
  `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32s3`
- `esp32dev` is classic ESP32 with 4 MB flash; `esp32s3` is ESP32-S3 with 16 MB flash and OPI PSRAM.
- For web-only changes build only LittleFS: `... run -e <env> --target buildfs`.
- There are no unit tests. Do not ignore compiler warnings.
- Serial monitor is 115200 baud.

## Firmware Boundaries

- `src/main.cpp` initializes hardware, defines shared globals, and creates FreeRTOS tasks.
- `src/state.h` contains `extern` declarations; define shared variables in `main.cpp`.
- `src/web/server.cpp` owns HTTP endpoints and WebSockets. Preserve endpoint names, DOM IDs, and message formats.
- `data/` is LittleFS, not a C++ build artifact.
- `include/board_config.h` is the only GPIO assignment source. Preserve its `ESP32S3` conditional.
- `src/config.h` contains nRF905 protocol constants compatible with the STM32 transmitter.
- `lib/Forecaster` is vendored; do not modify it unless explicitly requested.
- Preserve FreeRTOS task architecture, mutexes, hardware protocols, GPIO mappings, NVS namespaces, and NVS layout unless explicitly approved.

## HTTP API

Data:

- `GET /graph-data` - measurement JSON.
- `GET /sysinfo` - chip, heap, uptime, RSSI, stack information.
- `GET /bmeinfo` - I2C sensor status.
- `GET /nrf905Status` - nRF905 status.

Control:

- `POST /toggleTask` - toggle a task.
- `GET /getTasksState` - task state JSON.
- `POST /sendCommand` - STM32 command: `HEATER`, `NRF_REST`, or `REST`.
- `POST /restart` - restart ESP32.
- `POST /nrfreset` - reset local nRF905.
- `POST /resetNVS` - factory-reset NVS.

Settings and OTA:

- `GET /getSettings`, `POST /setSettings`, `POST /setNRF905`.
- `GET /updateform`, `POST /update` for `firmware.bin` or `littlefs.bin`.

WebSockets:

- `/ws` - FreeRTOS task state JSON.
- `/ws1` - local time, sunrise, sunset, sun elevation, and solar noon.

## NVS

- Namespace `nrf905`: channel, frequency band, and transmitter power.
- Namespace `settings`: WiFi, HTTP authorization, static IP/DHCP, InfluxDB, NTP, timezone, latitude, longitude, temperature correction, and altitude.
- NVS persists independently of firmware updates.
- Exact key names are defined by `src/utils/settings.cpp` and must not be guessed.

## Source Of Truth

- Actual file structure: filesystem and Git, with this tree kept as the agent index.
- HTTP behavior: `src/web/server.cpp` and related headers.
- NVS keys and serialization: `src/utils/settings.cpp` and related headers.
- GPIO: `include/board_config.h`.
- nRF905 protocol: `src/config.h` and the STM32 reference `@stm32`.
- Build environments/dependencies: `platformio.ini`.
- Project OpenCode behavior: `opencode.json` and `.opencode/`.
- Global OpenCode behavior: `C:\Users\evgen\.config\opencode\opencode.jsonc`.
- Human-oriented detail: relevant sections of `README.MD`; it is not a replacement for source code.

## Hardware And Safety

- `src/secrets.h` is local and gitignored. Never edit, print, or commit credentials.
- Heap monitor warns below 80 KB and reboots below 64 KB.
- For a web-only change, build and upload only LittleFS.
- Mutating meteostation MCP operations require an explicit user request.
- For diagnostics, get settings first, then collect read-only graph, task, sensor, radio, system, and time data as needed.
- Do not commit, amend, push, or change remote state unless explicitly requested.
