#pragma once
#include <ESPAsyncWebServer.h>

void handleUpdateUpload(AsyncWebServerRequest *request, String filename,
                        size_t index, uint8_t *data, size_t len, bool final);
void handleUpdateEnd(AsyncWebServerRequest *request);
