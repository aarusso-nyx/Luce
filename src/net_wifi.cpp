// Stage5 Wi-Fi lifecycle implementation.
// Compiled only when LUCE_HAS_WIFI is set.
#include "luce/net_wifi.h"
#include "luce/task_budgets.h"
#include "luce/nvs_helpers.h"
#include "luce/runtime_state.h"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if LUCE_HAS_WIFI

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

namespace {

constexpr const char* kTag = "[WIFI]";
constexpr const char* kNvsTag = "[WIFI][NVS]";
constexpr const char* kWifiNs = "wifi";
constexpr const char* kDefaultHostname = "luce-esp32";
constexpr const char* kDefaultSsid = "NYXK";
constexpr const char* kDefaultPass = "It's$14.99!";
constexpr TickType_t kStatusLogPeriodMs = 3000;
constexpr TickType_t kStoppedLogPeriodMs = 10000;
constexpr TickType_t kBackoffLogPeriodMs = 5000;

enum class WifiState : std::uint8_t {
  kDisabled = 0,
  kInit,
  kConnecting,
  kGotIp,
  kBackoff,
  kStopped,
};

struct WifiConfig {
  bool enabled = false;
  char ssid[33] = {};
  char pass[65] = {};
  char hostname[33] = {};
  std::uint32_t max_retries = 6;
  std::uint32_t backoff_min_ms = 500;
  std::uint32_t backoff_max_ms = 8000;
};

WifiConfig g_cfg;
esp_netif_t* g_sta_if = nullptr;
wifi_config_t g_wifi_config_storage {};
TaskHandle_t g_wifi_task = nullptr;
esp_event_handler_instance_t g_wifi_handler_instance = nullptr;
esp_event_handler_instance_t g_ip_handler_instance = nullptr;

WifiState g_state = WifiState::kInit;
std::uint32_t g_retry_count = 0;
std::uint32_t g_backoff_count = 0;
std::uint32_t g_next_backoff_ms = 0;
TickType_t g_next_retry_tick = 0;
TickType_t g_last_status_tick = 0;
TickType_t g_last_backoff_tick = 0;
uint16_t g_last_disconnect_reason = 0;
bool g_have_ip = false;

const char* wifi_state_name(WifiState state) {
  switch (state) {
    case WifiState::kDisabled:
      return "DISABLED";
    case WifiState::kInit:
      return "INIT";
    case WifiState::kConnecting:
      return "CONNECTING";
    case WifiState::kGotIp:
      return "GOT_IP";
    case WifiState::kBackoff:
      return "BACKOFF";
    case WifiState::kStopped:
      return "STOPPED";
    default:
      return "UNKNOWN";
  }
}

const char* mask_password() {
  return "********";
}

void set_state(WifiState next, const char* reason) {
  luce::runtime::set_state(g_state, next, wifi_state_name, "[WIFI][LIFECYCLE]", reason);
}

std::uint32_t next_backoff_ms() {
  std::uint32_t exponent = g_backoff_count;
  if (exponent > 31) {
    exponent = 31;
  }
  std::uint32_t max_allowed = g_cfg.backoff_max_ms ? g_cfg.backoff_max_ms : 1;
  std::uint32_t next = g_cfg.backoff_min_ms << exponent;
  return luce::runtime::clamp_u32(next, g_cfg.backoff_min_ms, max_allowed);
}

void schedule_backoff(const char* reason) {
  ++g_backoff_count;
  g_next_backoff_ms = next_backoff_ms();
  g_next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(g_next_backoff_ms);
  g_last_backoff_tick = xTaskGetTickCount();
  set_state(WifiState::kBackoff, reason);
  ESP_LOGW(kTag, "[WIFI][BACKOFF] delay_ms=%lu", static_cast<unsigned long>(g_next_backoff_ms));
}

void log_nvs_str(const char* key, const char* value, bool found, const char* fallback) {
  const bool mask = (std::strcmp(key, "pass") == 0);
  luce::nvs::log_nvs_string(kNvsTag, key, value, found, fallback, !mask, mask);
}

void load_wifi_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  std::snprintf(g_cfg.hostname, sizeof(g_cfg.hostname), "%s", kDefaultHostname);
  std::snprintf(g_cfg.ssid, sizeof(g_cfg.ssid), "%s", kDefaultSsid);
  std::snprintf(g_cfg.pass, sizeof(g_cfg.pass), "%s", kDefaultPass);
  g_cfg.enabled = true;
  g_cfg.max_retries = 6;
  g_cfg.backoff_min_ms = 500;
  g_cfg.backoff_max_ms = 8000;

  nvs_handle_t nvs_handle {};
  if (nvs_open(kWifiNs, NVS_READONLY, &nvs_handle) != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][NVS] namespace '%s' not found; defaults active", kWifiNs);
    ESP_LOGW(kTag, "[WIFI][NVS] key=enabled missing; using default=1");
    ESP_LOGI(
        kTag,
        "[WIFI][NVS] config summary ssid='%s' pass=%s hostname='%s' enabled=%d max_retries=%lu backoff_min_ms=%lu "
        "backoff_max_ms=%lu",
        g_cfg.ssid, mask_password(), g_cfg.hostname, g_cfg.enabled ? 1 : 0,
        static_cast<unsigned long>(g_cfg.max_retries), static_cast<unsigned long>(g_cfg.backoff_min_ms),
        static_cast<unsigned long>(g_cfg.backoff_max_ms));
    return;
  }

  bool found_flag = false;
  std::uint8_t value_u8 = 0;
  if (!luce::nvs::read_u8(nvs_handle, "enabled", value_u8, 1)) {
    found_flag = false;
  } else {
    found_flag = true;
  }
  g_cfg.enabled = (value_u8 != 0);
  luce::nvs::log_nvs_u8(kNvsTag, "enabled", value_u8, found_flag, 1);

  bool found_ssid = false;
  found_ssid = luce::nvs::read_string(nvs_handle, "ssid", g_cfg.ssid, sizeof(g_cfg.ssid), kDefaultSsid);
  log_nvs_str("ssid", g_cfg.ssid, found_ssid, "");

  bool found_pass = false;
  found_pass = luce::nvs::read_string(nvs_handle, "pass", g_cfg.pass, sizeof(g_cfg.pass), kDefaultPass);
  log_nvs_str("pass", g_cfg.pass, found_pass, "");

  bool found_hostname = false;
  found_hostname = luce::nvs::read_string(nvs_handle, "hostname", g_cfg.hostname, sizeof(g_cfg.hostname), kDefaultHostname);
  log_nvs_str("hostname", g_cfg.hostname, found_hostname, kDefaultHostname);

  bool found_max_retries = false;
  std::uint32_t value_u32 = 0;
  found_max_retries = luce::nvs::read_u32(nvs_handle, "max_retries", value_u32, 6);
  g_cfg.max_retries = luce::runtime::clamp_u32(value_u32, 0u, 600u);
  luce::nvs::log_nvs_u32(kNvsTag, "max_retries", value_u32, found_max_retries, 6);

  bool found_backoff_min = false;
  found_backoff_min = luce::nvs::read_u32(nvs_handle, "backoff_min_ms", value_u32, 500);
  g_cfg.backoff_min_ms = luce::runtime::clamp_u32(value_u32, 250u, 300000u);
  luce::nvs::log_nvs_u32(kNvsTag, "backoff_min_ms", value_u32, found_backoff_min, g_cfg.backoff_min_ms);

  bool found_backoff_max = false;
  found_backoff_max = luce::nvs::read_u32(nvs_handle, "backoff_max_ms", value_u32, 8000);
  g_cfg.backoff_max_ms = luce::runtime::clamp_u32(value_u32, g_cfg.backoff_min_ms, 300000u);
  luce::nvs::log_nvs_u32(kNvsTag, "backoff_max_ms", value_u32, found_backoff_max, g_cfg.backoff_max_ms);

  nvs_close(nvs_handle);

  ESP_LOGI(
      kTag,
      "[WIFI][NVS] config summary ssid='%s' pass=%s hostname='%s' enabled=%d max_retries=%lu backoff_min_ms=%lu "
      "backoff_max_ms=%lu",
      g_cfg.ssid, mask_password(), g_cfg.hostname, g_cfg.enabled ? 1 : 0,
      static_cast<unsigned long>(g_cfg.max_retries), static_cast<unsigned long>(g_cfg.backoff_min_ms),
      static_cast<unsigned long>(g_cfg.backoff_max_ms));
}

void apply_wifi_config() {
  std::memset(&g_wifi_config_storage, 0, sizeof(g_wifi_config_storage));
  std::strncpy(reinterpret_cast<char*>(g_wifi_config_storage.sta.ssid), g_cfg.ssid,
               sizeof(g_wifi_config_storage.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(g_wifi_config_storage.sta.password), g_cfg.pass,
               sizeof(g_wifi_config_storage.sta.password) - 1);
  g_wifi_config_storage.sta.threshold.authmode = WIFI_AUTH_OPEN;

  if (*g_cfg.hostname != '\0' && g_sta_if != nullptr) {
    esp_netif_set_hostname(g_sta_if, g_cfg.hostname);
  }

  const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &g_wifi_config_storage);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI] failed to apply station config: %s", esp_err_to_name(err));
    return;
  }
}

void connect_wifi(const char* reason) {
  if (!g_cfg.enabled) {
    set_state(WifiState::kDisabled, reason);
    return;
  }
  if (*g_cfg.ssid == '\0') {
    g_last_disconnect_reason = WIFI_REASON_AUTH_EXPIRE;
    ESP_LOGE(kTag, "[WIFI][ERROR] cannot connect: SSID is empty");
    schedule_backoff("ssid_missing");
    return;
  }

  const esp_err_t set_cfg_err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (set_cfg_err != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI] set_mode failed: %s", esp_err_to_name(set_cfg_err));
    schedule_backoff("set_mode");
    return;
  }
  apply_wifi_config();
  const esp_err_t connect_err = esp_wifi_connect();
  if (connect_err != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI] esp_wifi_connect failed: %s", esp_err_to_name(connect_err));
    schedule_backoff("connect_failed");
    return;
  }

  ++g_retry_count;
  g_last_disconnect_reason = 0;
  g_next_backoff_ms = 0;
    set_state(WifiState::kConnecting, reason ? reason : "connect");
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  (void)arg;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    connect_wifi("sta_start");
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
    if (event != nullptr) {
      g_last_disconnect_reason = event->reason;
    }
    g_have_ip = false;
    if (!g_cfg.enabled) {
      set_state(WifiState::kDisabled, "disabled");
      return;
    }
    if (g_cfg.max_retries != 0 && g_retry_count >= g_cfg.max_retries) {
      g_retry_count = 0;
      g_backoff_count = 0;
      set_state(WifiState::kStopped, "max_retries_exceeded");
      return;
    }
    schedule_backoff("disconnected");
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    g_have_ip = true;
    g_retry_count = 0;
    g_backoff_count = 0;
    g_next_backoff_ms = 0;
    g_last_disconnect_reason = 0;
    set_state(WifiState::kGotIp, "got_ip");
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    g_have_ip = false;
    schedule_backoff("lost_ip");
  }
}

void initialize_wifi_stack() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_sta_if = esp_netif_create_default_wifi_sta();
  if (g_sta_if == nullptr) {
    ESP_LOGE(kTag, "[WIFI] failed to create default Wi-Fi STA interface");
    return;
  }

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, &g_wifi_handler_instance));
  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, &g_ip_handler_instance));

  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI] unable to set Wi-Fi mode");
  }
  if (esp_wifi_start() != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI] esp_wifi_start failed");
    return;
  }

  set_state(WifiState::kInit, "stack_initialized");
}

void wifi_task(void*) {
  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if (!g_cfg.enabled) {
      if (g_state != WifiState::kDisabled) {
        set_state(WifiState::kDisabled, "runtime_disabled");
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (g_state == WifiState::kBackoff && now >= g_next_retry_tick) {
      connect_wifi("backoff_expired");
    }

    if (g_state == WifiState::kStopped) {
      if (now - g_last_status_tick > pdMS_TO_TICKS(kStoppedLogPeriodMs)) {
        g_last_status_tick = now;
        ESP_LOGW(kTag, "[WIFI] state=%s max_retries_exceeded. reboot config to retry.", wifi_state_name(g_state));
      }
    }

    if (g_state == WifiState::kBackoff) {
      if (now - g_last_backoff_tick > pdMS_TO_TICKS(kBackoffLogPeriodMs)) {
        g_last_backoff_tick = now;
        ESP_LOGW(kTag, "[WIFI][BACKOFF] remaining_ms=%lu", static_cast<unsigned long>(g_next_backoff_ms));
      }
    }

    if ((g_state == WifiState::kGotIp || g_state == WifiState::kConnecting || g_state == WifiState::kBackoff ||
         g_state == WifiState::kInit) &&
        now - g_last_status_tick > pdMS_TO_TICKS(kStatusLogPeriodMs)) {
      g_last_status_tick = now;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void wifi_startup() {
  load_wifi_config();

  if (!g_cfg.enabled) {
    set_state(WifiState::kDisabled, "cfg_disabled");
    return;
  }

  initialize_wifi_stack();
  if (g_wifi_task == nullptr) {
    (void)luce::start_task_once(g_wifi_task, wifi_task, luce::task_budget::kTaskWifi);
  }
}

void wifi_status_for_cli() {
}

void wifi_scan_for_cli() {
  if (!g_cfg.enabled) {
    ESP_LOGW(kTag, "[WIFI][SCAN] skipped: wifi disabled");
    return;
  }

  if (g_state == WifiState::kDisabled) {
    ESP_LOGW(kTag, "[WIFI][SCAN] skipped: wifi subsystem disabled");
    return;
  }

  wifi_scan_config_t scan_cfg {};
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_cfg.show_hidden = false;
  scan_cfg.scan_time.active.min = 100;
  scan_cfg.scan_time.active.max = 200;

  const esp_err_t start_err = esp_wifi_scan_start(&scan_cfg, true);
  if (start_err != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][SCAN] start failed: %s", esp_err_to_name(start_err));
    return;
  }

  uint16_t count = 0;
  if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) {
    ESP_LOGI(kTag, "[WIFI][SCAN] no APs found");
    return;
  }

  std::array<wifi_ap_record_t, 16> records {};
  uint16_t to_read = static_cast<uint16_t>(records.size());
  if (to_read > count) {
    to_read = count;
  }
  if (esp_wifi_scan_get_ap_records(&to_read, records.data()) != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][SCAN] failed to read records");
    return;
  }

  ESP_LOGI(kTag, "[WIFI][SCAN] count=%u", static_cast<unsigned>(to_read));
  for (uint16_t i = 0; i < to_read; ++i) {
    ESP_LOGI(kTag, "[WIFI][SCAN] %u: ssid=%s rssi=%d auth=%d", static_cast<unsigned>(i + 1),
             records[i].ssid[0] != '\0' ? reinterpret_cast<const char*>(records[i].ssid) : "(hidden)",
             records[i].rssi, static_cast<int>(records[i].authmode));
  }
}

bool wifi_is_enabled() {
  return g_cfg.enabled;
}

bool wifi_is_connecting() {
  return g_cfg.enabled && (g_state == WifiState::kConnecting || g_state == WifiState::kBackoff);
}

bool wifi_is_ip_ready() {
  return g_have_ip;
}

bool wifi_is_connected() {
  return g_cfg.enabled && (g_state == WifiState::kGotIp) && g_have_ip;
}

void wifi_get_ssid(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  std::snprintf(out, out_size, "%s", (g_cfg.ssid[0] != '\0') ? g_cfg.ssid : "n/a");
}

void wifi_copy_ip_str(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  if (!g_sta_if || !g_have_ip) {
    std::snprintf(out, out_size, "n/a");
    return;
  }

  esp_netif_ip_info_t ip_info {};
  if (esp_netif_get_ip_info(g_sta_if, &ip_info) != ESP_OK) {
    std::snprintf(out, out_size, "n/a");
    return;
  }
  std::snprintf(out, out_size, IPSTR, IP2STR(&ip_info.ip));
}

void wifi_get_rssi(int* rssi_out) {
  if (!rssi_out) {
    return;
  }
  wifi_ap_record_t ap {};
  if (!g_have_ip || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    *rssi_out = 0;
    return;
  }
  *rssi_out = ap.rssi;
}

#else

void wifi_startup() {}
void wifi_status_for_cli() {}
void wifi_scan_for_cli() {}
bool wifi_is_enabled() {
  return false;
}

bool wifi_is_connecting() {
  return false;
}

bool wifi_is_ip_ready() {
  return false;
}

bool wifi_is_connected() {
  return false;
}

void wifi_get_ssid(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  std::snprintf(out, out_size, "n/a");
}

void wifi_copy_ip_str(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  std::snprintf(out, out_size, "n/a");
}

void wifi_get_rssi(int* rssi_out) {
  if (!rssi_out) {
    return;
  }
  *rssi_out = 0;
}

#endif  // LUCE_HAS_WIFI
