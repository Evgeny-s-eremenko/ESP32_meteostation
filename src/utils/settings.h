#pragma once
#include <Arduino.h>

void settingsSaveAll();
void settingsLoadAll();
void nrf905SaveSettings(int channel, bool band, const char *power);
bool nrf905LoadSettings(int &channel, bool &band, char *power, size_t powerLen);
void resetNVS();
