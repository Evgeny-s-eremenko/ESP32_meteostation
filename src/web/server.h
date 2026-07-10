#pragma once
#include <ESPAsyncWebServer.h>

extern AsyncWebServer server;
extern AsyncWebSocket webSocket;
extern AsyncWebSocket webSocket1;

bool isAuthenticated(AsyncWebServerRequest *request);
void sendTaskStateUpdate();
void setupWebServer();
