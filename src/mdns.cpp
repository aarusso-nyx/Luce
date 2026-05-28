// mDNS responder — espressif/mdns managed component.
//
// Previously this file used lwIP's built-in mdns_resp_* responder
// (compiled inline via src/CMakeLists.txt). That worked but coupled
// the project to lwIP API drift; on the v6.0.1 upgrade lwIP renamed
// netif_alloc_client_data_id and we had to flip a Kconfig to keep it
// compiling. Switch to the supported espressif/mdns component fetched
// via idf_component.yml.
#include "luce/mdns.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if LUCE_HAS_MDNS

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"

#include "luce/net_wifi.h"
#include "luce/nvs_helpers.h"
#include "luce_build.h"
#include "luce/runtime_state.h"
#include "luce/task_budgets.h"

namespace {

constexpr const char* kTag = "[mDNS]";
constexpr const char* kMdnsNs = "mdns";
constexpr const char* kNetNs = "net";
constexpr const char* kDefaultInstance = "Luce Strategy";
constexpr const char* kDefaultHostnameFallback = "luce-";
constexpr const char* kServiceType = "_luce";
constexpr const char* kServiceProto = "_tcp";
constexpr std::uint32_t kPollPeriodMs = 1000;
constexpr std::uint16_t kDefaultPort = 80;
constexpr std::size_t kTxtFieldMax = 64;

enum class MdnsState : std::uint8_t {
  kDisabledByConfig = 0,
  kInit,
  kStarted,
  kFailed,
};

struct MdnsConfig {
  bool enabled = false;
  char instance[33] = {};
};

MdnsConfig g_cfg {};
MdnsState g_state = MdnsState::kDisabledByConfig;
char g_hostname[33] = {0};
std::uint16_t g_port = kDefaultPort;
TaskHandle_t g_task_handle = nullptr;
bool g_stack_inited = false;
bool g_service_registered = false;

// Static value buffers for the four TXT records. espressif/mdns wants
// {key, value} pairs as plain strings (it adds the `=` on the wire),
// rather than the lwip responder's "key=value" pre-formatted strings.
char g_txt_fw_value[kTxtFieldMax] = {};
char g_txt_strategy_value[kTxtFieldMax] = {};
char g_txt_device_value[kTxtFieldMax] = {};
char g_txt_build_value[kTxtFieldMax] = {};

const char* state_name(MdnsState state) {
  switch (state) {
    case MdnsState::kDisabledByConfig:
      return "DISABLED";
    case MdnsState::kInit:
      return "INIT";
    case MdnsState::kStarted:
      return "STARTED";
    case MdnsState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

const char* mdns_state_name_impl() {
  return state_name(g_state);
}

void set_state(MdnsState next, const char* reason = nullptr) {
  luce::runtime::set_state(g_state, next, state_name, "[mDNS]", reason);
}

void refresh_mdns_txt_values() {
  std::snprintf(g_txt_fw_value, sizeof(g_txt_fw_value), "%s", LUCE_PROJECT_VERSION);
  std::snprintf(g_txt_strategy_value, sizeof(g_txt_strategy_value), "%s", LUCE_STRATEGY_NAME);
  std::snprintf(g_txt_device_value, sizeof(g_txt_device_value), "%s",
                g_hostname[0] != '\0' ? g_hostname : "luce-device");
  std::snprintf(g_txt_build_value, sizeof(g_txt_build_value), "%s", __DATE__ " " __TIME__);
}

void load_hostname(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  std::memset(out, 0, out_size);
  bool got_name = false;

  nvs_handle_t handle = 0;
  if (nvs_open(kNetNs, NVS_READONLY, &handle) == ESP_OK) {
    if (luce::nvs::read_string(handle, "hostname", out, out_size, "")) {
      got_name = (out[0] != '\0');
    }
    nvs_close(handle);
  }

  if (got_name) {
    return;
  }

  std::uint8_t mac[6] = {};
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    std::snprintf(out, out_size, "%s%02X%02X", kDefaultHostnameFallback, mac[4], mac[5]);
  } else {
    std::snprintf(out, out_size, "luce-device");
  }
}

void load_mdns_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  std::snprintf(g_cfg.instance, sizeof(g_cfg.instance), "%s", kDefaultInstance);
  g_cfg.enabled = false;

  nvs_handle_t handle = 0;
  if (nvs_open(kMdnsNs, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "[mDNS] config namespace '%s' not found; defaults active", kMdnsNs);
    g_port = kDefaultPort;
    g_cfg.enabled = false;
    std::snprintf(g_hostname, sizeof(g_hostname), "%s", kDefaultHostnameFallback);
    load_hostname(g_hostname, sizeof(g_hostname));
    set_state(MdnsState::kDisabledByConfig, "namespace_missing");
    return;
  }

  std::uint8_t enabled = 0;
  (void)luce::nvs::read_u8(handle, "enabled", enabled, 0);
  g_cfg.enabled = (enabled != 0);
  (void)luce::nvs::read_string(handle, "instance", g_cfg.instance, sizeof(g_cfg.instance), kDefaultInstance);
  std::uint16_t configured_port = kDefaultPort;
  (void)luce::nvs::read_u16(handle, "port", configured_port, kDefaultPort);
  nvs_close(handle);

  g_port = configured_port != 0 ? configured_port : kDefaultPort;
  load_hostname(g_hostname, sizeof(g_hostname));
  refresh_mdns_txt_values();

  ESP_LOGI(kTag, "[mDNS] enabled=%d instance='%s' port=%u", g_cfg.enabled ? 1 : 0,
           g_cfg.instance[0] != '\0' ? g_cfg.instance : kDefaultInstance, g_port);
  if (g_cfg.enabled) {
    set_state(MdnsState::kInit, "config_loaded");
  } else {
    set_state(MdnsState::kDisabledByConfig, "config_disabled");
  }
}

// Bring up the stack + service. Pulled into an esp_err_t helper so the
// step list is linear and the void caller (start_mdns_service) only
// owns lifecycle bookkeeping.
esp_err_t start_mdns_stack_and_service() {
  if (!g_stack_inited) {
    ESP_RETURN_ON_ERROR(mdns_init(), kTag, "mdns_init");
    g_stack_inited = true;
  }
  ESP_RETURN_ON_ERROR(mdns_hostname_set(g_hostname), kTag, "mdns_hostname_set");
  ESP_RETURN_ON_ERROR(mdns_instance_name_set(g_cfg.instance), kTag, "mdns_instance_name_set");

  refresh_mdns_txt_values();
  mdns_txt_item_t items[] = {
      {"fw", g_txt_fw_value},
      {"strategy", g_txt_strategy_value},
      {"device", g_txt_device_value},
      {"build", g_txt_build_value},
  };
  ESP_RETURN_ON_ERROR(
      mdns_service_add(g_cfg.instance, kServiceType, kServiceProto, g_port, items,
                       sizeof(items) / sizeof(items[0])),
      kTag, "mdns_service_add");
  return ESP_OK;
}

void start_mdns_service() {
  if (!g_cfg.enabled || g_service_registered) {
    return;
  }

  if (!wifi_is_ip_ready()) {
    set_state(MdnsState::kInit, "waiting_ip");
    return;
  }

  const esp_err_t err = start_mdns_stack_and_service();
  if (err != ESP_OK) {
    set_state(MdnsState::kFailed, esp_err_to_name(err));
    return;
  }

  g_service_registered = true;
  set_state(MdnsState::kStarted, "started");
  ESP_LOGI(kTag, "[mDNS] started hostname=%s instance=%s port=%u", g_hostname, g_cfg.instance, g_port);
}

void stop_mdns_service() {
  if (!g_service_registered) {
    return;
  }
  const esp_err_t err = mdns_service_remove(kServiceType, kServiceProto);
  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
    ESP_LOGW(kTag, "[mDNS] mdns_service_remove failed: %s", esp_err_to_name(err));
  }
  g_service_registered = false;
  set_state(MdnsState::kInit, "stopped");
}

void mdns_task(void*) {
  for (;;) {
    if (!g_cfg.enabled) {
      if (g_service_registered) {
        stop_mdns_service();
      }
      set_state(MdnsState::kDisabledByConfig, "disabled");
      vTaskDelay(pdMS_TO_TICKS(kPollPeriodMs));
      continue;
    }

    if (!wifi_is_ip_ready() && g_service_registered) {
      stop_mdns_service();
    } else if (wifi_is_ip_ready() && !g_service_registered) {
      start_mdns_service();
    } else if (wifi_is_ip_ready() && g_service_registered) {
      set_state(MdnsState::kStarted, "running");
    }

    vTaskDelay(pdMS_TO_TICKS(kPollPeriodMs));
  }
}

}  // namespace

const char* mdns_state_name() {
  return mdns_state_name_impl();
}

bool mdns_is_enabled() {
  return g_cfg.enabled;
}

bool mdns_is_running() {
  return g_service_registered;
}

void mdns_startup() {
  load_mdns_config();
  if (g_task_handle == nullptr) {
    (void)luce::start_task_once(g_task_handle, mdns_task, luce::task_budget::kTaskMdns);
  }
}

void mdns_status_for_cli() {
  char wifi_ip[16] = {0};
  if (wifi_is_ip_ready()) {
    wifi_copy_ip_str(wifi_ip, sizeof(wifi_ip));
  } else {
    std::snprintf(wifi_ip, sizeof(wifi_ip), "n/a");
  }

  ESP_LOGI(kTag,
           "mdns.status state=%s enabled=%d hostname=%s instance=%s service=%s wifi_ip=%s fw=%s strategy=%s",
           state_name(g_state), g_cfg.enabled ? 1 : 0, g_hostname[0] != '\0' ? g_hostname : "(unset)",
           g_cfg.instance[0] != '\0' ? g_cfg.instance : kDefaultInstance,
           g_service_registered ? "1" : "0", wifi_ip, LUCE_PROJECT_VERSION, LUCE_STRATEGY_NAME);
}

#else

bool mdns_is_enabled() {
  return false;
}

bool mdns_is_running() {
  return false;
}

void mdns_startup() {}
void mdns_status_for_cli() {}

#endif  // LUCE_HAS_MDNS
