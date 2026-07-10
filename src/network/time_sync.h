#pragma once
#include <Arduino.h>

double calcSunElevation(double latitude, double longitude, time_t timestamp);
double calcSolarNoon(double longitude, time_t timestamp);
void taskGetTime(void *pvParameters);
