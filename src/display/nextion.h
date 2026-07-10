#pragma once
#include <Arduino.h>

void nextionFin();
void nextionWakeUP();
void nextionSleep();
void nextionRestart();
void syncButtonState(int buttonId, TaskHandle_t taskHandle);
void processNextionTask(void *parameter);
