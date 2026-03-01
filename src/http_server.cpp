// HTTPS API server + captive HTTP web portal.
#include "luce/http_server.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <strings.h>

#if LUCE_HAS_HTTP

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "nvs.h"

#include "luce/net_wifi.h"
#include "luce/i2c_io.h"
#include "luce/led_status.h"
#include "luce/ntp.h"
#include "luce/mqtt.h"
#include "luce/ota.h"
#include "luce_build.h"
#include "luce/nvs_helpers.h"
#include "luce/runtime_state.h"
#include "luce/task_budgets.h"

namespace {

constexpr const char* kTag = "[HTTP]";
constexpr const char* kHttpNs = "http";
constexpr std::uint16_t kDefaultHttpPort = 443;
constexpr std::uint16_t kCaptiveHttpPort = 80;
constexpr std::size_t kWsClientScanMax = 8;
constexpr const char* kDefaultToken = "luce-token";
constexpr const char* kUnauthorizedPayload = "{\"error\":\"unauthorized\"}";
constexpr const char* kMethodNotAllowedPayload = "{\"error\":\"method_not_allowed\",\"allowed\":\"%s\"}";
constexpr const char* kBadRequestPayload = "{\"error\":\"bad_request\",\"message\":\"%s\"}";

enum class HttpState : std::uint8_t {
  kDisabled = 0,
  kInit,
  kStarted,
  kFailed,
};

struct HttpConfig {
  bool enabled = false;
  uint16_t port = kDefaultHttpPort;
  char token[64] = {};
  bool tls_dev_mode = false;
};

HttpConfig g_cfg {};
HttpState g_state = HttpState::kDisabled;
httpd_handle_t g_httpd = nullptr;
httpd_handle_t g_captive_httpd = nullptr;
TaskHandle_t g_task = nullptr;

static const char kServerCertPgm[] = R"EOF(-----BEGIN CERTIFICATE-----
MIIDXTCCAk2gAwIBAgIJAK9...
-----END CERTIFICATE-----
)EOF";
static const char kServerKeyPgm[] = R"EOF(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9...
-----END PRIVATE KEY-----
)EOF";

extern "C" {
extern const unsigned char webapp_index_html[];
extern const unsigned int webapp_index_html_len;
extern const unsigned char webapp_app_css[];
extern const unsigned int webapp_app_css_len;
extern const unsigned char webapp_script_js[];
extern const unsigned int webapp_script_js_len;
}

struct WebAsset {
  const char* uri;
  const char* content_type;
  const std::uint8_t* data;
  std::size_t size;
};

const WebAsset kWebAppAssets[] = {
    {"/index.html", "text/html; charset=utf-8", webapp_index_html, static_cast<std::size_t>(webapp_index_html_len)},
    {"/app.css", "text/css; charset=utf-8", webapp_app_css, static_cast<std::size_t>(webapp_app_css_len)},
    {"/script.js", "text/javascript; charset=utf-8", webapp_script_js, static_cast<std::size_t>(webapp_script_js_len)},
};

const char* state_name(HttpState state) {
  switch (state) {
    case HttpState::kDisabled:
      return "DISABLED";
    case HttpState::kInit:
      return "INIT";
    case HttpState::kStarted:
      return "STARTED";
    case HttpState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

const char* http_state_name_impl() {
  return state_name(g_state);
}

void set_state(HttpState next, const char* reason = nullptr) {
  luce::runtime::set_state(g_state, next, state_name, "[HTTP]", reason);
}

void load_http_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.enabled = false;
  g_cfg.port = kDefaultHttpPort;
  std::snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", kDefaultToken);
  g_cfg.tls_dev_mode = false;

  nvs_handle_t handle = 0;
  if (nvs_open(kHttpNs, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP] namespace '%s' not found; defaults active", kHttpNs);
    set_state(HttpState::kDisabled, "namespace_missing");
    return;
  }

  std::uint8_t enabled = 0;
  std::uint8_t tls = 0;
  std::uint16_t port = kDefaultHttpPort;
  if (luce::nvs::read_u8(handle, "enabled", enabled, 0)) {
    g_cfg.enabled = (enabled != 0);
  }
  if (luce::nvs::read_u16(handle, "port", port, kDefaultHttpPort) && port != 0) {
    g_cfg.port = port;
  }
  if (luce::nvs::read_u8(handle, "tls_dev_mode", tls, 0)) {
    g_cfg.tls_dev_mode = (tls != 0);
  }
  (void)luce::nvs::read_string(handle, "token", g_cfg.token, sizeof(g_cfg.token), kDefaultToken);
  nvs_close(handle);
  set_state(g_cfg.enabled ? HttpState::kInit : HttpState::kDisabled, g_cfg.enabled ? "config_enabled" : "config_disabled");
  ESP_LOGI(kTag, "[HTTP] enabled=%d port=%u tls=%d", g_cfg.enabled ? 1 : 0, g_cfg.port, g_cfg.tls_dev_mode ? 1 : 0);
}

const char* as_n_a(const char* value) {
  return (value != nullptr && value[0] != '\0') ? value : "n/a";
}

esp_err_t send_json(httpd_req_t* req, int status, const char* payload, std::size_t payload_len = 0) {
  char status_line[16] = {0};
  std::snprintf(status_line, sizeof(status_line), "%d", status);
  httpd_resp_set_status(req, status_line);
  httpd_resp_set_type(req, "application/json");
  if (!payload) {
    return httpd_resp_send(req, "", 0);
  }
  if (payload_len == 0) {
    payload_len = std::strlen(payload);
  }
  return httpd_resp_send(req, payload, payload_len);
}

esp_err_t send_method_not_allowed(httpd_req_t* req, const char* allowed_methods) {
  char payload[96] = {0};
  std::snprintf(payload, sizeof(payload), kMethodNotAllowedPayload, allowed_methods ? allowed_methods : "GET");
  httpd_resp_set_status(req, "405");
  if (allowed_methods != nullptr) {
    httpd_resp_set_hdr(req, "Allow", allowed_methods);
  }
  return send_json(req, 405, payload, 0);
}

esp_err_t send_unauthorized(httpd_req_t* req) {
  ESP_LOGW(kTag, "[HTTP] auth fail");
  return send_json(req, 401, kUnauthorizedPayload, 0);
}

esp_err_t send_bad_request(httpd_req_t* req, const char* message) {
  char payload[160] = {0};
  std::snprintf(payload, sizeof(payload), kBadRequestPayload, message ? message : "invalid_request");
  return send_json(req, 400, payload, 0);
}

bool validate_auth(httpd_req_t* req) {
  char header[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
    return false;
  }
  const std::size_t token_len = std::strlen(g_cfg.token);
  if (token_len == 0) {
    return false;
  }
  char expected[80] = {0};
  std::snprintf(expected, sizeof(expected), "Bearer %s", g_cfg.token);
  return std::strncmp(header, expected, sizeof(expected)) == 0;
}

bool parse_bool_token(const char* text, bool* out_value) {
  if (!text || !out_value || text[0] == '\0') {
    return false;
  }
  if (std::strcmp(text, "1") == 0 || strcasecmp(text, "on") == 0 || strcasecmp(text, "true") == 0 ||
      strcasecmp(text, "yes") == 0) {
    *out_value = true;
    return true;
  }
  if (std::strcmp(text, "0") == 0 || strcasecmp(text, "off") == 0 || strcasecmp(text, "false") == 0 ||
      strcasecmp(text, "no") == 0) {
    *out_value = false;
    return true;
  }
  return false;
}

bool parse_led_manual_mode_token(const char* text, LedManualMode* out_mode) {
  if (!text || !out_mode || text[0] == '\0') {
    return false;
  }
  if (strcasecmp(text, "auto") == 0) {
    *out_mode = LedManualMode::kAuto;
    return true;
  }
  if (strcasecmp(text, "blink") == 0 || strcasecmp(text, "normal") == 0) {
    *out_mode = LedManualMode::kBlinkNormal;
    return true;
  }
  if (strcasecmp(text, "fast") == 0) {
    *out_mode = LedManualMode::kBlinkFast;
    return true;
  }
  if (strcasecmp(text, "slow") == 0) {
    *out_mode = LedManualMode::kBlinkSlow;
    return true;
  }
  if (strcasecmp(text, "flash") == 0) {
    *out_mode = LedManualMode::kFlash;
    return true;
  }
  bool on = false;
  if (parse_bool_token(text, &on)) {
    *out_mode = on ? LedManualMode::kOn : LedManualMode::kOff;
    return true;
  }
  return false;
}

const char* led_manual_mode_name(LedManualMode mode) {
  switch (mode) {
    case LedManualMode::kAuto:
      return "auto";
    case LedManualMode::kOff:
      return "off";
    case LedManualMode::kOn:
      return "on";
    case LedManualMode::kBlinkNormal:
      return "blink";
    case LedManualMode::kBlinkFast:
      return "fast";
    case LedManualMode::kBlinkSlow:
      return "slow";
    case LedManualMode::kFlash:
      return "flash";
    default:
      return "auto";
  }
}

bool parse_u32_token(const char* text, std::uint32_t* out_value) {
  if (!text || !out_value || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const std::uint32_t parsed = static_cast<std::uint32_t>(std::strtoul(text, &end, 0));
  if (!end || *end != '\0') {
    return false;
  }
  *out_value = parsed;
  return true;
}

bool read_request_value(httpd_req_t* req, char* out, std::size_t out_size) {
  if (!req || !out || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  if (req->content_len > 0 && req->content_len < static_cast<int>(out_size)) {
    const int got = httpd_req_recv(req, out, req->content_len);
    if (got > 0) {
      out[got < static_cast<int>(out_size) ? got : static_cast<int>(out_size - 1)] = '\0';
      return out[0] != '\0';
    }
  }

  char query[96] = {0};
  if (httpd_req_get_url_query_len(req) > 0 && httpd_req_get_url_query_len(req) < static_cast<int>(sizeof(query))) {
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      if (httpd_query_key_value(query, "value", out, out_size) == ESP_OK) {
        return out[0] != '\0';
      }
    }
  }
  return false;
}

void trim_ascii_whitespace_inplace(char* text) {
  if (!text) {
    return;
  }
  std::size_t len = std::strlen(text);
  while (len > 0 && std::isspace(static_cast<unsigned char>(text[len - 1])) != 0) {
    text[len - 1] = '\0';
    --len;
  }
  std::size_t start = 0;
  while (text[start] != '\0' && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  if (start > 0) {
    std::memmove(text, text + start, std::strlen(text + start) + 1);
  }
}

esp_err_t get_uptime_payload(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return ESP_FAIL;
  }
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  std::snprintf(out, out_size,
                "{\"service\":\"luce\",\"strategy\":\"%s\",\"status\":\"ok\",\"build\":\"%s\",\"sha\":\"%s\","
                "\"uptime_s\":%lld,\"wifi_ip\":\"%s\"}",
                LUCE_STRATEGY_NAME, __DATE__ " " __TIME__, LUCE_GIT_SHA, (long long)(esp_timer_get_time() / 1000000ULL),
                as_n_a(ip));
  return ESP_OK;
}

void build_ws_snapshot_payload(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  I2cSensorSnapshot snapshot {};
  const bool has_sensor = read_sensor_snapshot(snapshot);
  const std::uint16_t threshold = io_light_threshold();
  const std::uint8_t night_mask = io_relay_night_mask();
  const bool day = has_sensor ? (snapshot.light_raw > static_cast<int>(threshold)) : false;
  std::time_t now = 0;
  (void)std::time(&now);
  std::snprintf(
      out, out_size,
      "{\"type\":\"state\",\"tstamp\":%llu,\"state\":%u,\"night\":%u,\"day\":%u,\"threshold\":%u,"
      "\"light\":%d,\"voltage\":%d,\"temperature\":%.1f,\"humidity\":%.1f,\"sensor_ok\":%s}",
      static_cast<unsigned long long>(now > 0 ? now : 0), static_cast<unsigned>(g_relay_mask), static_cast<unsigned>(night_mask),
      day ? 1u : 0u, static_cast<unsigned>(threshold), has_sensor ? snapshot.light_raw : 0,
      has_sensor ? snapshot.voltage_raw : 0, has_sensor ? snapshot.temperature_c : 0.0f,
      has_sensor ? snapshot.humidity_percent : 0.0f, has_sensor && snapshot.dht_ok ? "true" : "false");
}

void ws_broadcast_snapshot(httpd_handle_t server) {
  if (!server) {
    return;
  }
  int client_fds[kWsClientScanMax] = {0};
  std::size_t clients = kWsClientScanMax;
  if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK) {
    return;
  }
  char payload[512] = {0};
  build_ws_snapshot_payload(payload, sizeof(payload));
  httpd_ws_frame_t ws_pkt {};
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  ws_pkt.payload = reinterpret_cast<std::uint8_t*>(payload);
  ws_pkt.len = std::strlen(payload);
  for (std::size_t i = 0; i < clients; ++i) {
    const int fd = client_fds[i];
    if (fd < 0) {
      continue;
    }
    if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
      continue;
    }
    (void)httpd_ws_send_frame_async(server, fd, &ws_pkt);
  }
}

esp_err_t route_health(httpd_req_t* req) {
  if (req->method != HTTP_GET) {
    return send_method_not_allowed(req, "GET");
  }
  char payload[256] = {0};
  get_uptime_payload(payload, sizeof(payload));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_info(httpd_req_t* req) {
  if (req->method != HTTP_GET) {
    return send_method_not_allowed(req, "GET");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  I2cSensorSnapshot snapshot {};
  const bool has_sensor = read_sensor_snapshot(snapshot);
  const std::uint16_t threshold = io_light_threshold();
  const std::uint8_t night_mask = io_relay_night_mask();
  const bool day = has_sensor ? (snapshot.light_raw > static_cast<int>(threshold)) : false;
  char payload[1024] = {0};
  std::snprintf(payload, sizeof(payload),
                "{\"service\":\"luce\",\"name\":\"%s\",\"version\":\"%s\",\"strategy\":\"%s\",\"sha\":\"%s\","
                "\"build\":\"%s %s\",\"uptimeMs\":%llu,\"uptime_s\":%llu,\"wifi_ip\":\"%s\","
                "\"http_enabled\":%s,\"http_port\":%u,\"tls\":%d,"
                "\"relays\":%u,\"nightMask\":%u,\"day\":%s,\"threshold\":%u,"
                "\"light\":%d,\"temperature\":%.1f,\"humidity\":%.1f,\"sensor_ok\":%s,"
                "\"network\":{\"ip\":\"%s\",\"wifiConnected\":%s,\"mqttConnected\":%s,\"ntpSynced\":%s}}",
                "luce", LUCE_PROJECT_VERSION, LUCE_STRATEGY_NAME, LUCE_GIT_SHA, __DATE__, __TIME__,
                static_cast<unsigned long long>(esp_timer_get_time() / 1000ULL),
                static_cast<unsigned long long>(esp_timer_get_time() / 1000000ULL), as_n_a(ip),
                g_cfg.enabled ? "true" : "false", g_cfg.port, g_cfg.tls_dev_mode ? 1 : 0,
                static_cast<unsigned>(g_relay_mask), static_cast<unsigned>(night_mask), day ? "true" : "false",
                static_cast<unsigned>(threshold), has_sensor ? snapshot.light_raw : 0,
                has_sensor ? snapshot.temperature_c : 0.0f, has_sensor ? snapshot.humidity_percent : 0.0f,
                has_sensor && snapshot.dht_ok ? "true" : "false", as_n_a(ip), wifi_is_connected() ? "true" : "false",
                mqtt_is_connected() ? "true" : "false", ntp_is_synced() ? "true" : "false");
  return send_json(req, 200, payload, 0);
}

esp_err_t route_version(httpd_req_t* req) {
  if (req->method != HTTP_GET) {
    return send_method_not_allowed(req, "GET");
  }
  char payload[192] = {0};
  std::snprintf(payload, sizeof(payload),
                "{\"service\":\"luce\",\"version\":\"%s\",\"strategy\":\"%s\",\"sha\":\"%s\",\"build\":\"%s %s\"}",
                LUCE_PROJECT_VERSION, LUCE_STRATEGY_NAME, LUCE_GIT_SHA, __DATE__, __TIME__);
  return send_json(req, 200, payload, 0);
}

esp_err_t route_state(httpd_req_t* req) {
  if (req->method != HTTP_GET) {
    return send_method_not_allowed(req, "GET");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }
  char payload[384] = {0};
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  std::snprintf(payload, sizeof(payload),
               "{\"state\":\"running\",\"wifi_ip\":\"%s\",\"relay\":%u,\"buttons\":%u,\"requests\":1,\"unauth\":0,"
               "\"service\":\"luce\",\"strategy\":\"%s\",\"ntp_state\":0}",
               as_n_a(ip), static_cast<unsigned>(g_relay_mask), static_cast<unsigned>(g_button_mask),
               LUCE_STRATEGY_NAME);
  return send_json(req, 200, payload, 0);
}

esp_err_t route_ota(httpd_req_t* req) {
  if (req->method != HTTP_GET) {
    return send_method_not_allowed(req, "GET");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }
  char payload[512] = {0};
  ota_build_status_payload(payload, sizeof(payload));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_ota_check(httpd_req_t* req) {
  if (req->method != HTTP_POST && req->method != HTTP_PUT) {
    return send_method_not_allowed(req, "POST, PUT");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }
  char query[64] = {0};
  char query_url[256] = {0};
  char body_url[256] = {0};
  const char* source = "default";
  if (req->content_len > 0 && req->content_len < static_cast<int>(sizeof(body_url))) {
    const int got = httpd_req_recv(req, body_url, req->content_len);
    if (got > 0) {
      body_url[got < static_cast<int>(sizeof(body_url)) ? got : static_cast<int>(sizeof(body_url) - 1)] = '\0';
      trim_ascii_whitespace_inplace(body_url);
    }
  }
  if (httpd_req_get_url_query_len(req) > 0 && httpd_req_get_url_query_len(req) < static_cast<int>(sizeof(query))) {
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      if (httpd_query_key_value(query, "url", query_url, sizeof(query_url)) == ESP_OK) {
        trim_ascii_whitespace_inplace(query_url);
      }
    }
  }
  if (query_url[0] != '\0') {
    ota_request_check_with_url(query_url);
    source = "query";
  } else if (body_url[0] != '\0') {
    ota_request_check_with_url(body_url);
    source = "body";
  } else {
    ota_request_check();
    source = "default";
  }
  char payload[160] = {0};
  std::snprintf(payload, sizeof(payload), "{\"status\":\"queued\",\"source\":\"%s\"}", source);
  return send_json(req, 202, payload, 0);
}

esp_err_t build_leds_payload(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return ESP_FAIL;
  }
  const std::uint8_t state = led_status_current_mask();
  const std::uint8_t manual_enabled = led_status_manual_enabled_mask();
  const std::uint8_t manual_value = led_status_manual_value_mask();
  std::snprintf(out, out_size,
                "{\"state\":%u,\"manual_enabled\":%u,\"manual_value\":%u,"
                "\"mode0\":\"%s\",\"mode1\":\"%s\",\"mode2\":\"%s\"}",
                static_cast<unsigned>(state), static_cast<unsigned>(manual_enabled), static_cast<unsigned>(manual_value),
                led_manual_mode_name(led_status_manual_mode(0)),
                led_manual_mode_name(led_status_manual_mode(1)),
                led_manual_mode_name(led_status_manual_mode(2)));
  return ESP_OK;
}

esp_err_t route_leds_state(httpd_req_t* req) {
  if (req->method != HTTP_GET && req->method != HTTP_PUT) {
    return send_method_not_allowed(req, "GET, PUT");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }

  if (req->method == HTTP_PUT) {
    char value[64] = {0};
    if (!read_request_value(req, value, sizeof(value))) {
      return send_bad_request(req, "missing_value");
    }
    std::uint32_t mask = 0;
    if (parse_u32_token(value, &mask) && mask <= 0x07u) {
      for (std::uint8_t idx = 0; idx < 3; ++idx) {
        const bool on = ((mask >> idx) & 0x1u) != 0u;
        (void)led_status_set_manual(idx, on);
      }
    } else {
      LedManualMode mode = LedManualMode::kAuto;
      if (!parse_led_manual_mode_token(value, &mode)) {
        return send_bad_request(req, "invalid_led_value");
      }
      for (std::uint8_t idx = 0; idx < 3; ++idx) {
        (void)led_status_set_manual_mode(idx, mode);
      }
    }
  }

  char payload[128] = {0};
  (void)build_leds_payload(payload, sizeof(payload));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_leds_state_0(httpd_req_t* req) {
  if (req->method != HTTP_GET && req->method != HTTP_PUT) {
    return send_method_not_allowed(req, "GET, PUT");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }
  if (req->method == HTTP_PUT) {
    char value[32] = {0};
    if (!read_request_value(req, value, sizeof(value))) {
      return send_bad_request(req, "missing_value");
    }
    LedManualMode mode = LedManualMode::kAuto;
    if (!parse_led_manual_mode_token(value, &mode)) {
      return send_bad_request(req, "invalid_led_value");
    }
    (void)led_status_set_manual_mode(0, mode);
  }
  const std::uint8_t state = led_status_current_mask();
  const std::uint8_t manual_enabled = led_status_manual_enabled_mask();
  const std::uint8_t manual_value = led_status_manual_value_mask();
  char payload[128] = {0};
  std::snprintf(payload, sizeof(payload), "{\"index\":0,\"state\":%u,\"manual\":%s,\"mode\":\"%s\"}",
                static_cast<unsigned>(state & 0x1u),
                (manual_enabled & 0x1u) ? (((manual_value & 0x1u) != 0u) ? "true" : "false") : "null",
                led_manual_mode_name(led_status_manual_mode(0)));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_leds_state_1(httpd_req_t* req) {
  if (req->method != HTTP_GET && req->method != HTTP_PUT) {
    return send_method_not_allowed(req, "GET, PUT");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }
  if (req->method == HTTP_PUT) {
    char value[32] = {0};
    if (!read_request_value(req, value, sizeof(value))) {
      return send_bad_request(req, "missing_value");
    }
    LedManualMode mode = LedManualMode::kAuto;
    if (!parse_led_manual_mode_token(value, &mode)) {
      return send_bad_request(req, "invalid_led_value");
    }
    (void)led_status_set_manual_mode(1, mode);
  }
  const std::uint8_t state = led_status_current_mask();
  const std::uint8_t manual_enabled = led_status_manual_enabled_mask();
  const std::uint8_t manual_value = led_status_manual_value_mask();
  char payload[128] = {0};
  std::snprintf(payload, sizeof(payload), "{\"index\":1,\"state\":%u,\"manual\":%s,\"mode\":\"%s\"}",
                static_cast<unsigned>((state >> 1) & 0x1u),
                (manual_enabled & 0x2u) ? (((manual_value & 0x2u) != 0u) ? "true" : "false") : "null",
                led_manual_mode_name(led_status_manual_mode(1)));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_leds_state_2(httpd_req_t* req) {
  if (req->method != HTTP_GET && req->method != HTTP_PUT) {
    return send_method_not_allowed(req, "GET, PUT");
  }
  if (!validate_auth(req)) {
    return send_unauthorized(req);
  }
  if (req->method == HTTP_PUT) {
    char value[32] = {0};
    if (!read_request_value(req, value, sizeof(value))) {
      return send_bad_request(req, "missing_value");
    }
    LedManualMode mode = LedManualMode::kAuto;
    if (!parse_led_manual_mode_token(value, &mode)) {
      return send_bad_request(req, "invalid_led_value");
    }
    (void)led_status_set_manual_mode(2, mode);
  }
  const std::uint8_t state = led_status_current_mask();
  const std::uint8_t manual_enabled = led_status_manual_enabled_mask();
  const std::uint8_t manual_value = led_status_manual_value_mask();
  char payload[128] = {0};
  std::snprintf(payload, sizeof(payload), "{\"index\":2,\"state\":%u,\"manual\":%s,\"mode\":\"%s\"}",
                static_cast<unsigned>((state >> 2) & 0x1u),
                (manual_enabled & 0x4u) ? (((manual_value & 0x4u) != 0u) ? "true" : "false") : "null",
                led_manual_mode_name(led_status_manual_mode(2)));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_ws(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    char payload[512] = {0};
    build_ws_snapshot_payload(payload, sizeof(payload));
    httpd_ws_frame_t ws_pkt {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = reinterpret_cast<std::uint8_t*>(payload);
    ws_pkt.len = std::strlen(payload);
    return httpd_ws_send_frame(req, &ws_pkt);
  }

  httpd_ws_frame_t ws_pkt {};
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t rc = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (rc != ESP_OK) {
    return rc;
  }

  std::uint8_t* rx_payload = nullptr;
  if (ws_pkt.len > 0) {
    rx_payload = static_cast<std::uint8_t*>(std::calloc(ws_pkt.len + 1, 1));
    if (!rx_payload) {
      return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = rx_payload;
    rc = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (rc != ESP_OK) {
      std::free(rx_payload);
      return rc;
    }
  }

  if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
    httpd_ws_frame_t pong {};
    pong.type = HTTPD_WS_TYPE_PONG;
    pong.payload = ws_pkt.payload;
    pong.len = ws_pkt.len;
    (void)httpd_ws_send_frame(req, &pong);
  } else if (ws_pkt.type != HTTPD_WS_TYPE_CLOSE) {
    char payload[512] = {0};
    build_ws_snapshot_payload(payload, sizeof(payload));
    httpd_ws_frame_t reply {};
    reply.type = HTTPD_WS_TYPE_TEXT;
    reply.payload = reinterpret_cast<std::uint8_t*>(payload);
    reply.len = std::strlen(payload);
    (void)httpd_ws_send_frame(req, &reply);
  }

  if (rx_payload) {
    std::free(rx_payload);
  }
  return ESP_OK;
}

const WebAsset* resolve_captive_asset(const char* path) {
  if (path == nullptr) {
    return &kWebAppAssets[0];
  }
  if (std::strcmp(path, "/") == 0 || std::strcmp(path, "/index.html") == 0) {
    return &kWebAppAssets[0];
  }
  for (std::size_t i = 1; i < (sizeof(kWebAppAssets) / sizeof(kWebAppAssets[0])); ++i) {
    if (std::strcmp(path, kWebAppAssets[i].uri) == 0) {
      return &kWebAppAssets[i];
    }
  }
  return &kWebAppAssets[0];
}

esp_err_t route_captive(httpd_req_t* req) {
  const WebAsset* asset = resolve_captive_asset(req != nullptr ? req->uri : nullptr);
  if (asset == nullptr || asset->data == nullptr || asset->size == 0) {
    ESP_LOGW(kTag, "[HTTP][CAPTIVE] requested asset missing uri=%s", req && req->uri ? req->uri : "<null>");
    return httpd_resp_send_404(req);
  }

  httpd_resp_set_type(req, asset->content_type);
  return httpd_resp_send(req, reinterpret_cast<const char*>(asset->data), asset->size);
}

httpd_uri_t g_uri_health = {
    .uri = "/api/health",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_health,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_info = {
    .uri = "/api/info",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_info,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_version = {
    .uri = "/api/version",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_version,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_state = {
    .uri = "/api/state",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_state,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_ota = {
    .uri = "/api/ota",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_ota,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_ota_check = {
    .uri = "/api/ota/check",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_ota_check,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_leds_state = {
    .uri = "/api/leds/state",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_leds_state,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_leds_state_0 = {
    .uri = "/api/leds/state/0",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_leds_state_0,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_leds_state_1 = {
    .uri = "/api/leds/state/1",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_leds_state_1,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_leds_state_2 = {
    .uri = "/api/leds/state/2",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_leds_state_2,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_ws = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = route_ws,
    .user_ctx = nullptr,
    .is_websocket = true,
    .handle_ws_control_frames = true,
    .supported_subprotocol = nullptr,
};

httpd_uri_t g_uri_captive = {
    .uri = "/*",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_captive,
    .user_ctx = nullptr,
    .is_websocket = false,
    .handle_ws_control_frames = false,
    .supported_subprotocol = nullptr,
};

void stop_http_server() {
  if (g_httpd == nullptr) {
    return;
  }
  httpd_ssl_stop(g_httpd);
  g_httpd = nullptr;
}

void stop_captive_http_server() {
  if (g_captive_httpd == nullptr) {
    return;
  }
  httpd_stop(g_captive_httpd);
  g_captive_httpd = nullptr;
}

void start_http_server() {
  if (g_httpd != nullptr) {
    return;
  }
  if (!g_cfg.enabled) {
    return;
  }

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.httpd.server_port = g_cfg.port;
  conf.httpd.max_uri_handlers = 16;
  conf.httpd.task_priority = 5;
  conf.httpd.stack_size = 8192;
  conf.servercert = (const unsigned char*)kServerCertPgm;
  conf.servercert_len = sizeof(kServerCertPgm);
  conf.prvtkey_pem = (const unsigned char*)kServerKeyPgm;
  conf.prvtkey_len = sizeof(kServerKeyPgm);

  const esp_err_t err = httpd_ssl_start(&g_httpd, &conf);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP] start failed=%s", esp_err_to_name(err));
    g_httpd = nullptr;
    return;
  }

  httpd_register_uri_handler(g_httpd, &g_uri_health);
  httpd_register_uri_handler(g_httpd, &g_uri_info);
  httpd_register_uri_handler(g_httpd, &g_uri_version);
  httpd_register_uri_handler(g_httpd, &g_uri_state);
  httpd_register_uri_handler(g_httpd, &g_uri_ota);
  httpd_register_uri_handler(g_httpd, &g_uri_ota_check);
  httpd_register_uri_handler(g_httpd, &g_uri_leds_state);
  httpd_register_uri_handler(g_httpd, &g_uri_leds_state_0);
  httpd_register_uri_handler(g_httpd, &g_uri_leds_state_1);
  httpd_register_uri_handler(g_httpd, &g_uri_leds_state_2);
  httpd_register_uri_handler(g_httpd, &g_uri_ws);
  ESP_LOGI(kTag, "[HTTP] started");
  ESP_LOGI(kTag, "[HTTP] route=/api/health, /api/info, /api/version, /api/state, /api/ota, /api/ota/check, /api/leds/state, /api/leds/state/{0,1,2}, /ws");
}

void start_captive_http_server() {
  if (g_captive_httpd != nullptr) {
    return;
  }
  if (!g_cfg.enabled || g_cfg.port == kCaptiveHttpPort) {
    return;
  }

  httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
  conf.server_port = kCaptiveHttpPort;
  conf.max_uri_handlers = 4;
  conf.task_priority = 5;
  conf.stack_size = 8192;
  conf.uri_match_fn = httpd_uri_match_wildcard;

  const esp_err_t err = httpd_start(&g_captive_httpd, &conf);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP][CAPTIVE] start failed=%s", esp_err_to_name(err));
    g_captive_httpd = nullptr;
    return;
  }

  httpd_register_uri_handler(g_captive_httpd, &g_uri_ws);
  httpd_register_uri_handler(g_captive_httpd, &g_uri_captive);
  ESP_LOGI(kTag, "[HTTP][CAPTIVE] started on port %u", static_cast<unsigned>(kCaptiveHttpPort));
}

void sync_http_state() {
  if (g_httpd != nullptr || g_captive_httpd != nullptr) {
    set_state(HttpState::kStarted, "running");
  } else if (g_cfg.enabled && wifi_is_ip_ready()) {
    set_state(HttpState::kFailed, "start_failed");
  }
}

void http_loop(void*) {
  for (;;) {
    if (!g_cfg.enabled) {
      stop_http_server();
      stop_captive_http_server();
      set_state(HttpState::kDisabled, "disabled");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!wifi_is_ip_ready()) {
      set_state(HttpState::kInit, "waiting_ip");
      stop_http_server();
      stop_captive_http_server();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (g_httpd == nullptr) {
      start_http_server();
    }
    if (g_captive_httpd == nullptr) {
      start_captive_http_server();
    }
    if (g_httpd == nullptr && g_captive_httpd == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    sync_http_state();
    ws_broadcast_snapshot(g_httpd);
    ws_broadcast_snapshot(g_captive_httpd);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

const char* http_state_name() {
  return http_state_name_impl();
}

bool http_is_enabled() {
  return g_cfg.enabled;
}

bool http_is_running() {
  return g_httpd != nullptr || g_captive_httpd != nullptr;
}

void http_startup() {
  load_http_config();
  if (!g_cfg.enabled) {
    return;
  }
  if (g_task == nullptr) {
    (void)luce::start_task_once(g_task, http_loop, luce::task_budget::kTaskHttp);
  }
}

void http_status_for_cli() {
  ESP_LOGI(kTag, "http.status state=%s enabled=%d https_port=%u captive_port=%u", state_name(g_state), g_cfg.enabled ? 1 : 0,
           g_cfg.port, static_cast<unsigned>(kCaptiveHttpPort));
}

#else

bool http_is_enabled() {
  return false;
}

bool http_is_running() {
  return false;
}

void http_startup() {}
void http_status_for_cli() {}

#endif
