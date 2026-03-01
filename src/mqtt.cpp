// MQTT publish telemetry implementation.
#include "luce/mqtt.h"

#include <cinttypes>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "luce/led_status.h"
#include "luce/task_budgets.h"

namespace {

constexpr const char* kTag = "[MQTT]";
constexpr const char* kMqttNs = "mqtt";
constexpr std::uint32_t kPublishIntervalMs = 30000;
constexpr std::uint32_t kBackoffMinMs = 30000;
constexpr std::uint32_t kBackoffMaxMs = 60000;
constexpr const char* kMqttNvsTag = "[MQTT][NVS]";
constexpr std::uint16_t kPayloadBufferBytes = 256;
constexpr std::uint16_t kPayloadTextBufferBytes = 128;
constexpr std::uint16_t kTopicSuffixBufferBytes = 128;
constexpr std::uint16_t kTopicTextBufferBytes = 128;
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

int publish_with_topic_suffix(const char* topic_suffix, const char* payload, std::size_t payload_len = 0);

bool parse_u32_value(const char* text, std::uint32_t* out_value) {
  if (!text || !out_value || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const std::uint32_t parsed = static_cast<std::uint32_t>(std::strtoul(text, &end, 0));
  if (*end != '\0') {
    return false;
  }
  *out_value = parsed;
  return true;
}

bool parse_bool_value(const char* text, bool* out_value) {
  if (!text || !out_value || *text == '\0') {
    return false;
  }
  if (std::strcmp(text, "1") == 0) {
    *out_value = true;
    return true;
  }
  if (std::strcmp(text, "0") == 0) {
    *out_value = false;
    return true;
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
  if (std::strcmp(lowered, "on") == 0 || std::strcmp(lowered, "true") == 0 || std::strcmp(lowered, "yes") == 0) {
    *out_value = true;
    return true;
  }
  if (std::strcmp(lowered, "off") == 0 || std::strcmp(lowered, "false") == 0 || std::strcmp(lowered, "no") == 0) {
    *out_value = false;
    return true;
  }
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
  while (text[start] != '\0' && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) {
    ++start;
  }

  std::size_t end = std::strlen(text);
  while (end > start &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
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
  nvs_handle_t handle = 0;
  if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to open namespace '%s' for update", ns);
    return false;
  }
  const esp_err_t set_rc = nvs_set_u8(handle, key, value);
  const bool ok = (set_rc == ESP_OK) ? (nvs_commit(handle) == ESP_OK) : false;
  nvs_close(handle);
  if (!ok) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to persist %s/%s=%u", ns, key, static_cast<unsigned>(value));
  }
  return ok;
}

bool nvs_write_u32(const char* ns, const char* key, std::uint32_t value) {
  nvs_handle_t handle = 0;
  if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to open namespace '%s' for update", ns);
    return false;
  }
  const esp_err_t set_rc = nvs_set_u32(handle, key, value);
  const bool ok = (set_rc == ESP_OK) ? (nvs_commit(handle) == ESP_OK) : false;
  nvs_close(handle);
  if (!ok) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to persist %s/%s=%lu", ns, key, static_cast<unsigned long>(value));
  }
  return ok;
}

bool nvs_write_string(const char* ns, const char* key, const char* value, bool mask_value = false) {
  if (!value) {
    return false;
  }
  nvs_handle_t handle = 0;
  if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to open namespace '%s' for update", ns);
    return false;
  }
  const esp_err_t set_rc = nvs_set_str(handle, key, value);
  const bool ok = (set_rc == ESP_OK) ? (nvs_commit(handle) == ESP_OK) : false;
  nvs_close(handle);
  if (!ok) {
    ESP_LOGW(kTag, "[MQTT][NVS] failed to persist %s/%s=%s", ns, key, mask_value ? "********" : value);
  }
  return ok;
}

void sanitize_json_text(const char* input, char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  if (!input) {
    out[0] = '\0';
    return;
  }
  std::size_t w = 0;
  for (std::size_t i = 0; input[i] != '\0' && w + 1 < out_size; ++i) {
    const unsigned char ch = static_cast<unsigned char>(input[i]);
    if (ch == '"' || ch == '\\' || ch < 0x20u || ch >= 0x7Fu) {
      out[w++] = '_';
    } else {
      out[w++] = static_cast<char>(ch);
    }
  }
  out[w] = '\0';
}

void publish_unsupported_legacy_topic(const char* topic_suffix, const char* reason, const char* payload) {
  if (!topic_suffix || topic_suffix[0] == '\0') {
    return;
  }
  char safe_topic[kTopicSuffixBufferBytes] = {0};
  char safe_reason[48] = {0};
  sanitize_json_text(topic_suffix, safe_topic, sizeof(safe_topic));
  sanitize_json_text(reason ? reason : "unsupported", safe_reason, sizeof(safe_reason));

  char body[kPayloadBufferBytes] = {0};
  std::snprintf(body, sizeof(body),
                "{\"status\":\"unsupported\",\"topic\":\"%s\",\"reason\":\"%s\",\"payload_present\":%s}",
                safe_topic, safe_reason, (payload && payload[0] != '\0') ? "true" : "false");
  (void)publish_with_topic_suffix("compat/unsupported", body);
}

void publish_unsupported_config(const char* subtopic, const char* payload) {
  char topic[kTopicSuffixBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "config/%s", subtopic ? subtopic : "");
  if (payload != nullptr) {
    ESP_LOGW(kTag, "[MQTT][IN] config topic '%s' unsupported or read-only with value '%s'", subtopic ? subtopic : "(null)",
             payload);
  } else {
    ESP_LOGW(kTag, "[MQTT][IN] config topic '%s' unsupported or read-only", subtopic ? subtopic : "(null)");
  }
  publish_unsupported_legacy_topic(topic, "unsupported_or_readonly", payload);
}

bool persist_config_bool(const char* ns, const char* key, const char* payload, const char* tag) {
  bool value = false;
  if (!parse_bool_value(payload, &value)) {
    ESP_LOGW(kTag, "[MQTT][IN] %s config value must be bool: %s", tag ? tag : "config", payload ? payload : "(null)");
    return false;
  }
  if (!nvs_write_u8(ns, key, value ? 1u : 0u)) {
    return false;
  }
  ESP_LOGI(kTag, "[MQTT][IN] %s set %s/%s=%u (reboot to apply)", tag ? tag : "config", ns, key,
           static_cast<unsigned>(value ? 1u : 0u));
  return true;
}

bool persist_config_u32(const char* ns, const char* key, const char* payload, const char* tag, std::uint32_t min = 0,
                       std::uint32_t max = 0xFFFFFFFFu) {
  std::uint32_t value = 0;
  if (!parse_u32_value(payload, &value)) {
    ESP_LOGW(kTag, "[MQTT][IN] %s config value must be integer: %s", tag ? tag : "config", payload ? payload : "(null)");
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
  if (!g_client || g_cfg.base_topic[0] == '\0') {
    return;
  }

  char topic[kTopicTextBufferBytes] = {0};
  std::snprintf(topic, sizeof(topic), "%s/config/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(g_client, topic, g_cfg.qos);
  std::snprintf(topic, sizeof(topic), "%s/relays/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(g_client, topic, g_cfg.qos);
  std::snprintf(topic, sizeof(topic), "%s/sensor/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(g_client, topic, g_cfg.qos);
  std::snprintf(topic, sizeof(topic), "%s/leds/#", g_cfg.base_topic);
  esp_mqtt_client_subscribe(g_client, topic, g_cfg.qos);
}

void handle_relay_state_index(const char* index_text, const char* payload) {
  std::uint32_t index = 0;
  if (!parse_u32_value(index_text, &index) || index >= kRelayCount) {
    ESP_LOGW(kTag, "[MQTT][IN] invalid relay index '%s' for relays/state", index_text ? index_text : "(null)");
    led_status_notify_user_error();
    return;
  }

  bool on = false;
  if (!parse_bool_value(payload, &on)) {
    ESP_LOGW(kTag, "[MQTT][IN] invalid relay value '%s' for relays/state/%u", payload ? payload : "(null)",
             static_cast<unsigned>(index));
    led_status_notify_user_error();
    return;
  }

  const std::uint8_t next_mask = relay_mask_for_channel_state(static_cast<int>(index), on, g_relay_mask);
  if (set_relay_mask_safe(next_mask) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][IN] relays/state/%u failed (i/o unavailable)", static_cast<unsigned>(index));
    led_status_notify_user_error();
    return;
  }
  g_relay_mask = next_mask;
  led_status_notify_user_input();
  ESP_LOGI(kTag, "[MQTT][IN] relays/state/%u=%s", static_cast<unsigned>(index), on ? "on" : "off");
}

void handle_relay_state(const char* payload) {
  std::uint32_t mask = 0;
  if (!parse_u32_value(payload, &mask) || mask > 0xFFu) {
    ESP_LOGW(kTag, "[MQTT][IN] invalid relay mask '%s' for relays/state", payload ? payload : "(null)");
    led_status_notify_user_error();
    return;
  }

  const std::uint8_t next_mask = static_cast<std::uint8_t>(mask & 0xFFu);
  if (set_relay_mask_safe(next_mask) != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][IN] relays/state failed (i/o unavailable)");
    led_status_notify_user_error();
    return;
  }
  g_relay_mask = next_mask;
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
        ESP_LOGW(kTag, "[MQTT][IN] relays/night/%u invalid value '%s'", static_cast<unsigned>(index), value_text);
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
      ESP_LOGI(kTag, "[MQTT][IN] relays/night/%u=%s (mask=0x%02X)", static_cast<unsigned>(index), on ? "on" : "off",
               static_cast<unsigned>(mask));
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

  if (std::strcmp(subtopic, "name") == 0) {
    if (nvs_write_string("net", "hostname", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/name set net/hostname=%s (reboot to apply)", value_text);
    }
    return;
  }
  if (std::strcmp(subtopic, "hostname") == 0) {
    if (nvs_write_string("net", "hostname", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/hostname set net/hostname=%s (reboot to apply)", value_text);
    }
    return;
  }
  if (std::strcmp(subtopic, "ssid") == 0) {
    if (nvs_write_string("wifi", "ssid", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/ssid set wifi/ssid=%s (reboot to apply)", value_text);
    }
    return;
  }
  if (std::strcmp(subtopic, "pass") == 0) {
    if (nvs_write_string("wifi", "pass", value_text, true)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/pass set wifi/pass (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "ssid2") == 0) {
    if (nvs_write_string("wifi", "ssid2", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/ssid2 set wifi/ssid2=%s (reboot to apply)", value_text);
    }
    return;
  }
  if (std::strcmp(subtopic, "pass2") == 0) {
    if (nvs_write_string("wifi", "pass2", value_text, true)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/pass2 set wifi/pass2 (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "wifi/ssid") == 0) {
    if (nvs_write_string("wifi", "ssid", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/wifi/ssid updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "wifi/pass") == 0) {
    if (nvs_write_string("wifi", "pass", value_text, true)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/wifi/pass updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "logConsoleFmt") == 0) {
    ESP_LOGI(kTag, "[MQTT][IN] config/logConsoleFmt='%s' (compat, retained only)", value_text);
    (void)nvs_write_string("compat", "log_console_fmt", value_text);
    return;
  }
  if (std::strcmp(subtopic, "logFileFmt") == 0) {
    ESP_LOGI(kTag, "[MQTT][IN] config/logFileFmt='%s' (compat, retained only)", value_text);
    (void)nvs_write_string("compat", "log_file_fmt", value_text);
    return;
  }
  if (std::strcmp(subtopic, "logConsoleLevel") == 0) {
    ESP_LOGI(kTag, "[MQTT][IN] config/logConsoleLevel='%s' (compat, retained only)", value_text);
    (void)nvs_write_string("compat", "log_console_level", value_text);
    return;
  }
  if (std::strcmp(subtopic, "logFileLevel") == 0) {
    ESP_LOGI(kTag, "[MQTT][IN] config/logFileLevel='%s' (compat, retained only)", value_text);
    (void)nvs_write_string("compat", "log_file_level", value_text);
    return;
  }
  if (std::strcmp(subtopic, "mqtt") == 0 || std::strcmp(subtopic, "mqtt/uri") == 0) {
    if (nvs_write_string("mqtt", "uri", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/mqtt uri updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "mqtt/client_id") == 0) {
    if (nvs_write_string("mqtt", "client_id", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/mqtt/client_id updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "mqtt/base_topic") == 0) {
    if (nvs_write_string("mqtt", "base_topic", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/mqtt/base_topic updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "mqtt/username") == 0) {
    if (nvs_write_string("mqtt", "username", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/mqtt/username updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "mqtt/password") == 0) {
    if (nvs_write_string("mqtt", "password", value_text, true)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/mqtt/password updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "mqtt/tls_enabled") == 0) {
    (void)persist_config_bool("mqtt", "tls_enabled", value_text, "mqtt");
    return;
  }
  if (std::strcmp(subtopic, "mqtt/ca_pem_source") == 0) {
    if (nvs_write_string("mqtt", "ca_pem_source", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/mqtt/ca_pem_source set=%s (reboot to apply)", value_text);
    }
    return;
  }
  if (std::strcmp(subtopic, "mqtt/qos") == 0) {
    (void)persist_config_u32("mqtt", "qos", value_text, "mqtt", 0u, 2u);
    return;
  }
  if (std::strcmp(subtopic, "mqtt/keepalive_s") == 0) {
    (void)persist_config_u32("mqtt", "keepalive_s", value_text, "mqtt", 30u, 7200u);
    return;
  }
  if (std::strcmp(subtopic, "mdns/instance") == 0) {
    if (nvs_write_string("mdns", "instance", value_text)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/mdns/instance set=%s (reboot to apply)", value_text);
    }
    return;
  }
  if (std::strcmp(subtopic, "http/token") == 0) {
    if (nvs_write_string("http", "token", value_text, true)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/http/token updated (reboot to apply)");
    }
    return;
  }
  if (std::strcmp(subtopic, "cli_net/token") == 0) {
    if (nvs_write_string("cli_net", "token", value_text, true)) {
      ESP_LOGI(kTag, "[MQTT][IN] config/cli_net/token updated (reboot to apply)");
    }
    return;
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
        ESP_LOGW(kTag, "[MQTT][IN] leds/state/%lu invalid payload '%s'", static_cast<unsigned long>(index), value);
        return;
      }
      (void)led_status_set_manual_mode(static_cast<std::uint8_t>(index), mode);
      ESP_LOGI(kTag, "[MQTT][IN] leds/state/%lu mode applied", static_cast<unsigned long>(index));
    }
    const std::uint8_t current_mask = led_status_current_mask();
    std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>((current_mask >> index) & 0x1u));
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
  if (!g_client || !g_connected) {
    return;
  }
  char payload[kPayloadTextBufferBytes] = {0};
  std::snprintf(payload, sizeof(payload), "%u", static_cast<unsigned>(g_relay_mask));
  (void)publish_with_topic_suffix("relays/state", payload);

  for (std::uint8_t idx = 0; idx < kRelayCount; ++idx) {
    const std::uint8_t on_mask = relay_mask_for_channel_state(static_cast<int>(idx), true, g_relay_mask);
    const bool on = (on_mask != g_relay_mask);
    char idx_topic[kTopicTextBufferBytes] = {0};
    std::snprintf(idx_topic, sizeof(idx_topic), "relays/state/%u", static_cast<unsigned>(idx));
    std::snprintf(payload, sizeof(payload), "%d", on ? 1 : 0);
    (void)publish_with_topic_suffix(idx_topic, payload);
  }
}

void publish_sensor_aliases() {
  if (!g_client || !g_connected) {
    return;
  }
  I2cSensorSnapshot snapshot {};
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
        subscribe_control_topics();
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
      case MQTT_EVENT_DATA:
      {
        if (!event || event->topic == nullptr || event->topic_len == 0 || event->data == nullptr || event->data_len == 0) {
          break;
        }
        char topic[kTopicTextBufferBytes] = {0};
        char payload[kPayloadTextBufferBytes] = {0};
        copy_text(event->topic, event->topic_len, topic, sizeof(topic));
        copy_text(event->data, event->data_len, payload, sizeof(payload));

        const std::size_t base_len = std::strlen(g_cfg.base_topic);
        if (base_len == 0 || std::strncmp(topic, g_cfg.base_topic, base_len) != 0 || topic[base_len] != '/') {
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
  }, nullptr);

  if (reg != ESP_OK) {
    set_state(MqttState::kFailed, "register_event");
  }
}

int publish_with_topic_suffix(const char* topic_suffix, const char* payload, std::size_t payload_len) {
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
  publish_sensor_aliases();
  publish_relay_aliases();
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
