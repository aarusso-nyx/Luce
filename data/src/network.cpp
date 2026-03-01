// network.cpp: Implementation of network initialization (Wi-Fi, SNTP, mDNS)
#include "network.h"
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <lwip/apps/sntp.h>
// #include "mdns.h"
#include <freertos/event_groups.h>
#include <nvs_flash.h>
#include <esp_err.h>
#include <time.h>
#include "logger.h"
#include "settings.h"
#include "lcd.h"

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

// Wi-Fi event handler
static void wifi_event_handler(void* arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data) {
  EventGroupHandle_t group = (EventGroupHandle_t)arg;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
    xEventGroupClearBits(group, WIFI_CONNECTED_BIT);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(group, WIFI_CONNECTED_BIT);
  }
}

bool networkInit(const char* deviceName) {
  // Initialize TCP/IP
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  // Wi-Fi station
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  // Event group for Wi-Fi
  s_wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID,
      &wifi_event_handler, s_wifi_event_group, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP,
      &wifi_event_handler, s_wifi_event_group, NULL));
  // Configure and start Wi-Fi
  const char* ssids[2] = { Settings.getSSID(), Settings.getSSID2() };
  const char* pwds[2]  = { Settings.getPass(), Settings.getPass2() };
  bool connected = false;
  for (int i = 0; i < 2; ++i) {
    if (!ssids[i] || !ssids[i][0]) continue;
    LOGINFO("WiFi","Connect","%s", ssids[i]);
    lcdShowBoot("SSID:%s", ssids[i]);
    wifi_config_t wcfg = {};
    strncpy((char*)wcfg.sta.ssid, ssids[i], sizeof(wcfg.sta.ssid));
    strncpy((char*)wcfg.sta.password, pwds[i], sizeof(wcfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT) {
      connected = true;
      esp_netif_ip_info_t ip_info;
      esp_netif_get_ip_info(
          esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
      char ipstr[16];
      snprintf(ipstr, sizeof(ipstr), "%d.%d.%d.%d",
               IP2STR(&ip_info.ip.u_addr.ip4));
      LOGSYS("WiFi","Connect","%s, IP:%s", ssids[i], ipstr);
      lcdShowBoot("IP:%s", ipstr);
      break;
    }
  }
  if (!connected) {
    LOGERR("WiFi","Connect","Failed to connect");
    return false;
  }
  // SNTP
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_init();
  while (time(NULL) < 100000) vTaskDelay(pdMS_TO_TICKS(100));
  char buf[32];
  time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  LOGSYS("NTP","Time","%s", buf);
  // mDNS
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(deviceName));
  ESP_ERROR_CHECK(mdns_instance_name_set(deviceName));
  ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
  ESP_ERROR_CHECK(mdns_service_add(NULL, "_mqtt", "_tcp", 1883, NULL, 0));
  LOGSYS("mDNS","Start","%s.local", deviceName);
  return true;
}