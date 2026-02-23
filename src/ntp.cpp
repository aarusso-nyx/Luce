// Stage6 SNTP implementation.
// Compiled only when LUCE_HAS_NTP is set.
#include "luce/ntp.h"

#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>

#if LUCE_HAS_NTP

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

constexpr const char* kTag = "[NTP]";
constexpr const char* kNs = "ntp";
constexpr const char* kDefaultServer1 = "pool.ntp.org";
constexpr const char* kDefaultServer2 = "time.google.com";
constexpr const char* kNtpTaskName = "ntp";
constexpr std::size_t kNtpTaskStackWords = 4096;

enum class NtpState : std::uint8_t {
  kDisabled = 0,
  kUnsynced,
  kSyncing,
  kSynced,
  kFailed,
};

struct NtpConfig {
  bool enabled = false;
  char server1[65] = {};
  char server2[65] = {};
  char server3[65] = {};
  std::uint32_t sync_timeout_s = 30;
  std::uint32_t sync_interval_s = 3600;
};

NtpConfig g_cfg;
NtpState g_state = NtpState::kDisabled;
TaskHandle_t g_task = nullptr;
TickType_t g_state_tick = 0;
TickType_t g_sync_tick = 0;
TickType_t g_last_sync_tick = 0;
uint32_t g_backoff_count = 0;
uint32_t g_backoff_ms = 0;
std::uint64_t g_last_sync_unix = 0;

const char* ntp_state_name(NtpState state) {
  switch (state) {
    case NtpState::kDisabled:
      return "DISABLED";
    case NtpState::kUnsynced:
      return "UNSYNCED";
    case NtpState::kSyncing:
      return "SYNCING";
    case NtpState::kSynced:
      return "SYNCED";
    case NtpState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

void set_state(NtpState next, const char* reason) {
  g_state = next;
  g_state_tick = xTaskGetTickCount();
  if (reason) {
    ESP_LOGI(kTag, "[NTP][LIFECYCLE] state=%s reason=%s", ntp_state_name(g_state), reason);
  } else {
    ESP_LOGI(kTag, "[NTP][LIFECYCLE] state=%s", ntp_state_name(g_state));
  }
}

void log_nvs_u8(const char* key, std::uint8_t value, bool found, std::uint8_t fallback) {
  if (found) {
    ESP_LOGI(kTag, "[NTP][NVS] key=%s value=%u", key, static_cast<unsigned>(value));
  } else {
    ESP_LOGW(kTag, "[NTP][NVS] key=%s missing; using default=%u", key, static_cast<unsigned>(fallback));
  }
}

void log_nvs_u32(const char* key, std::uint32_t value, bool found, std::uint32_t fallback) {
  if (found) {
    ESP_LOGI(kTag, "[NTP][NVS] key=%s value=%lu", key, static_cast<unsigned long>(value));
  } else {
    ESP_LOGW(kTag, "[NTP][NVS] key=%s missing; using default=%lu", key, static_cast<unsigned long>(fallback));
  }
}

void log_nvs_string(const char* key, const char* value, bool found, const char* fallback) {
  if (found) {
    ESP_LOGI(kTag, "[NTP][NVS] key=%s value='%s'", key, value);
  } else {
    ESP_LOGW(kTag, "[NTP][NVS] key=%s missing; using default='%s'", key, fallback);
  }
}

std::uint32_t clamp_u32(std::uint32_t value, std::uint32_t min_v, std::uint32_t max_v) {
  if (value < min_v) {
    return min_v;
  }
  if (value > max_v) {
    return max_v;
  }
  return value;
}

bool has_wifi_ipv4() {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) {
    return false;
  }
  esp_netif_ip_info_t ip_info {};
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
    return false;
  }
  return ip_info.ip.addr != 0;
}

void load_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  std::snprintf(g_cfg.server1, sizeof(g_cfg.server1), "%s", kDefaultServer1);
  std::snprintf(g_cfg.server2, sizeof(g_cfg.server2), "%s", kDefaultServer2);
  g_cfg.enabled = false;
  g_cfg.sync_timeout_s = 30;
  g_cfg.sync_interval_s = 3600;

  nvs_handle_t handle {};
  if (nvs_open(kNs, NVS_READONLY, &handle) != ESP_OK) {
    log_nvs_u8("enabled", 0, false, 0);
    log_nvs_string("server1", kDefaultServer1, false, kDefaultServer1);
    log_nvs_string("server2", kDefaultServer2, false, kDefaultServer2);
    log_nvs_string("server3", "", false, "");
    log_nvs_u32("sync_timeout_s", g_cfg.sync_timeout_s, false, g_cfg.sync_timeout_s);
    log_nvs_u32("sync_interval_s", g_cfg.sync_interval_s, false, g_cfg.sync_interval_s);
    return;
  }

  std::uint8_t u8 = 0;
  std::uint32_t u32 = 0;
  std::size_t needed = 0;
  auto read_u8 = [&](const char* key, std::uint8_t fallback, bool& out_found, std::uint8_t& out) {
    if (nvs_get_u8(handle, key, &u8) == ESP_OK) {
      out = u8;
      out_found = true;
    } else {
      out = fallback;
      out_found = false;
    }
  };

  auto read_u32 = [&](const char* key, std::uint32_t fallback, bool& out_found, std::uint32_t& out) {
    if (nvs_get_u32(handle, key, &u32) == ESP_OK) {
      out = u32;
      out_found = true;
    } else {
      out = fallback;
      out_found = false;
    }
  };

  auto read_str = [&](const char* key, char* out, std::size_t out_size, const char* fallback, bool& out_found) {
    if (nvs_get_str(handle, key, nullptr, &needed) == ESP_OK && needed > 0) {
      if (needed > out_size) {
        needed = out_size;
      }
      if (nvs_get_str(handle, key, out, &needed) == ESP_OK) {
        out_found = true;
        return;
      }
    }
    out_found = false;
    std::snprintf(out, out_size, "%s", fallback);
  };

  bool f_enabled = false;
  read_u8("enabled", 0, f_enabled, u8);
  g_cfg.enabled = (u8 != 0);
  log_nvs_u8("enabled", u8, f_enabled, 0);

  bool f_s1 = false;
  read_str("server1", g_cfg.server1, sizeof(g_cfg.server1), kDefaultServer1, f_s1);
  log_nvs_string("server1", g_cfg.server1, f_s1, kDefaultServer1);

  bool f_s2 = false;
  read_str("server2", g_cfg.server2, sizeof(g_cfg.server2), kDefaultServer2, f_s2);
  log_nvs_string("server2", g_cfg.server2, f_s2, kDefaultServer2);

  bool f_s3 = false;
  read_str("server3", g_cfg.server3, sizeof(g_cfg.server3), "", f_s3);
  log_nvs_string("server3", g_cfg.server3, f_s3, "");

  bool f_to = false;
  read_u32("sync_timeout_s", 30, f_to, u32);
  g_cfg.sync_timeout_s = clamp_u32(u32, 5, 600);
  log_nvs_u32("sync_timeout_s", g_cfg.sync_timeout_s, f_to, g_cfg.sync_timeout_s);

  bool f_int = false;
  read_u32("sync_interval_s", 3600, f_int, u32);
  g_cfg.sync_interval_s = clamp_u32(u32, 60, 86400);
  log_nvs_u32("sync_interval_s", g_cfg.sync_interval_s, f_int, g_cfg.sync_interval_s);

  nvs_close(handle);
}

std::uint32_t next_backoff_ms() {
  std::uint32_t exponent = g_backoff_count;
  if (exponent > 10) {
    exponent = 10;
  }
  return 1000u << exponent;
}

void configure_sntp_servers() {
  esp_sntp_setservername(0, g_cfg.server1);
  esp_sntp_setservername(1, g_cfg.server2);
  if (g_cfg.server3[0] != '\0') {
    esp_sntp_setservername(2, g_cfg.server3);
  }
}

void start_sync(const char* reason) {
  if (g_cfg.server1[0] == '\0') {
    ESP_LOGW(kTag, "[NTP] start_sync skipped: server1 missing");
    set_state(NtpState::kFailed, "no_server");
    return;
  }

  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  set_state(NtpState::kSyncing, reason);
  g_sync_tick = xTaskGetTickCount();
  configure_sntp_servers();
  esp_sntp_init();
  ESP_LOGI(kTag, "[NTP] sync started");
}

void log_synced_status() {
  if (g_last_sync_unix == 0) {
    ESP_LOGW(kTag, "[NTP] time not synced");
    return;
  }

  const std::uint64_t now = static_cast<std::uint64_t>(time(NULL));
  const std::uint64_t age = (now > g_last_sync_unix) ? (now - g_last_sync_unix) : 0;
  std::tm tbuf {};
  const std::time_t sync_time = static_cast<std::time_t>(g_last_sync_unix);
  const std::tm* const utc = gmtime_r(&sync_time, &tbuf);
  char utc_line[40] = "n/a";
  if (utc != nullptr) {
    std::snprintf(utc_line, sizeof(utc_line), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc->tm_year + 1900, utc->tm_mon + 1,
                  utc->tm_mday, utc->tm_hour, utc->tm_min, utc->tm_sec);
  }
  ESP_LOGI(kTag,
           "[NTP] time.status state=%s unix=%llu age_s=%llu utc=%s", ntp_state_name(g_state),
           static_cast<unsigned long long>(g_last_sync_unix), static_cast<unsigned long long>(age),
           utc_line);
}

void ntp_task(void*) {
  set_state(NtpState::kUnsynced, "startup");
  load_config();

  if (!g_cfg.enabled) {
    set_state(NtpState::kDisabled, "cfg_disabled");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  g_backoff_count = 0;
  g_backoff_ms = 0;
  g_last_sync_unix = 0;

  for (;;) {
    const TickType_t now = xTaskGetTickCount();
    const bool ip_up = has_wifi_ipv4();

    if (!ip_up) {
      if (g_state != NtpState::kUnsynced && g_state != NtpState::kDisabled) {
        set_state(NtpState::kUnsynced, "no_ip");
      }
      g_sync_tick = now;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (g_state == NtpState::kUnsynced) {
      start_sync("ip_ready");
    } else if (g_state == NtpState::kFailed) {
      const TickType_t elapsed_ms = (now - g_sync_tick) * portTICK_PERIOD_MS;
      if (elapsed_ms >= g_backoff_ms) {
        start_sync("retry");
      }
    } else if (g_state == NtpState::kSyncing) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        g_last_sync_unix = static_cast<std::uint64_t>(time(NULL));
        g_last_sync_tick = now;
        g_backoff_count = 0;
        g_backoff_ms = 0;
        set_state(NtpState::kSynced, "sync_done");
        ESP_LOGI(kTag, "[NTP] first successful sync unix=%llu", static_cast<unsigned long long>(g_last_sync_unix));
        continue;
      }
      const TickType_t elapsed_ms = (now - g_sync_tick) * portTICK_PERIOD_MS;
      if (elapsed_ms >= g_cfg.sync_timeout_s * 1000u) {
        g_backoff_count++;
        g_backoff_ms = next_backoff_ms();
        if (g_backoff_ms > g_cfg.sync_interval_s * 1000u) {
          g_backoff_ms = g_cfg.sync_interval_s * 1000u;
        }
        g_sync_tick = now;
        esp_sntp_stop();
        set_state(NtpState::kFailed, "sync_timeout");
        ESP_LOGW(kTag, "[NTP] sync timeout after %lu s", static_cast<unsigned long>(g_cfg.sync_timeout_s));
      }
    } else if (g_state == NtpState::kSynced) {
      const TickType_t elapsed_ms = (now - g_last_sync_tick) * portTICK_PERIOD_MS;
      if (elapsed_ms >= g_cfg.sync_interval_s * 1000u) {
        g_backoff_count++;
        start_sync("periodic_recycle");
      }
      if ((now - g_state_tick) >= pdMS_TO_TICKS(5000)) {
        g_state_tick = now;
        log_synced_status();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

}  // namespace

void ntp_startup() {
  if (g_task == nullptr) {
    xTaskCreate(ntp_task, kNtpTaskName, kNtpTaskStackWords, nullptr, 2, &g_task);
  }
}

void ntp_status_for_cli() {
  if (g_state == NtpState::kSynced) {
    log_synced_status();
    return;
  }
  ESP_LOGW(kTag, "[NTP] status state=%s time not synced: no valid time yet", ntp_state_name(g_state));
}

#endif
