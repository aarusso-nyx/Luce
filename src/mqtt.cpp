// MQTT publish telemetry implementation.
#include "luce/mqtt.h"

#include <cinttypes>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if LUCE_HAS_MQTT

#include "luce/nvs_helpers.h"
#include "luce/backoff.h"
#include "luce/json_writer.h"
#include "luce/runtime_state.h"
#include "luce/str_utils.h"
#include "luce/tls_material.h"
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
#include "luce/led_status.h"
#include "luce/pki.h"
#include "luce/task_budgets.h"

namespace {

constexpr const char* kTag = "[MQTT]";
constexpr const char* kMqttNs = "mqtt";
constexpr std::uint32_t kPublishIntervalMs = 30000;
constexpr std::uint32_t kBackoffMinMs = 1000;
constexpr std::uint32_t kBackoffMaxMs = 300000;
constexpr const char* kMqttNvsTag = "[MQTT][NVS]";
constexpr std::uint16_t kPayloadBufferBytes = 256;
constexpr std::uint16_t kPayloadTextBufferBytes = 128;
constexpr std::uint16_t kTopicSuffixBufferBytes = 128;
constexpr std::uint16_t kTopicTextBufferBytes = 128;
constexpr std::size_t kCaPemBufferBytes = 4096;
constexpr std::uint8_t kRelayCount = 8;
constexpr const char* kRelaysNs = "relays";
constexpr const char* kRelaysNightKey = "night_mask";

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
  char ca_pem[kCaPemBufferBytes] = {};
  std::uint32_t qos = 0;
  std::uint32_t keepalive_s = 120;
};

MqttConfig g_cfg{};
MqttState g_state = MqttState::kDisabled;
portMUX_TYPE g_mqtt_state_lock = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t g_task = nullptr;
esp_mqtt_client_handle_t g_client = nullptr;
std::uint32_t g_connect_count = 0;
std::uint32_t g_publish_count = 0;
std::uint32_t g_reconnect_count = 0;
std::uint32_t g_backoff_attempt = 0;
TickType_t g_next_retry_tick = 0;
std::uint32_t g_backoff_ms = 0;
bool g_backoff_scheduled = false;
bool g_connected = false;
std::uint32_t g_last_rcvd = 0;

struct MqttRuntimeSnapshot {
  MqttState state = MqttState::kDisabled;
  bool connected = false;
  bool backoff_scheduled = false;
  esp_mqtt_client_handle_t client = nullptr;
  TickType_t next_retry_tick = 0;
  std::uint32_t backoff_ms = 0;
  std::uint32_t connect_count = 0;
  std::uint32_t publish_count = 0;
  std::uint32_t reconnect_count = 0;
};

int publish_with_topic_suffix(const char* topic_suffix, const char* payload,
                              std::size_t payload_len = 0);
bool mqtt_connected_flag();
MqttRuntimeSnapshot runtime_snapshot();
esp_mqtt_client_handle_t client_snapshot();

bool parse_u32_value(const char* text, std::uint32_t* out_value) {
  return luce::str::parse_u32_token(text, out_value);
}

bool parse_bool_value(const char* text, bool* out_value) {
  if (!text || !out_value || *text == '\0') {
    return false;
  }
  char lowered[kPayloadTextBufferBytes] = {0};
  std::size_t idx = 0;
  for (const char* read = text; *read != '\0' && idx + 1 < sizeof(lowered); ++read) {
    const unsigned char ch = static_cast<unsigned char>(*read);
    if (ch == ' ' || ch == '\t') {
      continue;
    }
    lowered[idx++] = static_cast<char>(std::tolower(ch));
  }
  return luce::str::parse_bool_token(lowered, out_value);
}

bool mqtt_requires_tls() {
  return g_cfg.tls_enabled || luce::str::starts_with(g_cfg.uri, "mqtts://") ||
         luce::str::starts_with(g_cfg.uri, "wss://");
}

bool configure_mqtt_tls(esp_mqtt_client_config_t& client_cfg) {
  if (!mqtt_requires_tls()) {
    return true;
  }

  if (std::strcmp(g_cfg.ca_pem_source, "nvs") == 0) {
    const esp_err_t ca_err = luce::tls::load_ca_pem_from_nvs(kMqttNs, "ca_pem", g_cfg.ca_pem_source,
                                                             g_cfg.ca_pem, sizeof(g_cfg.ca_pem));
    if (ca_err != ESP_OK) {
      ESP_LOGE(kTag, "[MQTT][TLS] failed to load mqtt/ca_pem from NVS rc=0x%x",
               static_cast<unsigned>(ca_err));
      return false;
    }
    client_cfg.broker.verification.certificate = g_cfg.ca_pem;
    client_cfg.broker.verification.certificate_len = 0;
    client_cfg.broker.verification.skip_cert_common_name_check = false;
    ESP_LOGI(kTag, "[MQTT][TLS] broker verification via mqtt/ca_pem");
    const luce::pki::Status identity = luce::pki::get_status(luce::pki::Role::kMqttClient);
    if (identity.state == luce::pki::State::kActive) {
      client_cfg.credentials.authentication.certificate =
          luce::pki::cert_pem_for_tls(luce::pki::Role::kMqttClient);
      client_cfg.credentials.authentication.key =
          luce::pki::private_key_pem_for_tls(luce::pki::Role::kMqttClient);
      ESP_LOGI(kTag, "[MQTT][TLS] client identity role=%s fingerprint=%s", identity.role,
               identity.fingerprint[0] != '\0' ? identity.fingerprint : "n/a");
    } else if (identity.key_present || identity.cert_present || identity.staged_present) {
      ESP_LOGW(kTag, "[MQTT][TLS] client identity dormant state=%s error=%s",
               luce::pki::state_name(identity.state), identity.last_error);
    }
    return true;
  }

  ESP_LOGE(kTag, "[MQTT][TLS] unsupported mqtt/ca_pem_source='%s'", g_cfg.ca_pem_source);
  return false;
}

bool parse_led_manual_mode(const char* text, LedManualMode* out_mode) {
  if (!text || !out_mode) {
    return false;
  }
  if (std::strcmp(text, "auto") == 0 || std::strcmp(text, "AUTO") == 0) {
    *out_mode = LedManualMode::kAuto;
    return true;
  }
  if (std::strcmp(text, "blink") == 0 || std::strcmp(text, "normal") == 0 ||
      std::strcmp(text, "BLINK") == 0 || std::strcmp(text, "NORMAL") == 0) {
    *out_mode = LedManualMode::kBlinkNormal;
    return true;
  }
  if (std::strcmp(text, "fast") == 0 || std::strcmp(text, "FAST") == 0) {
    *out_mode = LedManualMode::kBlinkFast;
    return true;
  }
  if (std::strcmp(text, "slow") == 0 || std::strcmp(text, "SLOW") == 0) {
    *out_mode = LedManualMode::kBlinkSlow;
    return true;
  }
  if (std::strcmp(text, "flash") == 0 || std::strcmp(text, "FLASH") == 0) {
    *out_mode = LedManualMode::kFlash;
    return true;
  }
  bool on = false;
  if (parse_bool_value(text, &on)) {
    *out_mode = on ? LedManualMode::kOn : LedManualMode::kOff;
    return true;
  }
  return false;
}

bool trim_to_buffer(const char* text, char* out, std::size_t out_size) {
  if (!text || !out || out_size == 0) {
    return false;
  }

  std::size_t start = 0;
  while (text[start] != '\0' && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' ||
                                 text[start] == '\n')) {
    ++start;
  }

  std::size_t end = std::strlen(text);
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                         text[end - 1] == '\n')) {
    --end;
  }

  if (end <= start) {
    out[0] = '\0';
    return false;
  }

  const std::size_t text_len = end - start;
  const std::size_t safe_len = (text_len < (out_size - 1)) ? text_len : (out_size - 1);
  std::memcpy(out, text + start, safe_len);
  out[safe_len] = '\0';
  return true;
}

bool nvs_write_u8(const char* ns, const char* key, std::uint8_t value) {
  auto nvs = luce::nvs::Handle::Open(ns, NVS_READWRITE);
  if (!nvs.ok()) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to open namespace '%s' for update", ns);
    return false;
  }
  const esp_err_t set_rc = luce::nvs::write_u8(nvs.raw(), key, value);
  const bool ok = (set_rc == ESP_OK) ? (luce::nvs::commit(nvs.raw()) == ESP_OK) : false;
  if (!ok) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to persist %s/%s=%u", ns, key, static_cast<unsigned>(value));
  }
  return ok;
}

bool nvs_write_u32(const char* ns, const char* key, std::uint32_t value) {
  auto nvs = luce::nvs::Handle::Open(ns, NVS_READWRITE);
  if (!nvs.ok()) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to open namespace '%s' for update", ns);
    return false;
  }
  const esp_err_t set_rc = luce::nvs::write_u32(nvs.raw(), key, value);
  const bool ok = (set_rc == ESP_OK) ? (luce::nvs::commit(nvs.raw()) == ESP_OK) : false;
  if (!ok) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to persist %s/%s=%lu", ns, key,
             static_cast<unsigned long>(value));
  }
  return ok;
}

bool nvs_write_string(const char* ns, const char* key, const char* value, bool mask_value = false) {
  if (!value) {
    return false;
  }
  auto nvs = luce::nvs::Handle::Open(ns, NVS_READWRITE);
  if (!nvs.ok()) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to open namespace '%s' for update", ns);
    return false;
  }
  const esp_err_t set_rc = luce::nvs::write_string(nvs.raw(), key, value);
  const bool ok = (set_rc == ESP_OK) ? (luce::nvs::commit(nvs.raw()) == ESP_OK) : false;
  if (!ok) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to persist %s/%s=%s", ns, key,
             mask_value ? "********" : value);
  }
  return ok;
}

void publish_unsupported_legacy_topic(const char* topic_suffix, const char* reason,
                                      const char* payload) {
  if (!topic_suffix || topic_suffix[0] == '\0') {
    return;
  }

  char body[kPayloadBufferBytes] = {0};
  luce::json::Writer writer(body, sizeof(body));
  writer.begin_object();
  writer.key_str("status", "unsupported");
  writer.key_str("topic", topic_suffix);
  writer.key_str("reason", reason ? reason : "unsupported");
  writer.key_bool("payload_present", payload && payload[0] != '\0');
  writer.end_object();
  (void)publish_with_topic_suffix("compat/unsupported", body);
}

void publish_unsupported_config(const char* subtopic, const char* payload) {
  char topic[kTopicSuffixBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "config/%s", subtopic ? subtopic : "");
  if (payload != nullptr) {
    ESP_LOGW(kTag, "[MQTT][IN] config topic '%s' unsupported or read-only with value '%s'",
             subtopic ? subtopic : "(null)", payload);
  } else {
    ESP_LOGW(kTag, "[MQTT][IN] config topic '%s' unsupported or read-only",
             subtopic ? subtopic : "(null)");
  }
  publish_unsupported_legacy_topic(topic, "unsupported_or_readonly", payload);
}

bool persist_config_bool(const char* ns, const char* key, const char* payload, const char* tag) {
  bool value = false;
  if (!parse_bool_value(payload, &value)) {
    ESP_LOGW(kTag, "[MQTT][IN] %s config value must be bool: %s", tag ? tag : "config",
             payload ? payload : "(null)");
    return false;
  }
  if (!nvs_write_u8(ns, key, value ? 1u : 0u)) {
    return false;
  }
  ESP_LOGI(kTag, "[MQTT][IN] %s set %s/%s=%u (reboot to apply)", tag ? tag : "config", ns, key,
           static_cast<unsigned>(value ? 1u : 0u));
  return true;
}

bool persist_config_u32(const char* ns, const char* key, const char* payload, const char* tag,
                        std::uint32_t min = 0, std::uint32_t max = 0xFFFFFFFFu) {
  std::uint32_t value = 0;
  if (!parse_u32_value(payload, &value)) {
    ESP_LOGW(kTag, "[MQTT][IN] %s config value must be integer: %s", tag ? tag : "config",
             payload ? payload : "(null)");
    return false;
  }
  value = luce::runtime::clamp_u32(value, min, max);
  if (!nvs_write_u32(ns, key, value)) {
    return false;
  }
  ESP_LOGI(kTag, "[MQTT][IN] %s set %s/%s=%lu (reboot to apply)", tag ? tag : "config", ns, key,
           static_cast<unsigned long>(value));
  return true;
}
void copy_text(const char* source, std::size_t length, char* out, std::size_t out_size) {
  if (!source || !out || out_size == 0) {
    return;
  }
  const std::size_t safe_len = (length < (out_size - 1)) ? length : (out_size - 1);
  if (safe_len > 0) {
    std::memcpy(out, source, safe_len);
  }
  out[safe_len] = '\0';
}

void subscribe_control_topics() {
  esp_mqtt_client_handle_t client = client_snapshot();
  if (client == nullptr || g_cfg.base_topic[0] == '\0') {
    return;
  }

  char topic[kTopicTextBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "%s/config/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(client, topic, g_cfg.qos);
  std::snprintf(topic, sizeof(topic), "%s/relays/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(client, topic, g_cfg.qos);
  std::snprintf(topic, sizeof(topic), "%s/sensor/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(client, topic, g_cfg.qos);
  std::snprintf(topic, sizeof(topic), "%s/leds/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(client, topic, g_cfg.qos);
}

void handle_relay_state_index(const char* index_text, const char* payload) {
  std::uint32_t index = 0;
  if (!parse_u32_value(index_text, &index) || index >= kRelayCount) {
    ESP_LOGW(kTag, "[MQTT][IN] invalid relay index '%s' for relays/state",
             index_text ? index_text : "(null)");
    led_status_notify_user_error();
    return;
  }

  bool on = false;
  if (!parse_bool_value(payload, &on)) {
    ESP_LOGW(kTag, "[MQTT][IN] invalid relay value '%s' for relays/state/%u",
             payload ? payload : "(null)", static_cast<unsigned>(index));
    led_status_notify_user_error();
    return;
  }

  if (set_relay_channel_safe(static_cast<int>(index), on) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][IN] relays/state/%u failed (%s)", static_cast<unsigned>(index),
             io_hardware_degraded() ? "hardware_degraded" : "i/o unavailable");
    led_status_notify_user_error();
    return;
  }
  led_status_notify_user_input();
  ESP_LOGI(kTag, "[MQTT][IN] relays/state/%u=%s", static_cast<unsigned>(index), on ? "on" : "off");
}

void handle_relay_state(const char* payload) {
  std::uint32_t mask = 0;
  if (!parse_u32_value(payload, &mask) || mask > 0xFFu) {
    ESP_LOGW(kTag, "[MQTT][IN] invalid relay mask '%s' for relays/state",
             payload ? payload : "(null)");
    led_status_notify_user_error();
    return;
  }

  const std::uint8_t next_mask = static_cast<std::uint8_t>(mask & 0xFFu);
  if (set_relay_mask_safe(next_mask) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][IN] relays/state failed (%s)",
             io_hardware_degraded() ? "hardware_degraded" : "i/o unavailable");
    led_status_notify_user_error();
    return;
  }
  led_status_notify_user_input();
  ESP_LOGI(kTag, "[MQTT][IN] relays/state=%u", static_cast<unsigned>(next_mask));
}

void handle_relay_topic(const char* subtopic, const char* payload) {
  if (std::strcmp(subtopic, "state") == 0) {
    handle_relay_state(payload);
    return;
  }
  if (std::strncmp(subtopic, "state/", 6) == 0) {
    handle_relay_state_index(subtopic + 6, payload);
    return;
  }

  if (std::strncmp(subtopic, "night", 5) == 0) {
    if (std::strcmp(subtopic, "night") == 0) {
      char value_text[kPayloadTextBufferBytes] = {0};
      if (!trim_to_buffer(payload, value_text, sizeof(value_text))) {
        ESP_LOGW(kTag, "[MQTT][IN] relays/night payload empty");
        return;
      }
      std::uint32_t night_mask = 0;
      if (!parse_u32_value(value_text, &night_mask) || night_mask > 0xFFu) {
        ESP_LOGW(kTag, "[MQTT][IN] relays/night invalid mask '%s'", value_text);
        led_status_notify_user_error();
        return;
      }
      io_set_relay_night_mask(static_cast<std::uint8_t>(night_mask));
      ESP_LOGI(kTag, "[MQTT][IN] relays/night set=0x%02lX", static_cast<unsigned long>(night_mask));
      return;
    }

    if (std::strncmp(subtopic, "night/", 6) == 0) {
      char index_text[kTopicSuffixBufferBytes] = {0};
      std::snprintf(index_text, sizeof(index_text), "%s", subtopic + 6);
      std::uint32_t index = 0;
      if (!parse_u32_value(index_text, &index) || index >= kRelayCount) {
        ESP_LOGW(kTag, "[MQTT][IN] relays/night/%s invalid relay index", subtopic + 6);
        led_status_notify_user_error();
        return;
      }
      char value_text[kPayloadTextBufferBytes] = {0};
      if (!trim_to_buffer(payload, value_text, sizeof(value_text))) {
        ESP_LOGW(kTag, "[MQTT][IN] relays/night/%u payload empty", static_cast<unsigned>(index));
        return;
      }
      bool on = false;
      if (!parse_bool_value(value_text, &on)) {
        ESP_LOGW(kTag, "[MQTT][IN] relays/night/%u invalid value '%s'",
                 static_cast<unsigned>(index), value_text);
        led_status_notify_user_error();
        return;
      }
      std::uint8_t mask = io_relay_night_mask();
      if (on) {
        mask |= static_cast<std::uint8_t>(1u << index);
      } else {
        mask &= static_cast<std::uint8_t>(~(1u << index));
      }
      io_set_relay_night_mask(mask);
      ESP_LOGI(kTag, "[MQTT][IN] relays/night/%u=%s (mask=0x%02X)", static_cast<unsigned>(index),
               on ? "on" : "off", static_cast<unsigned>(mask));
      return;
    }

    ESP_LOGW(kTag, "[MQTT][IN] relays/%s ignored (legacy command not implemented)", subtopic);
    char topic[kTopicSuffixBufferBytes] = {0};
    std::snprintf(topic, sizeof(topic), "relays/%s", subtopic);
    publish_unsupported_legacy_topic(topic, "legacy_command_not_implemented", payload);
    return;
  }

  if (payload != nullptr) {
    ESP_LOGW(kTag, "[MQTT][IN] unhandled relays topic '%s' payload='%s'", subtopic, payload);
  } else {
    ESP_LOGW(kTag, "[MQTT][IN] unhandled relays topic '%s'", subtopic);
  }
  char topic[kTopicSuffixBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "relays/%s", subtopic ? subtopic : "");
  publish_unsupported_legacy_topic(topic, "unhandled_relays_topic", payload);
}

enum class ConfigValueKind : std::uint8_t {
  kString,
  kBool,
  kU32,
};

struct ConfigRoute {
  const char* suffix;
  const char* ns;
  const char* key;
  ConfigValueKind kind;
  bool mask_value;
  std::uint32_t min_value;
  std::uint32_t max_value;
};

bool apply_config_route(const ConfigRoute& route, const char* value_text) {
  switch (route.kind) {
  case ConfigValueKind::kString:
    if (nvs_write_string(route.ns, route.key, value_text, route.mask_value)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/%s updated %s/%s%s (reboot to apply)", route.suffix,
               route.ns, route.key, route.mask_value ? " (masked)" : "");
      return true;
    }
    return false;
  case ConfigValueKind::kBool:
    return persist_config_bool(route.ns, route.key, value_text, route.ns);
  case ConfigValueKind::kU32:
    return persist_config_u32(route.ns, route.key, value_text, route.ns, route.min_value,
                              route.max_value);
  default:
    return false;
  }
}

constexpr ConfigRoute kConfigRoutes[] = {
    {"name", "net", "hostname", ConfigValueKind::kString, false, 0u, 0u},
    {"hostname", "net", "hostname", ConfigValueKind::kString, false, 0u, 0u},
    {"ssid", "wifi", "ssid", ConfigValueKind::kString, false, 0u, 0u},
    {"pass", "wifi", "pass", ConfigValueKind::kString, true, 0u, 0u},
    {"ssid2", "wifi", "ssid2", ConfigValueKind::kString, false, 0u, 0u},
    {"pass2", "wifi", "pass2", ConfigValueKind::kString, true, 0u, 0u},
    {"wifi/ssid", "wifi", "ssid", ConfigValueKind::kString, false, 0u, 0u},
    {"wifi/pass", "wifi", "pass", ConfigValueKind::kString, true, 0u, 0u},
    {"logConsoleFmt", "compat", "log_console_fmt", ConfigValueKind::kString, false, 0u, 0u},
    {"logFileFmt", "compat", "log_file_fmt", ConfigValueKind::kString, false, 0u, 0u},
    {"logConsoleLevel", "compat", "log_console_level", ConfigValueKind::kString, false, 0u, 0u},
    {"logFileLevel", "compat", "log_file_level", ConfigValueKind::kString, false, 0u, 0u},
    {"mqtt", "mqtt", "uri", ConfigValueKind::kString, false, 0u, 0u},
    {"mqtt/uri", "mqtt", "uri", ConfigValueKind::kString, false, 0u, 0u},
    {"mqtt/client_id", "mqtt", "client_id", ConfigValueKind::kString, false, 0u, 0u},
    {"mqtt/base_topic", "mqtt", "base_topic", ConfigValueKind::kString, false, 0u, 0u},
    {"mqtt/username", "mqtt", "username", ConfigValueKind::kString, false, 0u, 0u},
    {"mqtt/password", "mqtt", "password", ConfigValueKind::kString, true, 0u, 0u},
    {"mqtt/tls_enabled", "mqtt", "tls_enabled", ConfigValueKind::kBool, false, 0u, 0u},
    {"mqtt/ca_pem_source", "mqtt", "ca_pem_source", ConfigValueKind::kString, false, 0u, 0u},
    {"mqtt/qos", "mqtt", "qos", ConfigValueKind::kU32, false, 0u, 2u},
    {"mqtt/keepalive_s", "mqtt", "keepalive_s", ConfigValueKind::kU32, false, 30u, 7200u},
    {"mdns/instance", "mdns", "instance", ConfigValueKind::kString, false, 0u, 0u},
    {"http/token", "http", "token", ConfigValueKind::kString, true, 0u, 0u},
    {"cli_net/token", "cli_net", "token", ConfigValueKind::kString, true, 0u, 0u},
};

void handle_config_topic(const char* subtopic, const char* payload) {
  if (!subtopic || subtopic[0] == '\0') {
    ESP_LOGW(kTag, "[MQTT][IN] empty config topic");
    return;
  }

  char value_text[kPayloadTextBufferBytes] = {0};
  if (!trim_to_buffer(payload, value_text, sizeof(value_text))) {
    ESP_LOGW(kTag, "[MQTT][IN] config/%s payload empty", subtopic);
    return;
  }

  for (const auto& route : kConfigRoutes) {
    if (std::strcmp(subtopic, route.suffix) == 0) {
      (void)apply_config_route(route, value_text);
      return;
    }
  }

  publish_unsupported_config(subtopic, value_text);
}

void handle_sensor_topic(const char* subtopic, const char* payload) {
  if (std::strcmp(subtopic, "threshold") == 0) {
    char value_text[kPayloadTextBufferBytes] = {0};
    if (trim_to_buffer(payload, value_text, sizeof(value_text))) {
      std::uint32_t threshold = 0u;
      if (parse_u32_value(value_text, &threshold)) {
        if (threshold > 5000u) {
          threshold = 5000u;
        }
        io_set_light_threshold(static_cast<std::uint16_t>(threshold));
        (void)persist_config_u32("sensor", "threshold", value_text, "sensor", 0u, 5000u);
      }
    }
    return;
  }
  ESP_LOGW(kTag, "[MQTT][IN] sensor topic '%s' ignored", subtopic);
  char topic[kTopicSuffixBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "sensor/%s", subtopic ? subtopic : "");
  publish_unsupported_legacy_topic(topic, "unsupported_sensor_topic", payload);
}

void handle_leds_topic(const char* subtopic, const char* payload) {
  if (!subtopic || subtopic[0] == '\0') {
    return;
  }
  if (std::strcmp(subtopic, "state") == 0) {
    char value[kPayloadTextBufferBytes] = {0};
    if (trim_to_buffer(payload, value, sizeof(value))) {
      std::uint32_t mask = 0;
      if (parse_u32_value(value, &mask) && mask <= 0x07u) {
        for (std::uint8_t idx = 0; idx < 3; ++idx) {
          const bool on = ((mask >> idx) & 0x1u) != 0u;
          (void)led_status_set_manual(idx, on);
        }
        ESP_LOGI(kTag, "[MQTT][IN] leds/state=0x%02lX", static_cast<unsigned long>(mask));
      } else {
        LedManualMode mode = LedManualMode::kAuto;
        if (!parse_led_manual_mode(value, &mode)) {
          publish_unsupported_legacy_topic("leds/state", "invalid_led_state_payload", value);
          ESP_LOGW(kTag, "[MQTT][IN] leds/state invalid payload '%s'", value);
          return;
        }
        for (std::uint8_t idx = 0; idx < 3; ++idx) {
          (void)led_status_set_manual_mode(idx, mode);
        }
        ESP_LOGI(kTag, "[MQTT][IN] leds/state mode applied");
      }
      const std::uint8_t current_mask = led_status_current_mask();
      std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(current_mask));
      (void)publish_with_topic_suffix("leds/state", value);
    } else {
      const std::uint8_t current_mask = led_status_current_mask();
      std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(current_mask));
      (void)publish_with_topic_suffix("leds/state", value);
    }
    return;
  }
  if (std::strncmp(subtopic, "state/", 6) == 0) {
    std::uint32_t index = 0;
    if (!parse_u32_value(subtopic + 6, &index) || index > 2) {
      ESP_LOGW(kTag, "[MQTT][IN] leds/state/<idx> index must be 0-2");
      char topic[kTopicSuffixBufferBytes] = {0};
      std::snprintf(topic, sizeof(topic), "leds/%s", subtopic);
      publish_unsupported_legacy_topic(topic, "unsupported_led_index", payload);
      return;
    }
    char value[kPayloadTextBufferBytes] = {0};
    if (trim_to_buffer(payload, value, sizeof(value))) {
      LedManualMode mode = LedManualMode::kAuto;
      if (!parse_led_manual_mode(value, &mode)) {
        char topic[kTopicSuffixBufferBytes] = {0};
        std::snprintf(topic, sizeof(topic), "leds/%s", subtopic);
        publish_unsupported_legacy_topic(topic, "invalid_led_state_payload", value);
        ESP_LOGW(kTag, "[MQTT][IN] leds/state/%lu invalid payload '%s'",
                 static_cast<unsigned long>(index), value);
        return;
      }
      (void)led_status_set_manual_mode(static_cast<std::uint8_t>(index), mode);
      ESP_LOGI(kTag, "[MQTT][IN] leds/state/%lu mode applied", static_cast<unsigned long>(index));
    }
    const std::uint8_t current_mask = led_status_current_mask();
    std::snprintf(value, sizeof(value), "%u",
                  static_cast<unsigned>((current_mask >> index) & 0x1u));
    (void)publish_with_topic_suffix(subtopic, value);
    return;
  }
  ESP_LOGW(kTag, "[MQTT][IN] leds topic '%s' ignored (status-only LED pipeline)", subtopic);
  char topic[kTopicSuffixBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "leds/%s", subtopic ? subtopic : "");
  publish_unsupported_legacy_topic(topic, "status_only_led_pipeline", payload);
}

void dispatch_inbound_message(const char* subtopic, const char* payload) {
  if (!subtopic || subtopic[0] == '\0') {
    return;
  }

  if (std::strncmp(subtopic, "config/", 7) == 0) {
    handle_config_topic(subtopic + 7, payload);
  } else if (std::strncmp(subtopic, "relays/", 7) == 0) {
    handle_relay_topic(subtopic + 7, payload);
  } else if (std::strncmp(subtopic, "sensor/", 7) == 0) {
    handle_sensor_topic(subtopic + 7, payload);
  } else if (std::strncmp(subtopic, "leds/", 5) == 0) {
    handle_leds_topic(subtopic + 5, payload);
  } else {
    ESP_LOGW(kTag, "[MQTT][IN] unhandled topic '%s'", subtopic);
    publish_unsupported_legacy_topic(subtopic, "unhandled_topic_family", payload);
  }
}

void publish_relay_aliases() {
  const MqttRuntimeSnapshot snapshot = runtime_snapshot();
  if (snapshot.client == nullptr || !snapshot.connected) {
    return;
  }
  char payload[kPayloadTextBufferBytes] = {0};
  const std::uint8_t relay_mask = io_relay_mask();
  std::snprintf(payload, sizeof(payload), "%u", static_cast<unsigned>(relay_mask));
  (void)publish_with_topic_suffix("relays/state", payload);

  for (std::uint8_t idx = 0; idx < kRelayCount; ++idx) {
    const std::uint8_t on_mask =
        relay_mask_for_channel_state(static_cast<int>(idx), true, relay_mask);
    const bool on = (on_mask != relay_mask);
    char idx_topic[kTopicTextBufferBytes] = {0};
    std::snprintf(idx_topic, sizeof(idx_topic), "relays/state/%u", static_cast<unsigned>(idx));
    std::snprintf(payload, sizeof(payload), "%d", on ? 1 : 0);
    (void)publish_with_topic_suffix(idx_topic, payload);
  }
}

void publish_sensor_aliases() {
  const MqttRuntimeSnapshot mqtt = runtime_snapshot();
  if (mqtt.client == nullptr || !mqtt.connected) {
    return;
  }
  I2cSensorSnapshot snapshot{};
  (void)read_sensor_snapshot(snapshot);

  char payload[kPayloadTextBufferBytes] = {0};
  std::snprintf(payload, sizeof(payload), "%d", snapshot.light_raw);
  (void)publish_with_topic_suffix("sensor/lighting", payload);
  std::snprintf(payload, sizeof(payload), "%d", snapshot.voltage_raw);
  (void)publish_with_topic_suffix("sensor/voltage", payload);
  std::snprintf(payload, sizeof(payload), "%.2f", static_cast<double>(snapshot.temperature_c));
  (void)publish_with_topic_suffix("sensor/temperature", payload);
  std::snprintf(payload, sizeof(payload), "%.2f", static_cast<double>(snapshot.humidity_percent));
  (void)publish_with_topic_suffix("sensor/humidity", payload);
}

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
  portENTER_CRITICAL(&g_mqtt_state_lock);
  const MqttState state = g_state;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  return state_name(state);
}

void set_state(MqttState next, const char* reason = nullptr) {
  bool changed = false;
  portENTER_CRITICAL(&g_mqtt_state_lock);
  if (g_state != next) {
    g_state = next;
    changed = true;
  }
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  if (!changed) {
    return;
  }
  if (reason && *reason) {
    ESP_LOGI("[MQTT][LIFECYCLE]", "state=%s reason=%s", state_name(next), reason);
  } else {
    ESP_LOGI("[MQTT][LIFECYCLE]", "state=%s", state_name(next));
  }
}

bool mqtt_connected_flag() {
  portENTER_CRITICAL(&g_mqtt_state_lock);
  const bool connected = g_connected;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  return connected;
}

MqttRuntimeSnapshot runtime_snapshot() {
  MqttRuntimeSnapshot snapshot;
  portENTER_CRITICAL(&g_mqtt_state_lock);
  snapshot.state = g_state;
  snapshot.connected = g_connected;
  snapshot.backoff_scheduled = g_backoff_scheduled;
  snapshot.client = g_client;
  snapshot.next_retry_tick = g_next_retry_tick;
  snapshot.backoff_ms = g_backoff_ms;
  snapshot.connect_count = g_connect_count;
  snapshot.publish_count = g_publish_count;
  snapshot.reconnect_count = g_reconnect_count;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  return snapshot;
}

esp_mqtt_client_handle_t client_snapshot() {
  portENTER_CRITICAL(&g_mqtt_state_lock);
  esp_mqtt_client_handle_t client = g_client;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  return client;
}

void set_client(esp_mqtt_client_handle_t client) {
  portENTER_CRITICAL(&g_mqtt_state_lock);
  g_client = client;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
}

esp_mqtt_client_handle_t take_client() {
  portENTER_CRITICAL(&g_mqtt_state_lock);
  esp_mqtt_client_handle_t client = g_client;
  g_client = nullptr;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  return client;
}

void set_connected_flag(bool connected) {
  portENTER_CRITICAL(&g_mqtt_state_lock);
  g_connected = connected;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
}

void note_connected() {
  portENTER_CRITICAL(&g_mqtt_state_lock);
  g_connected = true;
  ++g_connect_count;
  g_backoff_ms = 0;
  g_backoff_attempt = 0;
  g_backoff_scheduled = false;
  g_reconnect_count = 0;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
}

void load_mqtt_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  std::snprintf(g_cfg.uri, sizeof(g_cfg.uri), "mqtt://localhost:1883");
  std::snprintf(g_cfg.base_topic, sizeof(g_cfg.base_topic), "luce/net1");
  std::snprintf(g_cfg.ca_pem_source, sizeof(g_cfg.ca_pem_source), "nvs");
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
  f_uri =
      luce::nvs::read_string(handle, "uri", g_cfg.uri, sizeof(g_cfg.uri), "mqtt://localhost:1883");
  f_client =
      luce::nvs::read_string(handle, "client_id", g_cfg.client_id, sizeof(g_cfg.client_id), "");
  f_base = luce::nvs::read_string(handle, "base_topic", g_cfg.base_topic, sizeof(g_cfg.base_topic),
                                  "luce/net1");
  f_user = luce::nvs::read_string(handle, "username", g_cfg.username, sizeof(g_cfg.username), "");
  f_pass = luce::nvs::read_string(handle, "password", g_cfg.password, sizeof(g_cfg.password), "");
  f_ca = luce::nvs::read_string(handle, "ca_pem_source", g_cfg.ca_pem_source,
                                sizeof(g_cfg.ca_pem_source), "nvs");
  f_tls = luce::nvs::read_u8(handle, "tls_enabled", tls, 0);
  g_cfg.tls_enabled = (tls != 0);
  f_qos = luce::nvs::read_u32(handle, "qos", u32, 0);
  if (f_qos) {
    g_cfg.qos = luce::runtime::clamp_u32(u32, 0u, 2u);
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
  luce::nvs::log_nvs_string(kMqttNvsTag, "ca_pem_source", g_cfg.ca_pem_source, f_ca, "nvs", true);
  luce::nvs::log_nvs_u8(kMqttNvsTag, "tls_enabled", tls, f_tls, 0);
  luce::nvs::log_nvs_u32(kMqttNvsTag, "qos", g_cfg.qos, f_qos, g_cfg.qos);
  luce::nvs::log_nvs_u32(kMqttNvsTag, "keepalive_s", g_cfg.keepalive_s, f_keepalive,
                         g_cfg.keepalive_s);

  ESP_LOGI(kTag, "[MQTT][NVS] enabled=%d uri=%s base_topic=%s tls=%d qos=%lu keepalive_s=%lu",
           g_cfg.enabled ? 1 : 0, g_cfg.uri, g_cfg.base_topic, g_cfg.tls_enabled ? 1 : 0,
           static_cast<unsigned long>(g_cfg.qos), static_cast<unsigned long>(g_cfg.keepalive_s));
  if (g_cfg.enabled) {
    set_state(MqttState::kInitialized, "config_enabled");
  } else {
    set_state(MqttState::kDisabled, "config_disabled");
  }
}

void schedule_backoff() {
  TickType_t retry_tick = 0;
  std::uint32_t delay_ms = 0;
  std::uint32_t attempt = 0;
  portENTER_CRITICAL(&g_mqtt_state_lock);
  if (g_backoff_scheduled) {
    delay_ms = g_backoff_ms;
    retry_tick = g_next_retry_tick;
    attempt = g_backoff_attempt;
    portEXIT_CRITICAL(&g_mqtt_state_lock);
    ESP_LOGW(kTag, "[MQTT][BACKOFF] already scheduled attempt=%lu delay_ms=%lu retry_tick=%lu",
             static_cast<unsigned long>(attempt), static_cast<unsigned long>(delay_ms),
             static_cast<unsigned long>(retry_tick));
    return;
  }
  attempt = g_backoff_attempt++;
  delay_ms = luce::backoff::next_backoff_ms(attempt, kBackoffMinMs, kBackoffMaxMs,
                                            static_cast<std::uint32_t>(xTaskGetTickCount()));
  retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
  g_backoff_ms = delay_ms;
  g_next_retry_tick = retry_tick;
  g_backoff_scheduled = true;
  ++g_reconnect_count;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  set_state(MqttState::kBackoff, "backoff");
  ESP_LOGW(kTag, "[MQTT][BACKOFF] attempt=%lu delay_ms=%lu retry_tick=%lu",
           static_cast<unsigned long>(attempt), static_cast<unsigned long>(delay_ms),
           static_cast<unsigned long>(retry_tick));
}

void setup_client() {
  esp_mqtt_client_handle_t old_client = take_client();
  if (old_client != nullptr) {
    esp_mqtt_client_destroy(old_client);
  }

  esp_mqtt_client_config_t client_cfg{};
  client_cfg.broker.address.uri = g_cfg.uri;
  client_cfg.credentials.client_id = g_cfg.client_id;
  client_cfg.credentials.username = g_cfg.username[0] != '\0' ? g_cfg.username : nullptr;
  client_cfg.credentials.authentication.password =
      g_cfg.password[0] != '\0' ? g_cfg.password : nullptr;
  client_cfg.network.disable_auto_reconnect = true;
  client_cfg.session.disable_clean_session = false;
  client_cfg.session.keepalive = g_cfg.keepalive_s;

  if (g_cfg.keepalive_s != 0) {
    client_cfg.network.timeout_ms = 3000;
  }

  if (!configure_mqtt_tls(client_cfg)) {
    set_state(MqttState::kFailed, "tls_config");
    return;
  }

  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&client_cfg);
  if (client == nullptr) {
    set_state(MqttState::kFailed, "client_init");
    return;
  }
  set_client(client);

  esp_err_t reg = esp_mqtt_client_register_event(
      client, MQTT_EVENT_ANY,
      [](void* handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
        auto* event = static_cast<esp_mqtt_event_t*>(event_data);
        switch (event_id) {
        case MQTT_EVENT_CONNECTED:
          note_connected();
          set_state(MqttState::kConnected, "connected");
          subscribe_control_topics();
          ESP_LOGI(kTag, "[MQTT][EVENT] connected");
          break;
        case MQTT_EVENT_DISCONNECTED:
          set_connected_flag(false);
          g_last_rcvd = 0;
          ESP_LOGW(kTag, "[MQTT][EVENT] disconnected");
          schedule_backoff();
          break;
        case MQTT_EVENT_ERROR:
          set_connected_flag(false);
          set_state(MqttState::kBackoff, "error");
          ESP_LOGW(kTag, "[MQTT][EVENT] error");
          schedule_backoff();
          break;
        case MQTT_EVENT_DATA: {
          if (!event || event->topic == nullptr || event->topic_len == 0 ||
              event->data == nullptr || event->data_len == 0) {
            break;
          }
          char topic[kTopicTextBufferBytes] = {0};
          char payload[kPayloadTextBufferBytes] = {0};
          copy_text(event->topic, event->topic_len, topic, sizeof(topic));
          copy_text(event->data, event->data_len, payload, sizeof(payload));

          const std::size_t base_len = std::strlen(g_cfg.base_topic);
          if (base_len == 0 || std::strncmp(topic, g_cfg.base_topic, base_len) != 0 ||
              topic[base_len] != '/') {
            break;
          }
          const char* subtopic = topic + base_len + 1;
          if (subtopic[0] == '\0') {
            break;
          }
          dispatch_inbound_message(subtopic, payload);
          g_last_rcvd = xTaskGetTickCount();
          break;
        }
        default:
          break;
        }
        (void)handler_arg;
        (void)event_base;
        (void)event;
      },
      nullptr);

  if (reg != ESP_OK) {
    set_state(MqttState::kFailed, "register_event");
  }
}

int publish_with_topic_suffix(const char* topic_suffix, const char* payload,
                              std::size_t payload_len) {
  const MqttRuntimeSnapshot snapshot = runtime_snapshot();
  if (snapshot.client == nullptr || !snapshot.connected || topic_suffix == nullptr ||
      *topic_suffix == '\0' || payload == nullptr) {
    return -1;
  }
  char topic[kTopicSuffixBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "%s/%s", g_cfg.base_topic, topic_suffix);

  const std::size_t resolved_len = payload_len == 0 ? std::strlen(payload) : payload_len;
  return esp_mqtt_client_publish(snapshot.client, topic, payload, resolved_len, g_cfg.qos, 0);
}

void publish_state() {
  const MqttRuntimeSnapshot snapshot = runtime_snapshot();
  if (snapshot.client == nullptr || !snapshot.connected) {
    return;
  }
  char payload[kPayloadBufferBytes] = {0};
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  int wifi_rssi = 0;
  wifi_get_rssi(&wifi_rssi);

  luce::json::Writer writer(payload, sizeof(payload));
  writer.begin_object();
  writer.key_str("fw", LUCE_PROJECT_VERSION);
  writer.key_str("strategy", LUCE_STRATEGY_NAME);
  writer.key_str("ip", ip[0] != '\0' ? ip : "n/a");
  writer.key_uint("relay", io_relay_mask());
  writer.key_uint("buttons", io_button_mask());
  writer.key_int("wifi_rssi", wifi_rssi);
  writer.key_bool("hardware_degraded", io_hardware_degraded());
  writer.key_bool("connected", true);
  writer.end_object();
  const int rc = publish_with_topic_suffix("telemetry/state", payload);
  if (rc < 0) {
    ESP_LOGW(kTag, "[MQTT][PUB] failed rc=%d", rc);
    return;
  }
  publish_sensor_aliases();
  publish_relay_aliases();
  portENTER_CRITICAL(&g_mqtt_state_lock);
  ++g_publish_count;
  portEXIT_CRITICAL(&g_mqtt_state_lock);
  ESP_LOGI(kTag, "[MQTT][PUB] topic=%s/telemetry/state bytes=%zu rc=%d", g_cfg.base_topic,
           std::strlen(payload), rc);
}

void mqtt_loop(void*) {
  while (true) {
    if (!g_cfg.enabled) {
      esp_mqtt_client_handle_t client = take_client();
      if (client != nullptr) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
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

    const TickType_t now = xTaskGetTickCount();
    const MqttRuntimeSnapshot snapshot = runtime_snapshot();
    if (snapshot.state == MqttState::kBackoff && now < snapshot.next_retry_tick) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    esp_mqtt_client_handle_t reconnect_client = nullptr;
    if (snapshot.state == MqttState::kBackoff && snapshot.client != nullptr &&
        now >= snapshot.next_retry_tick) {
      portENTER_CRITICAL(&g_mqtt_state_lock);
      if (g_state == MqttState::kBackoff && g_client != nullptr && now >= g_next_retry_tick) {
        reconnect_client = g_client;
        g_backoff_scheduled = false;
      }
      portEXIT_CRITICAL(&g_mqtt_state_lock);
    }
    if (reconnect_client != nullptr) {
      const esp_err_t reconnect_rc = esp_mqtt_client_reconnect(reconnect_client);
      if (reconnect_rc == ESP_OK) {
        set_state(MqttState::kConnecting, "manual_reconnect");
      } else {
        ESP_LOGW(kTag, "[MQTT][BACKOFF] reconnect failed rc=%s", esp_err_to_name(reconnect_rc));
        schedule_backoff();
      }
    }

    const MqttRuntimeSnapshot setup_snapshot = runtime_snapshot();
    if ((setup_snapshot.state == MqttState::kInitialized ||
         setup_snapshot.state == MqttState::kBackoff ||
         setup_snapshot.state == MqttState::kFailed) &&
        setup_snapshot.client == nullptr) {
      portENTER_CRITICAL(&g_mqtt_state_lock);
      g_backoff_scheduled = false;
      portEXIT_CRITICAL(&g_mqtt_state_lock);
      setup_client();
      const MqttRuntimeSnapshot started_snapshot = runtime_snapshot();
      if (started_snapshot.state != MqttState::kFailed && started_snapshot.client != nullptr) {
        const esp_err_t start_rc = esp_mqtt_client_start(started_snapshot.client);
        if (start_rc == ESP_OK) {
          set_state(MqttState::kConnecting, "start");
        } else {
          set_state(MqttState::kFailed, "start_fail");
          schedule_backoff();
        }
      }
    }

    static TickType_t last_publish = 0;
    const MqttRuntimeSnapshot publish_snapshot = runtime_snapshot();
    if (publish_snapshot.connected && (now - last_publish) > pdMS_TO_TICKS(kPublishIntervalMs)) {
      last_publish = now;
      publish_state();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

} // namespace

const char* mqtt_state_name() { return mqtt_state_name_impl(); }

bool mqtt_is_enabled() { return g_cfg.enabled; }

bool mqtt_is_connected() { return mqtt_connected_flag(); }

bool mqtt_is_running() { return mqtt_connected_flag(); }

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
  const luce::pki::Status identity = luce::pki::get_status(luce::pki::Role::kMqttClient);
  const MqttRuntimeSnapshot snapshot = runtime_snapshot();
  ESP_LOGI(kTag,
           "mqtt.status state=%s enabled=%d connected=%d tls=%d ca_source=%s ca_present=%d "
           "client_identity=%s client_cert_present=%d connect_count=%u publish_count=%u "
           "reconnect_count=%u "
           "backoff_ms=%lu uri=%s qos=%lu keepalive=%lu",
           state_name(snapshot.state), g_cfg.enabled ? 1 : 0, snapshot.connected ? 1 : 0,
           mqtt_requires_tls() ? 1 : 0, g_cfg.ca_pem_source, g_cfg.ca_pem[0] != '\0' ? 1 : 0,
           luce::pki::state_name(identity.state), identity.cert_present ? 1 : 0,
           snapshot.connect_count, snapshot.publish_count, snapshot.reconnect_count,
           static_cast<unsigned long>(snapshot.backoff_ms), g_cfg.uri,
           static_cast<unsigned long>(g_cfg.qos), static_cast<unsigned long>(g_cfg.keepalive_s));
}

void mqtt_pubtest_for_cli() {
  if (!g_cfg.enabled) {
    ESP_LOGW(kTag, "CLI command mqtt.pubtest: disabled");
    return;
  }
  const MqttRuntimeSnapshot snapshot = runtime_snapshot();
  if (snapshot.client == nullptr || !snapshot.connected) {
    ESP_LOGW(kTag, "CLI command mqtt.pubtest: not connected");
    return;
  }
  const char payload[] = "{\"pubtest\":true}";
  const int rc = publish_with_topic_suffix("telemetry/pubtest", payload);
  ESP_LOGI(kTag, "CLI command mqtt.pubtest rc=%d", rc);
}

#else

bool mqtt_is_enabled() { return false; }

bool mqtt_is_connected() { return false; }

bool mqtt_is_running() { return false; }

void mqtt_startup() {}
void mqtt_status_for_cli() {}
void mqtt_pubtest_for_cli() {}

#endif
