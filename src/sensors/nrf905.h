#pragma once

float calculateDewPoint(float temp, float hum);
void resetNRF905();
void queueStmCommand(const char* cmd);
void taskNRF905(void *pvParameters);
void taskNRF905Tx(void *pvParameters);
