// Stage5 Wi-Fi lifecycle implementation.
// Compiled only when LUCE_HAS_WIFI is set.
#include "luce/net_wifi.h"

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
constexpr const char* kWifiNs = "wifi";
constexpr const char* kDefaultHostname = "luce-esp32";
constexpr std::size_t kWifiTaskStackWords = 4096;
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

std::uint32_t clamp_u32(std::uint32_t value, std::uint32_t min_val, std::uint32_t max_val) {
  if (value < min_val) {
    return min_val;
  }
  if (value > max_val) {
    return max_val;
  }
  return value;
}

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

const char* wifi_reject_reason_name(std::uint16_t reason) {
  if (reason == 0) {
    return "NONE";
  }
  if (reason == WIFI_REASON_UNSPECIFIED || reason == WIFI_REASON_AUTH_EXPIRE || reason == WIFI_REASON_AUTH_LEAVE ||
      reason == WIFI_REASON_ASSOC_EXPIRE || reason == WIFI_REASON_ASSOC_TOOMANY || reason == WIFI_REASON_ASSOC_LEAVE ||
      reason == WIFI_REASON_DISASSOC_PWRCAP_BAD || reason == WIFI_REASON_INVALID_PMKID ||
      reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
      reason == WIFI_REASON_CONNECTION_FAIL || reason == WIFI_REASON_TIMEOUT) {
    return "WIFI_ERROR";
  }
  return "OTHER";
}

const char* mask_password() {
  return "********";
}

void log_state_change(WifiState next, const char* reason) {
  g_state = next;
  if (reason && *reason) {
    ESP_LOGI(kTag, "[WIFI][LIFECYCLE] state=%s reason=%s", wifi_state_name(g_state), reason);
  } else {
    ESP_LOGI(kTag, "[WIFI][LIFECYCLE] state=%s", wifi_state_name(g_state));
  }
}

std::uint32_t next_backoff_ms() {
  std::uint32_t exponent = g_backoff_count;
  if (exponent > 31) {
    exponent = 31;
  }
  std::uint32_t max_allowed = g_cfg.backoff_max_ms ? g_cfg.backoff_max_ms : 1;
  std::uint32_t next = g_cfg.backoff_min_ms << exponent;
  return clamp_u32(next, g_cfg.backoff_min_ms, max_allowed);
}

void schedule_backoff(const char* reason) {
  ++g_backoff_count;
  g_next_backoff_ms = next_backoff_ms();
  g_next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(g_next_backoff_ms);
  g_last_backoff_tick = xTaskGetTickCount();
  log_state_change(WifiState::kBackoff, reason);
  ESP_LOGW(kTag, "[WIFI][BACKOFF] delay_ms=%lu", static_cast<unsigned long>(g_next_backoff_ms));
}

std::uint32_t active_retries_remaining() {
  if (g_cfg.max_retries == 0) {
    return 0xFFFFFFFFu;
  }
  if (g_retry_count >= g_cfg.max_retries) {
    return 0u;
  }
  return g_cfg.max_retries - g_retry_count;
}

void log_status_snapshot() {
  char ip_line[32] = "n/a";
  char gw_line[32] = "n/a";
  char mask_line[32] = "n/a";
  int8_t rssi = 0;

  if (g_have_ip && g_sta_if != nullptr) {
    esp_netif_ip_info_t info {};
    if (esp_netif_get_ip_info(g_sta_if, &info) == ESP_OK) {
      ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t*>(&info.ip), ip_line, sizeof(ip_line));
      ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t*>(&info.gw), gw_line, sizeof(gw_line));
      ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t*>(&info.netmask), mask_line, sizeof(mask_line));
    }
  }

  wifi_ap_record_t ap {};
  if (g_have_ip && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    rssi = ap.rssi;
  }

  ESP_LOGI(
      kTag,
      "[WIFI][STATUS] state=%s enabled=%d attempts=%lu remaining=%lu backoff_ms=%lu rssi=%d ip=%s gw=%s mask=%s reason=%s",
      wifi_state_name(g_state), g_cfg.enabled ? 1 : 0, static_cast<unsigned long>(g_retry_count),
      static_cast<unsigned long>(active_retries_remaining()), static_cast<unsigned long>(g_next_backoff_ms),
      static_cast<int>(rssi), ip_line, gw_line, mask_line, wifi_reject_reason_name(g_last_disconnect_reason));
}

void log_nvs_u8(const char* key, std::uint8_t value, bool found, std::uint8_t fallback) {
  if (found) {
    ESP_LOGI(kTag, "[WIFI][NVS] key=%s value=%u", key, static_cast<unsigned>(value));
  } else {
    ESP_LOGW(kTag, "[WIFI][NVS] key=%s missing; using default=%u", key, static_cast<unsigned>(fallback));
  }
}

void log_nvs_u32(const char* key, std::uint32_t value, bool found, std::uint32_t fallback) {
  if (found) {
    ESP_LOGI(kTag, "[WIFI][NVS] key=%s value=%lu", key, static_cast<unsigned long>(value));
  } else {
    ESP_LOGW(kTag, "[WIFI][NVS] key=%s missing; using default=%lu", key, static_cast<unsigned long>(fallback));
  }
}

void log_nvs_str(const char* key, const char* value, bool found, const char* fallback) {
  if (found) {
    if (std::strcmp(key, "pass") == 0) {
      ESP_LOGI(kTag, "[WIFI][NVS] key=%s value=%s", key, mask_password());
      return;
    }
    ESP_LOGI(kTag, "[WIFI][NVS] key=%s value='%s'", key, value);
    return;
  }
  if (std::strcmp(key, "pass") == 0) {
    ESP_LOGW(kTag, "[WIFI][NVS] key=%s missing; using default=%s", key, mask_password());
  } else {
    ESP_LOGW(kTag, "[WIFI][NVS] key=%s missing; using default='%s'", key, fallback);
  }
}

void load_wifi_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  std::snprintf(g_cfg.hostname, sizeof(g_cfg.hostname), "%s", kDefaultHostname);
  g_cfg.max_retries = 6;
  g_cfg.backoff_min_ms = 500;
  g_cfg.backoff_max_ms = 8000;

  nvs_handle_t nvs_handle {};
  if (nvs_open(kWifiNs, NVS_READONLY, &nvs_handle) != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][NVS] namespace '%s' not found; defaults active", kWifiNs);
    g_cfg.enabled = false;
    ESP_LOGW(kTag, "[WIFI][NVS] key=enabled missing; using default=0");
    ESP_LOGI(
        kTag,
        "[WIFI][NVS] config summary ssid='%s' pass=%s hostname='%s' enabled=%d max_retries=%lu backoff_min_ms=%lu "
        "backoff_max_ms=%lu",
        g_cfg.ssid, mask_password(), g_cfg.hostname, g_cfg.enabled ? 1 : 0,
        static_cast<unsigned long>(g_cfg.max_retries), static_cast<unsigned long>(g_cfg.backoff_min_ms),
        static_cast<unsigned long>(g_cfg.backoff_max_ms));
    return;
  }

  std::uint8_t value_u8 = 0;
  std::uint32_t value_u32 = 0;
  std::size_t needed = 0;

  auto read_u8 = [&](const char* key, std::uint8_t default_value, std::uint8_t& out, bool& found_flag) {
    if (nvs_get_u8(nvs_handle, key, &value_u8) == ESP_OK) {
      out = value_u8;
      found_flag = true;
    } else {
      out = default_value;
      found_flag = false;
    }
  };

  auto read_u32 = [&](const char* key, std::uint32_t default_value, std::uint32_t& out, bool& found_flag) {
    if (nvs_get_u32(nvs_handle, key, &value_u32) == ESP_OK) {
      out = value_u32;
      found_flag = true;
    } else {
      out = default_value;
      found_flag = false;
    }
  };

  auto read_string =
      [&](const char* key, char* out, std::size_t out_size, const char* fallback, bool& found_flag) {
        if (nvs_get_str(nvs_handle, key, nullptr, &needed) == ESP_OK && needed > 0) {
          if (needed > out_size) {
            needed = out_size;
          }
          if (nvs_get_str(nvs_handle, key, out, &needed) == ESP_OK) {
            found_flag = true;
            return;
          }
        }
        found_flag = false;
        std::snprintf(out, out_size, "%s", fallback);
      };

  bool found_flag = false;
  read_u8("enabled", 0, value_u8, found_flag);
  g_cfg.enabled = (value_u8 != 0);
  log_nvs_u8("enabled", value_u8, found_flag, 0);

  bool found_ssid = false;
  read_string("ssid", g_cfg.ssid, sizeof(g_cfg.ssid), "", found_ssid);
  log_nvs_str("ssid", g_cfg.ssid, found_ssid, "");

  bool found_pass = false;
  read_string("pass", g_cfg.pass, sizeof(g_cfg.pass), "", found_pass);
  log_nvs_str("pass", g_cfg.pass, found_pass, "");

  bool found_hostname = false;
  read_string("hostname", g_cfg.hostname, sizeof(g_cfg.hostname), kDefaultHostname, found_hostname);
  log_nvs_str("hostname", g_cfg.hostname, found_hostname, kDefaultHostname);

  bool found_max_retries = false;
  read_u32("max_retries", 6, value_u32, found_max_retries);
  g_cfg.max_retries = value_u32;
  log_nvs_u32("max_retries", value_u32, found_max_retries, 6);

  bool found_backoff_min = false;
  read_u32("backoff_min_ms", 500, value_u32, found_backoff_min);
  g_cfg.backoff_min_ms = clamp_u32(value_u32, 250, 300000);
  log_nvs_u32("backoff_min_ms", value_u32, found_backoff_min, g_cfg.backoff_min_ms);

  bool found_backoff_max = false;
  read_u32("backoff_max_ms", 8000, value_u32, found_backoff_max);
  g_cfg.backoff_max_ms = clamp_u32(value_u32, g_cfg.backoff_min_ms, 300000);
  log_nvs_u32("backoff_max_ms", value_u32, found_backoff_max, g_cfg.backoff_max_ms);

  if (g_cfg.max_retries > 600) {
    g_cfg.max_retries = 600;
  }
  if (g_cfg.backoff_min_ms < 250) {
    g_cfg.backoff_min_ms = 250;
  }
  if (g_cfg.backoff_max_ms < g_cfg.backoff_min_ms) {
    g_cfg.backoff_max_ms = g_cfg.backoff_min_ms;
  }

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
    log_state_change(WifiState::kDisabled, reason);
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
  log_state_change(WifiState::kConnecting, reason ? reason : "connect");
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
      log_state_change(WifiState::kDisabled, "disabled");
      return;
    }
    if (g_cfg.max_retries != 0 && g_retry_count >= g_cfg.max_retries) {
      g_retry_count = 0;
      g_backoff_count = 0;
      log_state_change(WifiState::kStopped, "max_retries_exceeded");
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
    log_state_change(WifiState::kGotIp, "got_ip");
    log_status_snapshot();
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

  log_state_change(WifiState::kInit, "stack_initialized");
}

void wifi_task(void*) {
  log_status_snapshot();
  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if (!g_cfg.enabled) {
      if (g_state != WifiState::kDisabled) {
        log_state_change(WifiState::kDisabled, "runtime_disabled");
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
      log_status_snapshot();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void wifi_startup() {
  load_wifi_config();

  if (!g_cfg.enabled) {
    log_state_change(WifiState::kDisabled, "cfg_disabled");
    return;
  }

  initialize_wifi_stack();
  if (g_wifi_task == nullptr) {
    xTaskCreate(wifi_task, "wifi", kWifiTaskStackWords, nullptr, 2, &g_wifi_task);
  }
}

void wifi_status_for_cli() {
  log_status_snapshot();
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

bool wifi_is_ip_ready() {
  return g_have_ip;
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

bool wifi_is_ip_ready() {
  return false;
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
