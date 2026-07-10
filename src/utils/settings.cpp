#include "settings.h"
#include "../state.h"
#include "../config.h"
#include <Forecaster.h>

extern Forecaster cond;

void nrf905SaveSettings(int channel, bool band, const char *power) {
  if (nrf905NvsHandle == 0) return;
  nvs_set_i32(nrf905NvsHandle, "channel", channel);
  nvs_set_u8(nrf905NvsHandle, "band", band ? 1 : 0);
  nvs_set_str(nrf905NvsHandle, "power", power);
  nvs_commit(nrf905NvsHandle);
  ESP_LOGI("NRF905", "Настройки сохранены в NVS: ch=%d band=%d pwr=%s", channel, band, power);
}

bool nrf905LoadSettings(int &channel, bool &band, char *power, size_t powerLen) {
  if (nrf905NvsHandle == 0) return false;
  int32_t ch; uint8_t b;
  if (nvs_get_i32(nrf905NvsHandle, "channel", &ch) != ESP_OK) return false;
  if (nvs_get_u8(nrf905NvsHandle, "band", &b) != ESP_OK) return false;
  if (nvs_get_str(nrf905NvsHandle, "power", power, &powerLen) != ESP_OK) return false;
  channel = (int)ch;
  band = (b != 0);
  ESP_LOGI("NRF905", "Настройки загружены из NVS: ch=%d band=%d pwr=%s", channel, band, power);
  return true;
}

void settingsSaveAll() {
  if (settingsNvsHandle == 0) return;
  nvs_set_str(settingsNvsHandle, "wifi_ssid",   ssid);
  nvs_set_str(settingsNvsHandle, "wifi_pass",   password);
  nvs_set_str(settingsNvsHandle, "http_user",   http_username);
  nvs_set_str(settingsNvsHandle, "http_pass",   http_password);
  nvs_set_u8(settingsNvsHandle, "use_static_ip",  useStaticIP ? 1 : 0);
  nvs_set_str(settingsNvsHandle, "static_ip",      staticIP);
  nvs_set_str(settingsNvsHandle, "static_gateway", staticGateway);
  nvs_set_str(settingsNvsHandle, "static_subnet",  staticSubnet);
  nvs_set_str(settingsNvsHandle, "static_dns",     staticDNS);
  nvs_set_str(settingsNvsHandle, "influx_host", influxDBHost);
  nvs_set_i32(settingsNvsHandle, "influx_port", influxDBPort);
  nvs_set_str(settingsNvsHandle, "influx_db",   influxDBDatabase);
  nvs_set_str(settingsNvsHandle, "ntp_server",  ntpServer);
  nvs_set_i32(settingsNvsHandle, "tz_sec",      gmtOffset_sec);
  nvs_set_str(settingsNvsHandle, "latitude",    String(latitude, 6).c_str());
  nvs_set_str(settingsNvsHandle, "longitude",   String(longitude, 6).c_str());
  nvs_set_i32(settingsNvsHandle, "tz_offset",   tzOffset);
  nvs_set_str(settingsNvsHandle, "tCorr",       String(tCorr, 2).c_str());
  nvs_set_str(settingsNvsHandle, "altitude_m",  String(altitude_m, 1).c_str());
  nvs_commit(settingsNvsHandle);
  ESP_LOGI("SETTINGS", "Настройки сохранены в NVS");
}

void settingsLoadAll() {
  if (settingsNvsHandle == 0) return;
  char buf[64];
  size_t len;
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "wifi_ssid", buf, &len) == ESP_OK) strncpy(ssid, buf, sizeof(ssid) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "wifi_pass", buf, &len) == ESP_OK) strncpy(password, buf, sizeof(password) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "http_user", buf, &len) == ESP_OK) strncpy(http_username, buf, sizeof(http_username) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "http_pass", buf, &len) == ESP_OK) strncpy(http_password, buf, sizeof(http_password) - 1);
  uint8_t ipMode;
  if (nvs_get_u8(settingsNvsHandle, "use_static_ip", &ipMode) == ESP_OK) useStaticIP = (ipMode != 0);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_ip", buf, &len) == ESP_OK) strncpy(staticIP, buf, sizeof(staticIP) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_gateway", buf, &len) == ESP_OK) strncpy(staticGateway, buf, sizeof(staticGateway) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_subnet", buf, &len) == ESP_OK) strncpy(staticSubnet, buf, sizeof(staticSubnet) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "static_dns", buf, &len) == ESP_OK) strncpy(staticDNS, buf, sizeof(staticDNS) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "influx_host", buf, &len) == ESP_OK) strncpy(influxDBHost, buf, sizeof(influxDBHost) - 1);
  int32_t port;
  if (nvs_get_i32(settingsNvsHandle, "influx_port", &port) == ESP_OK) influxDBPort = (int)port;
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "influx_db", buf, &len) == ESP_OK) strncpy(influxDBDatabase, buf, sizeof(influxDBDatabase) - 1);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "ntp_server", buf, &len) == ESP_OK) strncpy(ntpServer, buf, sizeof(ntpServer) - 1);
  int32_t tz;
  if (nvs_get_i32(settingsNvsHandle, "tz_sec", &tz) == ESP_OK) gmtOffset_sec = tz;
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "latitude", buf, &len) == ESP_OK) latitude = atof(buf);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "longitude", buf, &len) == ESP_OK) longitude = atof(buf);
  if (nvs_get_i32(settingsNvsHandle, "tz_offset", &tz) == ESP_OK) tzOffset = (int)tz;
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "tCorr", buf, &len) == ESP_OK) tCorr = atof(buf);
  len = sizeof(buf); if (nvs_get_str(settingsNvsHandle, "altitude_m", buf, &len) == ESP_OK) altitude_m = atof(buf);
  ESP_LOGI("SETTINGS", "Настройки загружены из NVS");
}

void resetNVS() {
  ESP_LOGW("NVS", "Полная очистка NVS...");

  if (nrf905NvsHandle != 0) { nvs_close(nrf905NvsHandle); nrf905NvsHandle = 0; }
  if (settingsNvsHandle != 0) { nvs_close(settingsNvsHandle); settingsNvsHandle = 0; }

  nvs_flash_erase();
  nvs_flash_init();

  delay(500);
  ESP.restart();
}
