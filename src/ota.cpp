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
#include "luce/json_writer.h"
#include "luce/net_wifi.h"
#include "luce/nvs_helpers.h"
#include "luce/pki.h"
#include "luce/runtime_state.h"
#include "luce/str_utils.h"
#include "luce/task_budgets.h"
#include "luce/tls_material.h"

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
constexpr std::size_t kCaPemBufferSize = 4096;
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
  char ca_pem_source[16] = {};
  char ca_pem[kCaPemBufferSize] = {};
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
  std::array<char, kUrlBufferSize> request_url{};
};

OtaConfig g_cfg{};
OtaRuntime g_rt{};
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

const char* state_name_impl() { return state_name(g_state); }

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

bool configure_ota_tls(esp_http_client_config_t& http_cfg, const char* url) {
  if (!luce::str::starts_with(url, "https://")) {
    return true;
  }

  http_cfg.skip_cert_common_name_check = false;
  if (std::strcmp(g_cfg.ca_pem_source, "nvs") == 0) {
    const esp_err_t ca_err = luce::tls::load_ca_pem_from_nvs(kOtaNs, "ca_pem", g_cfg.ca_pem_source,
                                                             g_cfg.ca_pem, sizeof(g_cfg.ca_pem));
    if (ca_err != ESP_OK) {
      ESP_LOGE(kTag, "[OTA][TLS] failed to load ota/ca_pem from NVS rc=0x%x",
               static_cast<unsigned>(ca_err));
      return false;
    }
    http_cfg.cert_pem = g_cfg.ca_pem;
    http_cfg.cert_len = 0;
    ESP_LOGI(kTag, "[OTA][TLS] server verification via ota/ca_pem");
    const luce::pki::Status identity = luce::pki::get_status(luce::pki::Role::kOtaClient);
    if (identity.state == luce::pki::State::kActive) {
      http_cfg.client_cert_pem = luce::pki::cert_pem_for_tls(luce::pki::Role::kOtaClient);
      http_cfg.client_key_pem = luce::pki::private_key_pem_for_tls(luce::pki::Role::kOtaClient);
      ESP_LOGI(kTag, "[OTA][TLS] client identity role=%s fingerprint=%s", identity.role,
               identity.fingerprint[0] != '\0' ? identity.fingerprint : "n/a");
    } else if (identity.key_present || identity.cert_present || identity.staged_present) {
      ESP_LOGW(kTag, "[OTA][TLS] client identity dormant state=%s error=%s",
               luce::pki::state_name(identity.state), identity.last_error);
    }
    return true;
  }

  ESP_LOGE(kTag, "[OTA][TLS] unsupported ota/ca_pem_source='%s'", g_cfg.ca_pem_source);
  return false;
}

void load_ota_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.enabled = false;
  g_cfg.check_interval_s = kDefaultCheckIntervalS;
  g_cfg.request_timeout_ms = kDefaultRequestTimeoutMs;
  g_cfg.url[0] = '\0';
  std::snprintf(g_cfg.ca_pem_source, sizeof(g_cfg.ca_pem_source), "nvs");

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
  bool found_ca_source = false;
  bool found_interval = false;
  bool found_timeout = false;

  found_enabled = luce::nvs::read_u8(handle, "enabled", enabled, 0);
  g_cfg.enabled = (enabled != 0);
  luce::nvs::log_nvs_u8(kTag, "enabled", enabled, found_enabled, 0);

  found_url = luce::nvs::read_string(handle, "url", g_cfg.url, sizeof(g_cfg.url), "");
  luce::nvs::log_nvs_string(kTag, "url", g_cfg.url, found_url, "", true);

  found_ca_source = luce::nvs::read_string(handle, "ca_pem_source", g_cfg.ca_pem_source,
                                           sizeof(g_cfg.ca_pem_source), "nvs");
  luce::nvs::log_nvs_string(kTag, "ca_pem_source", g_cfg.ca_pem_source, found_ca_source, "nvs",
                            true);

  found_interval =
      luce::nvs::read_u32(handle, "check_interval_s", check_interval, kDefaultCheckIntervalS);
  if (found_interval) {
    g_cfg.check_interval_s = luce::runtime::clamp_u32(check_interval, 0u, kMaxCheckIntervalS);
  } else {
    g_cfg.check_interval_s = kDefaultCheckIntervalS;
  }
  luce::nvs::log_nvs_u32(kTag, "check_interval_s", g_cfg.check_interval_s, found_interval,
                         g_cfg.check_interval_s);

  found_timeout =
      luce::nvs::read_u32(handle, "request_timeout_ms", timeout_ms, kDefaultRequestTimeoutMs);
  if (found_timeout) {
    g_cfg.request_timeout_ms = luce::runtime::clamp_u32(timeout_ms, 1000u, 60000u);
  } else {
    g_cfg.request_timeout_ms = kDefaultRequestTimeoutMs;
  }
  luce::nvs::log_nvs_u32(kTag, "request_timeout_ms", g_cfg.request_timeout_ms, found_timeout,
                         g_cfg.request_timeout_ms);
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

  esp_http_client_config_t http_cfg{};
  http_cfg.url = url;
  http_cfg.timeout_ms = g_cfg.request_timeout_ms;
  http_cfg.keep_alive_enable = true;
  if (!configure_ota_tls(http_cfg, url)) {
    ++g_rt.failure_count;
    g_rt.last_check_error = ESP_ERR_INVALID_ARG;
    set_last_error("invalid tls config");
    set_state(OtaState::kInvalidConfig, "tls_config");
    return false;
  }

  esp_https_ota_config_t ota_cfg{};
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

void apply_update_request(const std::array<char, kUrlBufferSize>& request_url,
                          bool has_request_url) {
  const char* const source =
      (has_request_url && request_url[0] != '\0') ? request_url.data() : g_cfg.url;
  (void)perform_ota(source);
}

void consume_check_request(bool* had_request, std::array<char, kUrlBufferSize>& request_url,
                           bool* has_request_url) {
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
  return g_cfg.check_interval_s > 0 && g_next_periodic_check_tick != 0 &&
         now >= g_next_periodic_check_tick;
}

void schedule_periodic(bool include_padding) {
  if (g_cfg.check_interval_s == 0) {
    g_next_periodic_check_tick = 0;
    return;
  }
  const TickType_t delay = pdMS_TO_TICKS(g_cfg.check_interval_s * 1000u);
  const TickType_t padding = include_padding ? pdMS_TO_TICKS(kPeriodicTickPaddingMs) : 0;
  g_next_periodic_check_tick = xTaskGetTickCount() + delay + padding;
}

void ota_loop(void*) {
  load_ota_config();

  if (g_cfg.enabled) {
    g_next_periodic_check_tick = 0;
    schedule_periodic(false);
  } else {
    g_next_periodic_check_tick = 0;
  }

  for (;;) {
    const TickType_t now = xTaskGetTickCount();
    std::array<char, kUrlBufferSize> request_url{};
    bool has_request = false;
    bool has_request_url = false;

    if (!g_cfg.enabled) {
      set_state(OtaState::kDisabled, "disabled");
      vTaskDelay(pdMS_TO_TICKS(1000));
      load_ota_config();
      if (g_cfg.enabled && g_next_periodic_check_tick == 0) {
        schedule_periodic(true);
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
      schedule_periodic(true);
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
      schedule_periodic(true);
      vTaskDelay(pdMS_TO_TICKS(kDefaultPollMs));
      continue;
    }

    if (g_state != OtaState::kChecking) {
      set_state(OtaState::kIdle, "running");
    }
    vTaskDelay(pdMS_TO_TICKS(kDefaultPollMs));
  }
}

} // namespace

void ota_startup() {
  if (g_task == nullptr) {
    (void)luce::start_task_once(g_task, ota_loop, luce::task_budget::kTaskOta);
  }
}

void ota_status_for_cli() {
  const luce::pki::Status identity = luce::pki::get_status(luce::pki::Role::kOtaClient);
  ESP_LOGI(
      kTag,
      "ota.status state=%s enabled=%d running=%d checks=%lu success=%lu fail=%lu interval_s=%lu "
      "url='%s' ca_source=%s ca_present=%d client_identity=%s client_cert_present=%d "
      "last_error='%s' last_check_s=%llu last_success_s=%llu",
      state_name(g_state), g_cfg.enabled ? 1 : 0, (g_state == OtaState::kChecking) ? 1 : 0,
      static_cast<unsigned long>(g_rt.total_checks), static_cast<unsigned long>(g_rt.success_count),
      static_cast<unsigned long>(g_rt.failure_count),
      static_cast<unsigned long>(g_cfg.check_interval_s), g_cfg.url, g_cfg.ca_pem_source,
      g_cfg.ca_pem[0] != '\0' ? 1 : 0, luce::pki::state_name(identity.state),
      identity.cert_present ? 1 : 0, g_rt.last_error,
      static_cast<unsigned long long>(g_rt.last_check_s),
      static_cast<unsigned long long>(g_rt.last_success_s));
}

bool ota_is_enabled() { return g_cfg.enabled; }

bool ota_is_running() { return g_state == OtaState::kChecking; }

const char* ota_state_name() { return state_name_impl(); }

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
  luce::json::Writer writer(out, out_size);
  writer.begin_object();
  writer.key_bool("enabled", g_cfg.enabled);
  writer.key_str("state", state_name(g_state));
  writer.key_bool("running", g_state == OtaState::kChecking);
  writer.key_uint("checks", g_rt.total_checks);
  writer.key_uint("success", g_rt.success_count);
  writer.key_uint("fail", g_rt.failure_count);
  writer.key_uint("interval_s", g_cfg.check_interval_s);
  writer.key_str("url", g_cfg.url);
  writer.key_uint("last_error_code", g_rt.last_check_error);
  writer.key_uint("last_check_s", static_cast<std::uint32_t>(g_rt.last_check_s));
  writer.key_uint("last_success_s", static_cast<std::uint32_t>(g_rt.last_success_s));
  writer.key_str("last_error", g_rt.last_error);
  writer.end_object();
}

#else

void ota_startup() {}
void ota_status_for_cli() {}
bool ota_is_enabled() { return false; }
bool ota_is_running() { return false; }
const char* ota_state_name() { return "DISABLED"; }
void ota_request_check() {}
void ota_request_check_with_url(const char*) {}
void ota_build_status_payload(char* out, std::size_t out_size) {
  if (out && out_size > 0) {
    snprintf(out, out_size, "{\"enabled\":false,\"state\":\"DISABLED\",\"running\":false}");
  }
}

#endif
