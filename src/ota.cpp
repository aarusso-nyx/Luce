// OTA update service implementation.
#include "luce/ota.h"
#include <cstdio>

#if LUCE_HAS_OTA

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "luce_build.h"
#include "luce/net_wifi.h"
#include "luce/nvs_helpers.h"
#include "luce/runtime_state.h"
#include "luce/task_budgets.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

constexpr const char* kTag = "[OTA]";
constexpr const char* kOtaNs = "ota";
constexpr std::size_t kUrlBufferSize = 256;
constexpr std::size_t kErrorBufferSize = 128;
constexpr std::uint32_t kDefaultCheckIntervalS = 0;
constexpr std::uint32_t kDefaultRequestTimeoutMs = 10000;
constexpr std::uint32_t kMaxCheckIntervalS = 86400;
constexpr std::uint32_t kDefaultPollMs = 250;
constexpr std::uint32_t kPeriodicTickPaddingMs = 1000;

enum class OtaState : std::uint8_t {
  kDisabled = 0,
  kIdle,
  kChecking,
  kSuccess,
  kFailed,
  kNoPartition,
  kInvalidConfig,
};

struct OtaConfig {
  bool enabled = false;
  char url[kUrlBufferSize] = {};
  std::uint32_t check_interval_s = kDefaultCheckIntervalS;
  std::uint32_t request_timeout_ms = kDefaultRequestTimeoutMs;
};

struct OtaRuntime {
  std::uint32_t total_checks = 0;
  std::uint32_t success_count = 0;
  std::uint32_t failure_count = 0;
  std::uint32_t last_check_error = ESP_OK;
  char last_error[kErrorBufferSize] = "never";
  std::uint64_t last_check_s = 0;
  std::uint64_t last_success_s = 0;
  bool check_requested = false;
  bool has_request_url = false;
  std::array<char, kUrlBufferSize> request_url {};
};

OtaConfig g_cfg {};
OtaRuntime g_rt {};
OtaState g_state = OtaState::kDisabled;
TaskHandle_t g_task = nullptr;
TickType_t g_next_periodic_check_tick = 0;
portMUX_TYPE g_state_lock = portMUX_INITIALIZER_UNLOCKED;

const char* const kNoConfig = "no_update_partition";
const char* const kNoUrl = "no_url";
const char* const kWaitingForIp = "waiting_ip";
const char* const kManualRequest = "manual_request";
const char* const kPeriodicRequest = "periodic_request";

const char* state_name(OtaState state) {
  switch (state) {
    case OtaState::kDisabled:
      return "DISABLED";
    case OtaState::kIdle:
      return "IDLE";
    case OtaState::kChecking:
      return "CHECKING";
    case OtaState::kSuccess:
      return "SUCCESS";
    case OtaState::kFailed:
      return "FAILED";
    case OtaState::kNoPartition:
      return "NO_PARTITION";
    case OtaState::kInvalidConfig:
      return "INVALID_CONFIG";
    default:
      return "UNKNOWN";
  }
}

const char* state_name_impl() {
  return state_name(g_state);
}

void set_state(OtaState next, const char* reason = nullptr) {
  luce::runtime::set_state(g_state, next, state_name, "[OTA][LIFECYCLE]", reason);
}

void set_last_error(const char* value) {
  if (!value || *value == '\0') {
    std::snprintf(g_rt.last_error, sizeof(g_rt.last_error), "unknown");
    return;
  }
  std::snprintf(g_rt.last_error, sizeof(g_rt.last_error), "%s", value);
}

void load_ota_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.enabled = false;
  g_cfg.check_interval_s = kDefaultCheckIntervalS;
  g_cfg.request_timeout_ms = kDefaultRequestTimeoutMs;
  g_cfg.url[0] = '\0';

  nvs_handle_t handle = 0;
  if (nvs_open(kOtaNs, NVS_READONLY, &handle) != ESP_OK) {
    g_cfg.enabled = false;
    set_state(OtaState::kDisabled, "namespace_missing");
    ESP_LOGW(kTag, "[OTA] namespace '%s' not found; disabled by default", kOtaNs);
    return;
  }

  std::uint8_t enabled = 0;
  std::uint32_t check_interval = kDefaultCheckIntervalS;
  std::uint32_t timeout_ms = kDefaultRequestTimeoutMs;
  bool found_enabled = false;
  bool found_url = false;
  bool found_interval = false;
  bool found_timeout = false;

  found_enabled = luce::nvs::read_u8(handle, "enabled", enabled, 0);
  g_cfg.enabled = (enabled != 0);
  luce::nvs::log_nvs_u8(kTag, "enabled", enabled, found_enabled, 0);

  found_url = luce::nvs::read_string(handle, "url", g_cfg.url, sizeof(g_cfg.url), "");
  luce::nvs::log_nvs_string(kTag, "url", g_cfg.url, found_url, "", true);

  found_interval = luce::nvs::read_u32(handle, "check_interval_s", check_interval, kDefaultCheckIntervalS);
  if (found_interval) {
    g_cfg.check_interval_s = luce::runtime::clamp_u32(check_interval, 0u, kMaxCheckIntervalS);
  } else {
    g_cfg.check_interval_s = kDefaultCheckIntervalS;
  }
  luce::nvs::log_nvs_u32(kTag, "check_interval_s", g_cfg.check_interval_s, found_interval, g_cfg.check_interval_s);

  found_timeout = luce::nvs::read_u32(handle, "request_timeout_ms", timeout_ms, kDefaultRequestTimeoutMs);
  if (found_timeout) {
    g_cfg.request_timeout_ms = luce::runtime::clamp_u32(timeout_ms, 1000u, 60000u);
  } else {
    g_cfg.request_timeout_ms = kDefaultRequestTimeoutMs;
  }
  luce::nvs::log_nvs_u32(kTag, "request_timeout_ms", g_cfg.request_timeout_ms, found_timeout, g_cfg.request_timeout_ms);
  nvs_close(handle);

  if (g_cfg.enabled) {
    set_state(OtaState::kIdle, "config_enabled");
  } else {
    set_state(OtaState::kDisabled, "config_disabled");
  }
}

bool has_update_partition() {
  const esp_partition_t* const running = esp_ota_get_running_partition();
  if (!running) {
    return false;
  }
  const esp_partition_t* const next = esp_ota_get_next_update_partition(running);
  return next != nullptr;
}

bool perform_ota(const char* url) {
  if (!url || *url == '\0') {
    set_state(OtaState::kInvalidConfig, kNoUrl);
    set_last_error("missing url");
    ++g_rt.failure_count;
    ++g_rt.total_checks;
    g_rt.last_check_error = ESP_ERR_INVALID_ARG;
    g_rt.last_check_s = static_cast<std::uint64_t>(esp_timer_get_time() / 1000000ULL);
    return false;
  }

  if (!has_update_partition()) {
    set_state(OtaState::kNoPartition, kNoConfig);
    set_last_error("no OTA partition");
    ++g_rt.failure_count;
    ++g_rt.total_checks;
    g_rt.last_check_error = ESP_ERR_NO_MEM;
    g_rt.last_check_s = static_cast<std::uint64_t>(esp_timer_get_time() / 1000000ULL);
    return false;
  }

  ++g_rt.total_checks;
  set_state(OtaState::kChecking, "start");
  g_rt.last_check_s = static_cast<std::uint64_t>(esp_timer_get_time() / 1000000ULL);
  g_rt.last_check_error = ESP_OK;
  set_last_error("starting");

  esp_http_client_config_t http_cfg {};
  http_cfg.url = url;
  http_cfg.timeout_ms = g_cfg.request_timeout_ms;
  http_cfg.keep_alive_enable = true;

  esp_https_ota_config_t ota_cfg {};
  ota_cfg.http_config = &http_cfg;

  const esp_err_t rc = esp_https_ota(&ota_cfg);
  if (rc == ESP_OK) {
    ++g_rt.success_count;
    g_rt.last_success_s = g_rt.last_check_s;
    g_rt.last_check_error = ESP_OK;
    set_last_error("success");
    set_state(OtaState::kSuccess, "complete");
    ESP_LOGI(kTag, "[OTA] update completed; rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return true;
  }

  ++g_rt.failure_count;
  g_rt.last_check_error = rc;
  set_last_error(esp_err_to_name(rc));
  set_state(OtaState::kFailed, "update_failed");
  ESP_LOGW(kTag, "[OTA] update failed: %s", esp_err_to_name(rc));
  return false;
}

void apply_update_request(const std::array<char, kUrlBufferSize>& request_url, bool has_request_url) {
  const char* const source = (has_request_url && request_url[0] != '\0') ? request_url.data() : g_cfg.url;
  (void)perform_ota(source);
}

void consume_check_request(bool* had_request, std::array<char, kUrlBufferSize>& request_url, bool* has_request_url) {
  request_url.fill('\0');
  *had_request = false;
  *has_request_url = false;

  portENTER_CRITICAL(&g_state_lock);
  if (g_rt.check_requested) {
    *had_request = true;
    *has_request_url = g_rt.has_request_url;
    if (*has_request_url && g_rt.request_url[0] != '\0') {
      std::size_t url_len = 0;
      while ((url_len < g_rt.request_url.size()) && (g_rt.request_url[url_len] != '\0')) {
        ++url_len;
      }
      if (url_len > 0) {
        std::memcpy(request_url.data(), g_rt.request_url.data(), url_len + 1);
      }
    }
    g_rt.check_requested = false;
    g_rt.has_request_url = false;
  }
  portEXIT_CRITICAL(&g_state_lock);
}

bool periodic_due(TickType_t now) {
  return g_cfg.check_interval_s > 0 && g_next_periodic_check_tick != 0 && now >= g_next_periodic_check_tick;
}

void schedule_periodic() {
  if (g_cfg.check_interval_s == 0) {
    g_next_periodic_check_tick = 0;
    return;
  }
  const TickType_t delay = pdMS_TO_TICKS(g_cfg.check_interval_s * 1000u);
  g_next_periodic_check_tick = xTaskGetTickCount() + delay + pdMS_TO_TICKS(kPeriodicTickPaddingMs);
}

void schedule_periodic_from_now() {
  if (g_cfg.check_interval_s == 0) {
    g_next_periodic_check_tick = 0;
    return;
  }
  g_next_periodic_check_tick = xTaskGetTickCount() + pdMS_TO_TICKS(g_cfg.check_interval_s * 1000u);
}

void ota_loop(void*) {
  load_ota_config();

  if (g_cfg.enabled) {
    g_next_periodic_check_tick = 0;
    schedule_periodic_from_now();
  } else {
    g_next_periodic_check_tick = 0;
  }

  for (;;) {
    const TickType_t now = xTaskGetTickCount();
    std::array<char, kUrlBufferSize> request_url {};
    bool has_request = false;
    bool has_request_url = false;

    if (!g_cfg.enabled) {
      set_state(OtaState::kDisabled, "disabled");
      vTaskDelay(pdMS_TO_TICKS(1000));
      load_ota_config();
      if (g_cfg.enabled && g_next_periodic_check_tick == 0) {
        schedule_periodic();
      }
      continue;
    }

    if (!wifi_is_ip_ready()) {
      set_state(OtaState::kIdle, kWaitingForIp);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    consume_check_request(&has_request, request_url, &has_request_url);
    if (has_request) {
      set_state(OtaState::kChecking, kManualRequest);
      apply_update_request(request_url, has_request_url);
      schedule_periodic();
      vTaskDelay(pdMS_TO_TICKS(kDefaultPollMs));
      continue;
    }

    if (periodic_due(now)) {
      set_state(OtaState::kChecking, kPeriodicRequest);
      if (g_cfg.url[0] != '\0') {
        (void)perform_ota(g_cfg.url);
      } else {
        set_state(OtaState::kInvalidConfig, kNoUrl);
        set_last_error("periodic url missing");
        g_rt.failure_count++;
      }
      schedule_periodic();
      vTaskDelay(pdMS_TO_TICKS(kDefaultPollMs));
      continue;
    }

    if (g_state != OtaState::kChecking) {
      set_state(OtaState::kIdle, "running");
    }
    vTaskDelay(pdMS_TO_TICKS(kDefaultPollMs));
  }
}

}  // namespace

void ota_startup() {
  if (g_task == nullptr) {
    (void)luce::start_task_once(g_task, ota_loop, luce::task_budget::kTaskOta);
  }
}

void ota_status_for_cli() {
  ESP_LOGI(kTag,
           "ota.status state=%s enabled=%d running=%d checks=%lu success=%lu fail=%lu interval_s=%lu "
           "url='%s' last_error='%s' last_check_s=%llu last_success_s=%llu",
           state_name(g_state), g_cfg.enabled ? 1 : 0, (g_state == OtaState::kChecking) ? 1 : 0,
           static_cast<unsigned long>(g_rt.total_checks), static_cast<unsigned long>(g_rt.success_count),
           static_cast<unsigned long>(g_rt.failure_count), static_cast<unsigned long>(g_cfg.check_interval_s), g_cfg.url,
           g_rt.last_error, static_cast<unsigned long long>(g_rt.last_check_s),
           static_cast<unsigned long long>(g_rt.last_success_s));
}

bool ota_is_enabled() {
  return g_cfg.enabled;
}

bool ota_is_running() {
  return g_state == OtaState::kChecking;
}

const char* ota_state_name() {
  return state_name_impl();
}

void ota_request_check() {
  if (!g_cfg.enabled) {
    return;
  }
  portENTER_CRITICAL(&g_state_lock);
  g_rt.check_requested = true;
  g_rt.has_request_url = false;
  g_rt.request_url[0] = '\0';
  portEXIT_CRITICAL(&g_state_lock);
}

void ota_request_check_with_url(const char* url) {
  if (!g_cfg.enabled) {
    return;
  }

  portENTER_CRITICAL(&g_state_lock);
  g_rt.check_requested = true;
  g_rt.has_request_url = (url != nullptr && *url != '\0');
  if (g_rt.has_request_url) {
    std::snprintf(g_rt.request_url.data(), g_rt.request_url.size(), "%s", url);
  } else {
    g_rt.request_url[0] = '\0';
  }
  portEXIT_CRITICAL(&g_state_lock);
}

void ota_build_status_payload(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  std::snprintf(out, out_size,
               "{\"enabled\":%s,\"state\":\"%s\",\"running\":%s,\"checks\":%lu,\"success\":%lu,\"fail\":%lu,"
               "\"interval_s\":%lu,\"url\":\"%s\",\"last_error_code\":%lu,\"last_check_s\":%llu,\"last_success_s\":%llu,"
               "\"last_error\":\"%s\"}",
               g_cfg.enabled ? "true" : "false", state_name(g_state), (g_state == OtaState::kChecking) ? "true" : "false",
               static_cast<unsigned long>(g_rt.total_checks), static_cast<unsigned long>(g_rt.success_count),
               static_cast<unsigned long>(g_rt.failure_count), static_cast<unsigned long>(g_cfg.check_interval_s),
               g_cfg.url, static_cast<unsigned long>(g_rt.last_check_error),
               static_cast<unsigned long long>(g_rt.last_check_s),
               static_cast<unsigned long long>(g_rt.last_success_s), g_rt.last_error);
}

#else

void ota_startup() {}
void ota_status_for_cli() {}
bool ota_is_enabled() {
  return false;
}
bool ota_is_running() {
  return false;
}
const char* ota_state_name() {
  return "DISABLED";
}
void ota_request_check() {}
void ota_request_check_with_url(const char*) {}
void ota_build_status_payload(char* out, std::size_t out_size) {
  if (out && out_size > 0) {
    snprintf(out, out_size, "{\"enabled\":false,\"state\":\"DISABLED\",\"running\":false}");
  }
}

#endif
