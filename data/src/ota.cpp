// ota.cpp: implements OTA update routines
#include "ota.h"
#include "logger.h"
#include <esp_https_ota.h>
#include <esp_http_client.h>
#include "settings.h"

// ota.cpp: Replace ArduinoOTA with native ESP-IDF HTTPS OTA client (stub)

// Default OTA URL configured via Kconfig (CONFIG_OTA_URL)
#ifndef CONFIG_OTA_URL
#define CONFIG_OTA_URL ""
#endif

// Initialize native OTA client; return false to abort task
bool otaInit() {
  LOGINFO("OTA","Init","native HTTP(S) OTA client");
  return true;
}

// Poll OTA state; return false to terminate task after one run
bool otaLoop() {
  const char* url = CONFIG_OTA_URL;
  if (!url || url[0] == '\0') {
    LOGERR("OTA","URL","no OTA URL configured (CONFIG_OTA_URL)");
  } else {
    esp_http_client_config_t config = {};
    config.url = url;
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &config;
    LOGINFO("OTA","Start","fetch %s", url);
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
      LOGINFO("OTA","End","update successful, restarting");
      esp_restart();
    } else {
      LOGERR("OTA","Fail","update failed, err=%d", ret);
    }
  }
  return false;
}