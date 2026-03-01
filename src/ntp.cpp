// Stage6 SNTP implementation.
// Compiled only when LUCE_HAS_NTP is set.
#include "luce/ntp.h"
#include "luce/net_wifi.h"
#include "luce/task_budgets.h"
#include "luce/nvs_helpers.h"
#include "luce/runtime_state.h"

#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>

#if LUCE_HAS_NTP

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

constexpr const char* kTag = "[NTP]";
constexpr const char* kNvsTag = "[NTP][NVS]";
constexpr const char* kNs = "ntp";
constexpr const char* kDefaultServer1 = "pool.ntp.org";
constexpr const char* kDefaultServer2 = "time.google.com";

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

const char* ntp_state_name_current_impl() {
  return ntp_state_name(g_state);
}

void set_state(NtpState next, const char* reason) {
  g_state_tick = xTaskGetTickCount();
  luce::runtime::set_state(g_state, next, ntp_state_name, "[NTP][LIFECYCLE]", reason);
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
    luce::nvs::log_nvs_u8(kNvsTag, "enabled", 0, false, 0);
    luce::nvs::log_nvs_string(kNvsTag, "server1", kDefaultServer1, false, kDefaultServer1, true);
    luce::nvs::log_nvs_string(kNvsTag, "server2", kDefaultServer2, false, kDefaultServer2, true);
    luce::nvs::log_nvs_string(kNvsTag, "server3", "", false, "", true);
    luce::nvs::log_nvs_u32(kNvsTag, "sync_timeout_s", g_cfg.sync_timeout_s, false, g_cfg.sync_timeout_s);
    luce::nvs::log_nvs_u32(kNvsTag, "sync_interval_s", g_cfg.sync_interval_s, false, g_cfg.sync_interval_s);
    return;
  }

  bool f_enabled = false;
  std::uint8_t u8 = 0;
  f_enabled = luce::nvs::read_u8(handle, "enabled", u8, 0);
  g_cfg.enabled = (u8 != 0);
  luce::nvs::log_nvs_u8(kNvsTag, "enabled", u8, f_enabled, 0);

  bool f_s1 = false;
  f_s1 = luce::nvs::read_string(handle, "server1", g_cfg.server1, sizeof(g_cfg.server1), kDefaultServer1);
  luce::nvs::log_nvs_string(kNvsTag, "server1", g_cfg.server1, f_s1, kDefaultServer1, true);

  bool f_s2 = false;
  f_s2 = luce::nvs::read_string(handle, "server2", g_cfg.server2, sizeof(g_cfg.server2), kDefaultServer2);
  luce::nvs::log_nvs_string(kNvsTag, "server2", g_cfg.server2, f_s2, kDefaultServer2, true);

  bool f_s3 = false;
  f_s3 = luce::nvs::read_string(handle, "server3", g_cfg.server3, sizeof(g_cfg.server3), "");
  luce::nvs::log_nvs_string(kNvsTag, "server3", g_cfg.server3, f_s3, "", true);

  std::uint32_t u32 = 0;
  bool f_to = false;
  f_to = luce::nvs::read_u32(handle, "sync_timeout_s", u32, 30);
  g_cfg.sync_timeout_s = luce::runtime::clamp_u32(u32, 5u, 600u);
  luce::nvs::log_nvs_u32(kNvsTag, "sync_timeout_s", g_cfg.sync_timeout_s, f_to, g_cfg.sync_timeout_s);

  bool f_int = false;
  f_int = luce::nvs::read_u32(handle, "sync_interval_s", u32, 3600);
  g_cfg.sync_interval_s = luce::runtime::clamp_u32(u32, 60u, 86400u);
  luce::nvs::log_nvs_u32(kNvsTag, "sync_interval_s", g_cfg.sync_interval_s, f_int, g_cfg.sync_interval_s);

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
    const bool ip_up = wifi_is_ip_ready();

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

const char* ntp_state_name_current() {
  return ntp_state_name_current_impl();
}

bool ntp_is_enabled() {
  return g_cfg.enabled;
}

bool ntp_is_synced() {
  return g_state == NtpState::kSynced;
}

void ntp_startup() {
  if (g_task == nullptr) {
    (void)luce::start_task_once(g_task, ntp_task, luce::task_budget::kTaskNtp);
  }
}

void ntp_status_for_cli() {
  if (g_state == NtpState::kSynced) {
    log_synced_status();
    return;
  }
  ESP_LOGW(kTag, "[NTP] status state=%s time not synced: no valid time yet", ntp_state_name(g_state));
}

#else

bool ntp_is_enabled() {
  return false;
}

bool ntp_is_synced() {
  return false;
}

const char* ntp_state_name_current() {
  return "DISABLED";
}

void ntp_status_for_cli() {}

#endif
