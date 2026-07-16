#pragma once
#include <Arduino.h>

// ── WiFi reconnect ───────────────────────────────────────────
#define MAX_ATTEMPTS_PER_CYCLE  3
#define MAX_CYCLES              3
#define SHORT_COOLDOWN          30000
#define LONG_COOLDOWN           300000

// ── nRF905 ───────────────────────────────────────────────────
#define NRF905_CMD_LEN          16
#define NRF905_CMD_QUEUE_LEN    5

// ── Command IDs (match STM32 transmitter) ────────────────────
#define CMD_HEATER              0x01
#define CMD_NRF_REST            0x02
#define CMD_REST                0x03
#define CMD_BURST_ID_BASE       0xF0
#define IS_CMD_PACKET(id)       ((id) >= CMD_BURST_ID_BASE)
#define BURST_COUNT             6
#define BURST_PAUSE_MS          5

// ── Heap thresholds ──────────────────────────────────────────
#define HEAP_WARN_THRESHOLD     81920
#define HEAP_CRIT_THRESHOLD     65536

// ── Status codes (match STM32 transmitter) ───────────────────
#define ST_NORMAL   0x01
#define ST_HEATER   0x02
#define ST_COOLING  0x03
#define ST_FAN_OFF  0x00
#define ST_FAN_ON   0x01
