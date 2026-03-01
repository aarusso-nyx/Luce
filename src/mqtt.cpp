// MQTT publish telemetry implementation.
#include "luce/mqtt.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if LUCE_HAS_MQTT

#include "luce/nvs_helpers.h"
#include "luce/runtime_state.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "luce/net_wifi.h"
#include "luce/i2c_io.h"
#include "luce_build.h"
#include "luce/task_budgets.h"

namespace {

constexpr const char* kTag = "[MQTT]";
constexpr const char* kMqttNs = "mqtt";
constexpr std::uint32_t kPublishIntervalMs = 30000;
constexpr std::uint32_t kBackoffMinMs = 30000;
constexpr std::uint32_t kBackoffMaxMs = 60000;
constexpr const char* kMqttNvsTag = "[MQTT][NVS]";
constexpr std::uint16_t kPayloadBufferBytes = 256;
constexpr std::uint16_t kTopicSuffixBufferBytes = 128;

enum class MqttState : std::uint8_t {
  kDisabled = 0,
  kInitialized,
  kConnecting,
  kConnected,
  kBackoff,
  kFailed,
};

struct MqttConfig {
  bool enabled = false;
  char uri[128] = {};
  char client_id[48] = {};
  char base_topic[48] = {};
  char username[64] = {};
  char password[64] = {};
  bool tls_enabled = false;
  char ca_pem_source[16] = {};
  std::uint32_t qos = 0;
  std::uint32_t keepalive_s = 120;
};

MqttConfig g_cfg {};
MqttState g_state = MqttState::kDisabled;
TaskHandle_t g_task = nullptr;
esp_mqtt_client_handle_t g_client = nullptr;
std::uint32_t g_connect_count = 0;
std::uint32_t g_publish_count = 0;
std::uint32_t g_reconnect_count = 0;
TickType_t g_next_retry_tick = 0;
std::uint32_t g_backoff_ms = 0;
bool g_connected = false;
std::uint32_t g_last_rcvd = 0;

const char* state_name(MqttState state) {
  switch (state) {
    case MqttState::kDisabled:
      return "DISABLED";
    case MqttState::kInitialized:
      return "INITIALIZED";
    case MqttState::kConnecting:
      return "CONNECTING";
    case MqttState::kConnected:
      return "CONNECTED";
    case MqttState::kBackoff:
      return "BACKOFF";
    case MqttState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

const char* mqtt_state_name_impl() {
  return state_name(g_state);
}

void set_state(MqttState next, const char* reason = nullptr) {
  luce::runtime::set_state(g_state, next, state_name, "[MQTT][LIFECYCLE]", reason);
}

void load_mqtt_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  std::snprintf(g_cfg.uri, sizeof(g_cfg.uri), "mqtt://localhost:1883");
  std::snprintf(g_cfg.base_topic, sizeof(g_cfg.base_topic), "luce/net1");
  g_cfg.qos = 0;
  g_cfg.keepalive_s = 120;
  g_cfg.enabled = false;

  nvs_handle_t handle = 0;
  if (nvs_open(kMqttNs, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT] namespace '%s' not found; defaults active", kMqttNs);
    set_state(MqttState::kDisabled, "namespace_missing");
    return;
  }

  std::uint8_t enabled = 0;
  bool f_enabled = false;
  f_enabled = luce::nvs::read_u8(handle, "enabled", enabled, 0);
  g_cfg.enabled = (enabled != 0);
  luce::nvs::log_nvs_u8(kMqttNvsTag, "enabled", enabled, f_enabled, 0);

  bool f_uri = false;
  bool f_client = false;
  bool f_base = false;
  bool f_user = false;
  bool f_pass = false;
  bool f_ca = false;
  bool f_tls = false;
  bool f_qos = false;
  bool f_keepalive = false;
  std::uint32_t u32 = 0;
  std::uint8_t tls = 0;
  f_uri = luce::nvs::read_string(handle, "uri", g_cfg.uri, sizeof(g_cfg.uri), "mqtt://localhost:1883");
  f_client = luce::nvs::read_string(handle, "client_id", g_cfg.client_id, sizeof(g_cfg.client_id), "");
  f_base = luce::nvs::read_string(handle, "base_topic", g_cfg.base_topic, sizeof(g_cfg.base_topic), "luce/net1");
  f_user = luce::nvs::read_string(handle, "username", g_cfg.username, sizeof(g_cfg.username), "");
  f_pass = luce::nvs::read_string(handle, "password", g_cfg.password, sizeof(g_cfg.password), "");
  f_ca = luce::nvs::read_string(handle, "ca_pem_source", g_cfg.ca_pem_source, sizeof(g_cfg.ca_pem_source), "embedded");
  f_tls = luce::nvs::read_u8(handle, "tls_enabled", tls, 0);
  g_cfg.tls_enabled = (tls != 0);
  f_qos = luce::nvs::read_u32(handle, "qos", u32, 0);
  if (f_qos) {
    g_cfg.qos = luce::runtime::clamp_u32(u32, 0u, 1u);
  }
  f_keepalive = luce::nvs::read_u32(handle, "keepalive_s", u32, 120);
  if (f_keepalive) {
    g_cfg.keepalive_s = luce::runtime::clamp_u32(u32, 30u, 7200u);
  }
  nvs_close(handle);

  luce::nvs::log_nvs_string(kMqttNvsTag, "uri", g_cfg.uri, f_uri, "mqtt://localhost:1883", true);
  luce::nvs::log_nvs_string(kMqttNvsTag, "client_id", g_cfg.client_id, f_client, "", true);
  luce::nvs::log_nvs_string(kMqttNvsTag, "base_topic", g_cfg.base_topic, f_base, "luce/net1", true);
  luce::nvs::log_nvs_string(kMqttNvsTag, "username", g_cfg.username, f_user, "", true);
  luce::nvs::log_nvs_string(kMqttNvsTag, "password", g_cfg.password, f_pass, "", true, true);
  luce::nvs::log_nvs_string(kMqttNvsTag, "ca_pem_source", g_cfg.ca_pem_source, f_ca, "embedded", true);
  luce::nvs::log_nvs_u8(kMqttNvsTag, "tls_enabled", tls, f_tls, 0);
  luce::nvs::log_nvs_u32(kMqttNvsTag, "qos", g_cfg.qos, f_qos, g_cfg.qos);
  luce::nvs::log_nvs_u32(kMqttNvsTag, "keepalive_s", g_cfg.keepalive_s, f_keepalive, g_cfg.keepalive_s);

  ESP_LOGI(kTag, "[MQTT][NVS] enabled=%d uri=%s base_topic=%s tls=%d qos=%lu keepalive_s=%lu", g_cfg.enabled ? 1 : 0,
           g_cfg.uri, g_cfg.base_topic, g_cfg.tls_enabled ? 1 : 0, static_cast<unsigned long>(g_cfg.qos),
           static_cast<unsigned long>(g_cfg.keepalive_s));
  if (g_cfg.enabled) {
    set_state(MqttState::kInitialized, "config_enabled");
  } else {
    set_state(MqttState::kDisabled, "config_disabled");
  }
}

void schedule_backoff() {
  if (g_backoff_ms == 0) {
    g_backoff_ms = kBackoffMinMs;
  } else {
    g_backoff_ms = g_backoff_ms * 2;
    if (g_backoff_ms > kBackoffMaxMs) {
      g_backoff_ms = kBackoffMaxMs;
    }
  }
  g_next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(g_backoff_ms);
  set_state(MqttState::kBackoff, "backoff");
  ESP_LOGW(kTag, "[MQTT][BACKOFF] delay_ms=%lu", static_cast<unsigned long>(g_backoff_ms));
}

void setup_client() {
  if (g_client != nullptr) {
    esp_mqtt_client_destroy(g_client);
    g_client = nullptr;
  }

  esp_mqtt_client_config_t client_cfg {};
  client_cfg.broker.address.uri = g_cfg.uri;
  client_cfg.credentials.client_id = g_cfg.client_id;
  client_cfg.credentials.username = g_cfg.username[0] != '\0' ? g_cfg.username : nullptr;
  client_cfg.credentials.authentication.password = g_cfg.password[0] != '\0' ? g_cfg.password : nullptr;
  client_cfg.network.disable_auto_reconnect = true;
  client_cfg.session.disable_clean_session = false;
  client_cfg.session.keepalive = g_cfg.keepalive_s;

  if (g_cfg.keepalive_s != 0) {
    client_cfg.network.timeout_ms = 3000;
  }

  g_client = esp_mqtt_client_init(&client_cfg);
  if (g_client == nullptr) {
    set_state(MqttState::kFailed, "client_init");
    return;
  }

  esp_err_t reg = esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY,
                                                 [](void* handler_arg, esp_event_base_t event_base, int32_t event_id,
                                                    void* event_data) {
    auto* event = static_cast<esp_mqtt_event_t*>(event_data);
    switch (event_id) {
      case MQTT_EVENT_CONNECTED:
        g_connected = true;
        ++g_connect_count;
        g_backoff_ms = 0;
        g_reconnect_count = 0;
        set_state(MqttState::kConnected, "connected");
        ESP_LOGI(kTag, "[MQTT][EVENT] connected");
        break;
      case MQTT_EVENT_DISCONNECTED:
        g_connected = false;
        g_last_rcvd = 0;
        ++g_reconnect_count;
        ESP_LOGW(kTag, "[MQTT][EVENT] disconnected");
        schedule_backoff();
        break;
      case MQTT_EVENT_ERROR:
        g_connected = false;
        set_state(MqttState::kBackoff, "error");
        ESP_LOGW(kTag, "[MQTT][EVENT] error");
        schedule_backoff();
        break;
      default:
        break;
    }
    (void)handler_arg;
    (void)event_base;
    (void)event;
  }, nullptr);

  if (reg != ESP_OK) {
    set_state(MqttState::kFailed, "register_event");
  }
}

int publish_with_topic_suffix(const char* topic_suffix, const char* payload, std::size_t payload_len = 0) {
  if (!g_client || !g_connected || topic_suffix == nullptr || *topic_suffix == '\0' || payload == nullptr) {
    return -1;
  }
  char topic[kTopicSuffixBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "%s/%s", g_cfg.base_topic, topic_suffix);

  const std::size_t resolved_len = payload_len == 0 ? std::strlen(payload) : payload_len;
  return esp_mqtt_client_publish(g_client, topic, payload, resolved_len, g_cfg.qos, 0);
}

void publish_state() {
  if (!g_client || !g_connected) {
    return;
  }
  char payload[kPayloadBufferBytes] = {0};
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  int wifi_rssi = 0;
  wifi_get_rssi(&wifi_rssi);

  std::snprintf(payload, sizeof(payload),
               "{\"fw\":\"%s\",\"strategy\":\"%s\",\"ip\":\"%s\",\"relay\":%u,\"buttons\":%u,\"wifi_rssi\":%d,"
               "\"connected\":true}",
               LUCE_PROJECT_VERSION, LUCE_STRATEGY_NAME, ip[0] != '\0' ? ip : "n/a", static_cast<unsigned>(g_relay_mask),
               static_cast<unsigned>(g_button_mask), wifi_rssi);
  const int rc = publish_with_topic_suffix("telemetry/state", payload);
  if (rc < 0) {
    ESP_LOGW(kTag, "[MQTT][PUB] failed rc=%d", rc);
    return;
  }
  ++g_publish_count;
  ESP_LOGI(kTag, "[MQTT][PUB] topic=%s/telemetry/state bytes=%zu rc=%d", g_cfg.base_topic, std::strlen(payload), rc);
}

void mqtt_loop(void*) {
  while (true) {
    if (!g_cfg.enabled) {
      if (g_client != nullptr) {
        esp_mqtt_client_stop(g_client);
        esp_mqtt_client_destroy(g_client);
        g_client = nullptr;
      }
      set_state(MqttState::kDisabled, "disabled");
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    if (!wifi_is_ip_ready()) {
      set_state(MqttState::kInitialized, "waiting_ip");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (g_state == MqttState::kBackoff && xTaskGetTickCount() < g_next_retry_tick) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if ((g_state == MqttState::kInitialized || g_state == MqttState::kBackoff || g_state == MqttState::kFailed) &&
        g_client == nullptr) {
      setup_client();
      if (g_state != MqttState::kFailed && g_client != nullptr) {
        const esp_err_t start_rc = esp_mqtt_client_start(g_client);
        if (start_rc == ESP_OK) {
          set_state(MqttState::kConnecting, "start");
        } else {
          set_state(MqttState::kFailed, "start_fail");
          schedule_backoff();
        }
      }
    }

    static TickType_t last_publish = 0;
    if (g_connected && (xTaskGetTickCount() - last_publish) > pdMS_TO_TICKS(kPublishIntervalMs)) {
      last_publish = xTaskGetTickCount();
      publish_state();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

const char* mqtt_state_name() {
  return mqtt_state_name_impl();
}

bool mqtt_is_enabled() {
  return g_cfg.enabled;
}

bool mqtt_is_connected() {
  return g_connected;
}

bool mqtt_is_running() {
  return g_connected;
}

void mqtt_startup() {
  load_mqtt_config();
  if (!g_cfg.enabled) {
    return;
  }
  if (g_task == nullptr) {
    (void)luce::start_task_once(g_task, mqtt_loop, luce::task_budget::kTaskMqtt);
  }
}

void mqtt_status_for_cli() {
  ESP_LOGI(kTag, "mqtt.status state=%s enabled=%d connected=%d connect_count=%u publish_count=%u uri=%s qos=%lu keepalive=%lu",
           state_name(g_state), g_cfg.enabled ? 1 : 0, g_connected ? 1 : 0, g_connect_count, g_publish_count, g_cfg.uri,
           static_cast<unsigned long>(g_cfg.qos), static_cast<unsigned long>(g_cfg.keepalive_s));
}

void mqtt_pubtest_for_cli() {
  if (!g_cfg.enabled) {
    ESP_LOGW(kTag, "CLI command mqtt.pubtest: disabled");
    return;
  }
  if (!g_client || !g_connected) {
    ESP_LOGW(kTag, "CLI command mqtt.pubtest: not connected");
    return;
  }
  const char payload[] = "{\"pubtest\":true}";
  const int rc = publish_with_topic_suffix("telemetry/pubtest", payload);
  ESP_LOGI(kTag, "CLI command mqtt.pubtest rc=%d", rc);
}

#else

bool mqtt_is_enabled() {
  return false;
}

bool mqtt_is_connected() {
  return false;
}

bool mqtt_is_running() {
  return false;
}

void mqtt_startup() {}
void mqtt_status_for_cli() {}
void mqtt_pubtest_for_cli() {}

#endif
