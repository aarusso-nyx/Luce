// HTTPS API server + captive HTTP web portal.
#include "luce/http_server.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
#include "luce/ntp.h"
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
constexpr const char* kDefaultToken = "luce-token";
constexpr const char* kUnauthorizedPayload = "{\"error\":\"unauthorized\"}";
constexpr const char* kMethodNotAllowedPayload = "{\"error\":\"method_not_allowed\",\"allowed\":\"%s\"}";

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
  char payload[256] = {0};
  std::snprintf(payload, sizeof(payload),
               "{\"service\":\"luce\",\"strategy\":\"%s\",\"wifi_ip\":\"%s\",\"http_enabled\":%s,\"http_port\":%u,\"tls\":%d,\"uptime_s\":%lld}",
               LUCE_STRATEGY_NAME, as_n_a(ip), g_cfg.enabled ? "true" : "false", g_cfg.port,
               g_cfg.tls_dev_mode ? 1 : 0, (long long)(esp_timer_get_time() / 1000000ULL));
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
  char url[256] = {0};
  if (req->content_len > 0 && req->content_len < sizeof(url)) {
    const int got = httpd_req_recv(req, url, req->content_len);
    if (got > 0) {
      url[got < static_cast<int>(sizeof(url)) ? got : static_cast<int>(sizeof(url) - 1)] = '\0';
    }
  }
  if (httpd_req_get_url_query_len(req) > 0 && httpd_req_get_url_query_len(req) < static_cast<int>(sizeof(query))) {
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      if (httpd_query_key_value(query, "url", url, sizeof(url)) == ESP_OK) {
        ota_request_check_with_url(url);
      } else {
        ota_request_check();
      }
    } else {
      ota_request_check();
    }
  } else {
    ota_request_check();
  }
  char payload[128] = {0};
  std::snprintf(payload, sizeof(payload), "{\"status\":\"queued\"}");
  return send_json(req, 202, payload, 0);
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
};

httpd_uri_t g_uri_info = {
    .uri = "/api/info",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_info,
    .user_ctx = nullptr,
};

httpd_uri_t g_uri_version = {
    .uri = "/api/version",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_version,
    .user_ctx = nullptr,
};

httpd_uri_t g_uri_state = {
    .uri = "/api/state",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_state,
    .user_ctx = nullptr,
};

httpd_uri_t g_uri_ota = {
    .uri = "/api/ota",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_ota,
    .user_ctx = nullptr,
};

httpd_uri_t g_uri_ota_check = {
    .uri = "/api/ota/check",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_ota_check,
    .user_ctx = nullptr,
};

httpd_uri_t g_uri_captive = {
    .uri = "/*",
    .method = static_cast<httpd_method_t>(HTTP_ANY),
    .handler = route_captive,
    .user_ctx = nullptr,
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
  conf.httpd.max_uri_handlers = 8;
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
  ESP_LOGI(kTag, "[HTTP] started");
  ESP_LOGI(kTag, "[HTTP] route=/api/health, /api/info, /api/version, /api/state, /api/ota, /api/ota/check");
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
