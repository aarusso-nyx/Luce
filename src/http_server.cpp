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

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
extern "C" {
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/pk_private.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
}
#include "nvs.h"

#include "luce/net_wifi.h"
#include "luce/i2c_io.h"
#include "luce/led_status.h"
#include "luce/ntp.h"
#include "luce/mqtt.h"
#include "luce/ota.h"
#include "luce_build.h"
#include "luce/json_writer.h"
#include "luce/nvs_helpers.h"
#include "luce/runtime_state.h"
#include "luce/str_utils.h"
#include "luce/task_budgets.h"

// Captive-portal webapp assets are embedded by src/CMakeLists.txt as
// `extern "C"` symbols in a generated TU. The declarations must live at
// file scope, NOT inside the anonymous namespace below — GCC 15 honours
// the internal linkage of the surrounding anonymous namespace over the
// `extern "C"` linkage specifier and gives the symbols namespace-mangled
// names, which then fail to resolve against the C-linkage definitions.
extern "C" {
extern const unsigned char webapp_index_html[];
extern const unsigned int webapp_index_html_len;
extern const unsigned char webapp_app_css[];
extern const unsigned int webapp_app_css_len;
extern const unsigned char webapp_script_js[];
extern const unsigned int webapp_script_js_len;
}

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

// HTTPS identity is CSR-based: the device generates and keeps the private key
// locally, while only the CSR leaves the unit and only a signed certificate
// returns. Externally generated private-key imports are intentionally absent.
constexpr std::size_t kTlsKeyPemBufferSize = 768;
constexpr std::size_t kTlsCertPemBufferSize = 4096;
constexpr std::size_t kTlsCsrPemBufferSize = 1536;
constexpr std::size_t kTlsSubjectBufferSize = 128;
constexpr std::size_t kTlsFingerprintBufferSize = 96;
constexpr const char* kTlsKeyPemKey = "tls_key_pem";
constexpr const char* kTlsCertPemKey = "tls_cert_pem";
constexpr const char* kTlsCertStagedKey = "tls_cert_staged";
constexpr const char* kTlsKeyAlgKey = "tls_key_alg";
constexpr const char* kTlsKeyAlgEcP256 = "ec-p256";

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
  char cert_pem[kTlsCertPemBufferSize] = {};
  char key_pem[kTlsKeyPemBufferSize] = {};
  char cert_subject[kTlsSubjectBufferSize] = {};
  char cert_issuer[kTlsSubjectBufferSize] = {};
  char cert_fingerprint[kTlsFingerprintBufferSize] = {};
};

HttpConfig g_cfg {};
HttpState g_state = HttpState::kDisabled;
httpd_handle_t g_httpd = nullptr;
httpd_handle_t g_captive_httpd = nullptr;
TaskHandle_t g_task = nullptr;
bool g_cert_pem_oversized = false;
bool g_key_pem_oversized = false;
bool g_cert_pem_read_error = false;
bool g_key_pem_read_error = false;
const char* g_tls_last_error = "none";
esp_err_t g_tls_start_err = ESP_OK;

// Runtime HTTPS private-key import is intentionally absent. The private key is
// generated on-device, stored under http/tls_key_pem, and never printed. The
// only import path accepts a CA-signed certificate that must match that key.

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

void set_tls_error(const char* reason, esp_err_t err = ESP_OK) {
  g_tls_last_error = (reason != nullptr && reason[0] != '\0') ? reason : "none";
  g_tls_start_err = err;
}

bool tls_material_read_error() {
  return g_cert_pem_read_error || g_key_pem_read_error;
}

bool tls_material_oversized() {
  return g_cert_pem_oversized || g_key_pem_oversized;
}

bool tls_key_present() {
  return g_cfg.key_pem[0] != '\0';
}

bool tls_cert_present() {
  return g_cfg.cert_pem[0] != '\0';
}

void clear_cert_metadata() {
  g_cfg.cert_subject[0] = '\0';
  g_cfg.cert_issuer[0] = '\0';
  g_cfg.cert_fingerprint[0] = '\0';
}

void format_sha256_fingerprint(const unsigned char* data, std::size_t len, char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!data || len == 0) {
    return;
  }
  unsigned char digest[32] = {};
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (sha256 == nullptr || mbedtls_md(sha256, data, len, digest) != 0) {
    return;
  }
  std::size_t used = 0;
  for (std::size_t i = 0; i < sizeof(digest) && used + 3 < out_size; ++i) {
    const int written = std::snprintf(out + used, out_size - used, "%s%02X", i == 0 ? "" : ":", digest[i]);
    if (written <= 0) {
      break;
    }
    used += static_cast<std::size_t>(written);
  }
}

bool validate_cert_matches_key(const char* cert_pem, char* subject, std::size_t subject_size,
                               char* issuer, std::size_t issuer_size,
                               char* fingerprint, std::size_t fingerprint_size) {
  if (!cert_pem || cert_pem[0] == '\0' || g_cfg.key_pem[0] == '\0') {
    return false;
  }

  mbedtls_x509_crt crt;
  mbedtls_pk_context key;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&key);

  bool ok = false;
  int rc = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(cert_pem), std::strlen(cert_pem) + 1);
  if (rc != 0) {
    set_tls_error("cert_parse_failed");
    goto done;
  }

  rc = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(g_cfg.key_pem),
                            std::strlen(g_cfg.key_pem) + 1, nullptr, 0);
  if (rc != 0) {
    set_tls_error("key_parse_failed");
    goto done;
  }

  rc = mbedtls_pk_check_pair(&crt.pk, &key);
  if (rc != 0) {
    set_tls_error("cert_key_mismatch");
    goto done;
  }

  if (subject && subject_size > 0) {
    subject[0] = '\0';
    (void)mbedtls_x509_dn_gets(subject, subject_size, &crt.subject);
  }
  if (issuer && issuer_size > 0) {
    issuer[0] = '\0';
    (void)mbedtls_x509_dn_gets(issuer, issuer_size, &crt.issuer);
  }
  if (fingerprint && fingerprint_size > 0) {
    format_sha256_fingerprint(crt.raw.p, crt.raw.len, fingerprint, fingerprint_size);
  }
  ok = true;

done:
  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&crt);
  return ok;
}

void make_csr_subject(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  std::uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    std::snprintf(out, out_size, "CN=luce-%02x%02x%02x%02x%02x%02x,O=LUCE",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return;
  }
  std::snprintf(out, out_size, "CN=luce-device,O=LUCE");
}

const char* tls_status_name() {
  if (!g_cfg.enabled) {
    return "disabled";
  }
  if (tls_material_read_error()) {
    return "material_read_error";
  }
  if (tls_material_oversized()) {
    return "material_oversized";
  }
  if (!tls_key_present()) {
    return "key_missing";
  }
  if (!tls_cert_present()) {
    return "csr_ready";
  }
  if (g_httpd != nullptr) {
    return "started";
  }
  if (g_tls_start_err != ESP_OK) {
    return "start_failed";
  }
  if (!wifi_is_ip_ready()) {
    return "waiting_ip";
  }
  return "material_present";
}

esp_err_t nvs_set_http_str(const char* key, const char* value) {
  auto nvs = luce::nvs::Handle::Open(kHttpNs, NVS_READWRITE);
  if (!nvs.ok()) {
    return nvs.error();
  }
  ESP_RETURN_ON_ERROR(luce::nvs::write_string(nvs.raw(), key, value), kTag, "nvs_set_str(%s)", key);
  ESP_RETURN_ON_ERROR(luce::nvs::commit(nvs.raw()), kTag, "nvs_commit(%s)", key);
  return ESP_OK;
}

esp_err_t nvs_erase_http_key(const char* key) {
  auto nvs = luce::nvs::Handle::Open(kHttpNs, NVS_READWRITE);
  if (!nvs.ok()) {
    return nvs.error();
  }
  esp_err_t err = nvs_erase_key(nvs.raw(), key);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, kTag, "nvs_erase_key(%s)", key);
  ESP_RETURN_ON_ERROR(luce::nvs::commit(nvs.raw()), kTag, "nvs_commit erase %s", key);
  return ESP_OK;
}

int tls_hardware_rng(void*, unsigned char* output, std::size_t output_len) {
  if (output_len == 0) {
    return 0;
  }
  if (!output) {
    return -1;
  }
  esp_fill_random(output, output_len);
  return 0;
}

void load_http_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.enabled = false;
  g_cfg.port = kDefaultHttpPort;
  std::snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", kDefaultToken);
  g_cert_pem_oversized = false;
  g_key_pem_oversized = false;
  g_cert_pem_read_error = false;
  g_key_pem_read_error = false;
  clear_cert_metadata();
  set_tls_error("none");

  nvs_handle_t handle = 0;
  if (nvs_open(kHttpNs, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP] namespace '%s' not found; defaults active", kHttpNs);
    set_state(HttpState::kDisabled, "namespace_missing");
    return;
  }

  std::uint8_t enabled = 0;
  std::uint16_t port = kDefaultHttpPort;
  if (luce::nvs::read_u8(handle, "enabled", enabled, 0)) {
    g_cfg.enabled = (enabled != 0);
  }
  if (luce::nvs::read_u16(handle, "port", port, kDefaultHttpPort) && port != 0) {
    g_cfg.port = port;
  }
  (void)luce::nvs::read_string(handle, "token", g_cfg.token, sizeof(g_cfg.token), kDefaultToken);
  // TLS key/cert: read silently into runtime buffers. The key body is never
  // logged or returned; only presence and status are reported.
  const auto cert_status = luce::nvs::read_string_status(handle, kTlsCertPemKey, g_cfg.cert_pem, sizeof(g_cfg.cert_pem), "");
  const auto key_status = luce::nvs::read_string_status(handle, kTlsKeyPemKey, g_cfg.key_pem, sizeof(g_cfg.key_pem), "");
  g_cert_pem_oversized = (cert_status == luce::nvs::StringReadStatus::kOversized);
  g_key_pem_oversized = (key_status == luce::nvs::StringReadStatus::kOversized);
  g_cert_pem_read_error = (cert_status == luce::nvs::StringReadStatus::kReadError);
  g_key_pem_read_error = (key_status == luce::nvs::StringReadStatus::kReadError);
  nvs_close(handle);

  if (g_cert_pem_oversized || g_key_pem_oversized) {
    set_tls_error("tls_material_oversized");
    ESP_LOGE(kTag,
             "[HTTP] TLS material rejected: cert_oversized=%d key_oversized=%d max_pem_bytes=%u",
             g_cert_pem_oversized ? 1 : 0, g_key_pem_oversized ? 1 : 0,
             static_cast<unsigned>(kTlsCertPemBufferSize - 1));
  } else if (g_cert_pem_read_error || g_key_pem_read_error) {
    set_tls_error("tls_material_read_error");
    ESP_LOGW(kTag, "[HTTP] TLS material read error: cert_read_error=%d key_read_error=%d",
             g_cert_pem_read_error ? 1 : 0, g_key_pem_read_error ? 1 : 0);
  } else if (g_cfg.cert_pem[0] != '\0' && g_cfg.key_pem[0] != '\0') {
    if (!validate_cert_matches_key(g_cfg.cert_pem, g_cfg.cert_subject, sizeof(g_cfg.cert_subject),
                                   g_cfg.cert_issuer, sizeof(g_cfg.cert_issuer),
                                   g_cfg.cert_fingerprint, sizeof(g_cfg.cert_fingerprint))) {
      ESP_LOGW(kTag, "[HTTP] TLS certificate does not validate against local key");
    }
  }

  set_state(g_cfg.enabled ? HttpState::kInit : HttpState::kDisabled, g_cfg.enabled ? "config_enabled" : "config_disabled");
  ESP_LOGI(kTag, "[HTTP] enabled=%d port=%u key_present=%d cert_present=%d tls_status=%s",
           g_cfg.enabled ? 1 : 0, g_cfg.port,
           g_cfg.key_pem[0] != '\0' ? 1 : 0, g_cfg.cert_pem[0] != '\0' ? 1 : 0,
           tls_status_name());
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

// Route table — one row per JSON API endpoint. Each route's implementation
// runs through route_dispatch_trampoline below, which performs the method
// check then the optional auth check before delegating to `impl`. The
// method check runs first so unauthenticated callers cannot probe for a
// route's allowed methods. Method mask uses a bitset of httpd_method_t
// values (HTTP_GET, HTTP_POST, …) so a single route can advertise GET+PUT
// or POST+PUT cleanly. methods_label is the Allow-header / 405-payload
// string used when a request misses the mask.
struct RouteSpec {
  const char* uri;
  std::uint16_t method_mask;
  bool requires_auth;
  esp_err_t (*impl)(httpd_req_t* req);
  const char* methods_label;
};

constexpr std::uint16_t kMethodGet = 1u << HTTP_GET;
constexpr std::uint16_t kMethodPost = 1u << HTTP_POST;
constexpr std::uint16_t kMethodPut = 1u << HTTP_PUT;

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
  const int written = std::snprintf(expected, sizeof(expected), "Bearer %s", g_cfg.token);
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(expected)) {
    return false;
  }
  return std::strlen(header) == static_cast<std::size_t>(written) &&
         std::strcmp(header, expected) == 0;
}

bool parse_bool_token(const char* text, bool* out_value) {
  return luce::str::parse_bool_token(text, out_value);
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
  return luce::str::parse_u32_token(text, out_value);
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
  (void)luce::str::trim_ascii_inplace(text);
}

esp_err_t get_uptime_payload(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return ESP_FAIL;
  }
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  luce::json::Writer writer(out, out_size);
  writer.begin_object();
  writer.key_str("service", "luce");
  writer.key_str("strategy", LUCE_STRATEGY_NAME);
  writer.key_str("status", "ok");
  writer.key_str("build", __DATE__ " " __TIME__);
  writer.key_str("sha", LUCE_GIT_SHA);
  writer.key_uint("uptime_s", static_cast<std::uint32_t>(esp_timer_get_time() / 1000000ULL));
  writer.key_str("wifi_ip", as_n_a(ip));
  writer.end_object();
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
  const bool hardware_degraded = io_hardware_degraded();
	  const bool day = has_sensor ? (snapshot.light_raw > static_cast<int>(threshold)) : false;
  std::time_t now = 0;
  (void)std::time(&now);
  luce::json::Writer writer(out, out_size);
  writer.begin_object();
  writer.key_str("type", "state");
  writer.key_uint("tstamp", static_cast<std::uint32_t>(now > 0 ? now : 0));
  writer.key_uint("state", g_relay_mask);
  writer.key_uint("night", night_mask);
  writer.key_uint("day", day ? 1u : 0u);
  writer.key_bool("hardware_degraded", hardware_degraded);
  writer.key_uint("threshold", threshold);
  writer.key_int("light", has_sensor ? snapshot.light_raw : 0);
  writer.key_int("voltage", has_sensor ? snapshot.voltage_raw : 0);
  writer.key_double("temperature", has_sensor ? snapshot.temperature_c : 0.0f, 1);
  writer.key_double("humidity", has_sensor ? snapshot.humidity_percent : 0.0f, 1);
  writer.key_bool("sensor_ok", has_sensor && snapshot.dht_ok);
  writer.end_object();
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

esp_err_t route_health_impl(httpd_req_t* req) {
  char payload[256] = {0};
  get_uptime_payload(payload, sizeof(payload));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_info_impl(httpd_req_t* req) {
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  I2cSensorSnapshot snapshot {};
  const bool has_sensor = read_sensor_snapshot(snapshot);
	  const std::uint16_t threshold = io_light_threshold();
	  const std::uint8_t night_mask = io_relay_night_mask();
  const bool hardware_degraded = io_hardware_degraded();
	  const bool day = has_sensor ? (snapshot.light_raw > static_cast<int>(threshold)) : false;
  const bool cert_present = (g_cfg.cert_pem[0] != '\0');
  const bool key_present = (g_cfg.key_pem[0] != '\0');
  char payload[1792] = {0};
  luce::json::Writer writer(payload, sizeof(payload));
  writer.begin_object();
  writer.key_str("service", "luce");
  writer.key_str("name", "luce");
  writer.key_str("version", LUCE_PROJECT_VERSION);
  writer.key_str("strategy", LUCE_STRATEGY_NAME);
  writer.key_str("sha", LUCE_GIT_SHA);
  writer.key_str("build", __DATE__ " " __TIME__);
  writer.key_uint("uptimeMs", static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL));
  writer.key_uint("uptime_s", static_cast<std::uint32_t>(esp_timer_get_time() / 1000000ULL));
  writer.key_str("wifi_ip", as_n_a(ip));
  writer.key_bool("http_enabled", g_cfg.enabled);
  writer.key_uint("http_port", g_cfg.port);
  writer.key_str("http_state", state_name(g_state));
  writer.key_bool("https_running", g_httpd != nullptr);
  writer.key_str("tls_state", tls_status_name());
  writer.key_str("tls_status", tls_status_name());
  writer.key_str("tls_last_error", g_tls_last_error);
  writer.key_bool("key_present", key_present);
  writer.key_bool("csr_ready", key_present);
  writer.key_bool("cert_present", cert_present);
  writer.key_str("cert_fingerprint", g_cfg.cert_fingerprint);
  writer.key_str("cert_subject", g_cfg.cert_subject);
  writer.key_str("cert_issuer", g_cfg.cert_issuer);
  writer.key_uint("relays", g_relay_mask);
  writer.key_uint("nightMask", night_mask);
  writer.key_bool("day", day);
  writer.key_bool("hardware_degraded", hardware_degraded);
  writer.key_uint("threshold", threshold);
  writer.key_int("light", has_sensor ? snapshot.light_raw : 0);
  writer.key_double("temperature", has_sensor ? snapshot.temperature_c : 0.0f, 1);
  writer.key_double("humidity", has_sensor ? snapshot.humidity_percent : 0.0f, 1);
  writer.key_bool("sensor_ok", has_sensor && snapshot.dht_ok);
  writer.key_object_begin("network");
  writer.key_str("ip", as_n_a(ip));
  writer.key_bool("wifiConnected", wifi_is_connected());
  writer.key_bool("mqttConnected", mqtt_is_connected());
  writer.key_bool("ntpSynced", ntp_is_synced());
  writer.end_object();
  writer.end_object();
  return send_json(req, 200, payload, 0);
}

esp_err_t route_version_impl(httpd_req_t* req) {
  char payload[192] = {0};
  luce::json::Writer writer(payload, sizeof(payload));
  writer.begin_object();
  writer.key_str("service", "luce");
  writer.key_str("version", LUCE_PROJECT_VERSION);
  writer.key_str("strategy", LUCE_STRATEGY_NAME);
  writer.key_str("sha", LUCE_GIT_SHA);
  writer.key_str("build", __DATE__ " " __TIME__);
  writer.end_object();
  return send_json(req, 200, payload, 0);
}

esp_err_t route_state_impl(httpd_req_t* req) {
  char payload[384] = {0};
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));
  luce::json::Writer writer(payload, sizeof(payload));
  writer.begin_object();
  writer.key_str("state", "running");
  writer.key_str("wifi_ip", as_n_a(ip));
  writer.key_uint("relay", g_relay_mask);
  writer.key_uint("buttons", g_button_mask);
  writer.key_bool("hardware_degraded", io_hardware_degraded());
  writer.key_uint("requests", 1);
  writer.key_uint("unauth", 0);
  writer.key_str("service", "luce");
  writer.key_str("strategy", LUCE_STRATEGY_NAME);
  writer.key_uint("ntp_state", 0);
  writer.end_object();
  return send_json(req, 200, payload, 0);
}

esp_err_t route_ota_impl(httpd_req_t* req) {
  char payload[512] = {0};
  ota_build_status_payload(payload, sizeof(payload));
  return send_json(req, 200, payload, 0);
}

esp_err_t route_ota_check_impl(httpd_req_t* req) {
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
  luce::json::Writer writer(payload, sizeof(payload));
  writer.begin_object();
  writer.key_str("status", "queued");
  writer.key_str("source", source);
  writer.end_object();
  return send_json(req, 202, payload, 0);
}

esp_err_t build_leds_payload(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return ESP_FAIL;
  }
  const std::uint8_t state = led_status_current_mask();
  const std::uint8_t manual_enabled = led_status_manual_enabled_mask();
  const std::uint8_t manual_value = led_status_manual_value_mask();
  luce::json::Writer writer(out, out_size);
  writer.begin_object();
  writer.key_uint("state", state);
  writer.key_uint("manual_enabled", manual_enabled);
  writer.key_uint("manual_value", manual_value);
  writer.key_str("mode0", led_manual_mode_name(led_status_manual_mode(0)));
  writer.key_str("mode1", led_manual_mode_name(led_status_manual_mode(1)));
  writer.key_str("mode2", led_manual_mode_name(led_status_manual_mode(2)));
  writer.end_object();
  return ESP_OK;
}

esp_err_t route_leds_state_impl(httpd_req_t* req) {
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

esp_err_t route_leds_state_0_impl(httpd_req_t* req) {
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

esp_err_t route_leds_state_1_impl(httpd_req_t* req) {
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

esp_err_t route_leds_state_2_impl(httpd_req_t* req) {
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

constexpr RouteSpec kJsonApiRoutes[] = {
    {"/api/health",       kMethodGet,                false, route_health_impl,       "GET"},
    {"/api/info",         kMethodGet,                true,  route_info_impl,         "GET"},
    {"/api/version",      kMethodGet,                false, route_version_impl,      "GET"},
    {"/api/state",        kMethodGet,                true,  route_state_impl,        "GET"},
    {"/api/ota",          kMethodGet,                true,  route_ota_impl,          "GET"},
    {"/api/ota/check",    kMethodPost | kMethodPut,  true,  route_ota_check_impl,    "POST, PUT"},
    {"/api/leds/state",   kMethodGet | kMethodPut,   true,  route_leds_state_impl,   "GET, PUT"},
    {"/api/leds/state/0", kMethodGet | kMethodPut,   true,  route_leds_state_0_impl, "GET, PUT"},
    {"/api/leds/state/1", kMethodGet | kMethodPut,   true,  route_leds_state_1_impl, "GET, PUT"},
    {"/api/leds/state/2", kMethodGet | kMethodPut,   true,  route_leds_state_2_impl, "GET, PUT"},
};
constexpr std::size_t kJsonApiRouteCount = sizeof(kJsonApiRoutes) / sizeof(kJsonApiRoutes[0]);

// httpd_uri_t entries point user_ctx at the corresponding RouteSpec so the
// single trampoline can read method/auth metadata at dispatch time. Built
// at first call to start_http_server() to keep this TU header-light.
httpd_uri_t g_json_uri_handlers[kJsonApiRouteCount] = {};

esp_err_t route_dispatch_trampoline(httpd_req_t* req) {
  const auto* spec = static_cast<const RouteSpec*>(req->user_ctx);
  if (spec == nullptr) {
    return send_method_not_allowed(req, "GET");
  }
  const std::uint16_t method_bit = (req->method >= 0 && req->method < 16)
                                       ? static_cast<std::uint16_t>(1u << req->method)
                                       : 0u;
  if ((spec->method_mask & method_bit) == 0u) {
    return send_method_not_allowed(req, spec->methods_label);
  }
  if (spec->requires_auth && !validate_auth(req)) {
    return send_unauthorized(req);
  }
  return spec->impl(req);
}

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

  if (tls_material_read_error()) {
    ESP_LOGE(kTag, "[HTTP] start refused: TLS material read error (cert_read_error=%d key_read_error=%d)",
             g_cert_pem_read_error ? 1 : 0, g_key_pem_read_error ? 1 : 0);
    set_tls_error("tls_material_read_error");
    set_state(HttpState::kFailed, "tls_material_read_error");
    return;
  }

  if (tls_material_oversized()) {
    ESP_LOGE(kTag,
             "[HTTP] start refused: TLS material oversized (cert_oversized=%d key_oversized=%d max_pem_bytes=%u)",
             g_cert_pem_oversized ? 1 : 0, g_key_pem_oversized ? 1 : 0,
             static_cast<unsigned>(kTlsCertPemBufferSize - 1));
    set_tls_error("tls_material_oversized");
    set_state(HttpState::kFailed, "tls_material_oversized");
    return;
  }

  if (g_cfg.key_pem[0] == '\0') {
    ESP_LOGE(kTag,
             "[HTTP] start refused: missing local TLS key; run tls.keygen then tls.csr/sign/import");
    set_tls_error("tls_key_missing");
    set_state(HttpState::kFailed, "tls_key_missing");
    return;
  }

  if (g_cfg.cert_pem[0] == '\0') {
    ESP_LOGE(kTag,
             "[HTTP] start refused: missing signed TLS certificate; export CSR with tls.csr and import cert with tls.cert.*");
    set_tls_error("tls_cert_missing");
    set_state(HttpState::kFailed, "tls_cert_missing");
    return;
  }

  if (!validate_cert_matches_key(g_cfg.cert_pem, g_cfg.cert_subject, sizeof(g_cfg.cert_subject),
                                 g_cfg.cert_issuer, sizeof(g_cfg.cert_issuer),
                                 g_cfg.cert_fingerprint, sizeof(g_cfg.cert_fingerprint))) {
    ESP_LOGE(kTag, "[HTTP] start refused: signed certificate does not match local key");
    set_state(HttpState::kFailed, g_tls_last_error);
    return;
  }

  const std::size_t cert_len = std::strlen(g_cfg.cert_pem) + 1;
  const std::size_t key_len = std::strlen(g_cfg.key_pem) + 1;

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.httpd.server_port = g_cfg.port;
  conf.httpd.max_uri_handlers = 16;
  conf.httpd.task_priority = 5;
  conf.httpd.stack_size = 8192;
  conf.servercert = reinterpret_cast<const unsigned char*>(g_cfg.cert_pem);
  conf.servercert_len = cert_len;
  conf.prvtkey_pem = reinterpret_cast<const unsigned char*>(g_cfg.key_pem);
  conf.prvtkey_len = key_len;

  const esp_err_t err = httpd_ssl_start(&g_httpd, &conf);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP] start failed=%s", esp_err_to_name(err));
    g_httpd = nullptr;
    set_tls_error("httpd_ssl_start_failed", err);
    set_state(HttpState::kFailed, "httpd_ssl_start_failed");
    return;
  }
  set_tls_error("none");

  for (std::size_t i = 0; i < kJsonApiRouteCount; ++i) {
    const RouteSpec& spec = kJsonApiRoutes[i];
    g_json_uri_handlers[i] = httpd_uri_t{
        .uri = spec.uri,
        .method = static_cast<httpd_method_t>(HTTP_ANY),
        .handler = route_dispatch_trampoline,
        .user_ctx = const_cast<RouteSpec*>(&spec),
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &g_json_uri_handlers[i]);
  }
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
  ESP_LOGI(kTag,
           "http.status state=%s enabled=%d https_running=%d https_port=%u captive_running=%d captive_port=%u",
           state_name(g_state), g_cfg.enabled ? 1 : 0, g_httpd != nullptr ? 1 : 0,
           g_cfg.port, g_captive_httpd != nullptr ? 1 : 0, static_cast<unsigned>(kCaptiveHttpPort));
}

void http_tls_status_for_cli() {
  const std::size_t cert_len = std::strlen(g_cfg.cert_pem);
  const std::size_t key_len = std::strlen(g_cfg.key_pem);
  const bool ready = (cert_len > 0 && key_len > 0 && !tls_material_read_error() && !tls_material_oversized());
  ESP_LOGI(kTag,
           "tls.status workflow=csr key_alg=%s key_present=%d key_pem_bytes=%u "
           "csr_ready=%d cert_present=%d cert_pem_bytes=%u http_state=%s "
           "https_running=%d tls_state=%s last_error=%s start_err=%s ready=%d "
           "fingerprint=%s subject='%s' issuer='%s'",
           kTlsKeyAlgEcP256, key_len > 0 ? 1 : 0, static_cast<unsigned>(key_len),
           key_len > 0 ? 1 : 0, cert_len > 0 ? 1 : 0, static_cast<unsigned>(cert_len),
           state_name(g_state), g_httpd != nullptr ? 1 : 0, tls_status_name(),
           g_tls_last_error, esp_err_to_name(g_tls_start_err), ready ? 1 : 0,
           g_cfg.cert_fingerprint[0] != '\0' ? g_cfg.cert_fingerprint : "n/a",
           g_cfg.cert_subject[0] != '\0' ? g_cfg.cert_subject : "n/a",
           g_cfg.cert_issuer[0] != '\0' ? g_cfg.cert_issuer : "n/a");
}

esp_err_t http_tls_keygen_for_cli() {
  if (g_cfg.key_pem[0] != '\0') {
    ESP_LOGW(kTag, "tls.keygen refused: key already exists; run tls.reset to reprovision");
    return ESP_ERR_INVALID_STATE;
  }

  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_pk_context key;
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_pk_init(&key);

  int rc = 0;
  const char* personalization = "luce-http-tls-keygen";
  rc = mbedtls_ctr_drbg_seed(&ctr_drbg, tls_hardware_rng, nullptr,
                             reinterpret_cast<const unsigned char*>(personalization),
                             std::strlen(personalization));
  if (rc != 0) {
    set_tls_error("keygen_rng_failed");
    goto done;
  }

  rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (rc != 0) {
    set_tls_error("keygen_pk_setup_failed");
    goto done;
  }
  rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                           mbedtls_ctr_drbg_random, &ctr_drbg);
  if (rc != 0) {
    set_tls_error("keygen_ec_failed");
    goto done;
  }

  std::memset(g_cfg.key_pem, 0, sizeof(g_cfg.key_pem));
  rc = mbedtls_pk_write_key_pem(&key, reinterpret_cast<unsigned char*>(g_cfg.key_pem), sizeof(g_cfg.key_pem));
  if (rc != 0) {
    set_tls_error("keygen_pem_failed");
    goto done;
  }

  if (nvs_set_http_str(kTlsKeyPemKey, g_cfg.key_pem) != ESP_OK ||
      nvs_set_http_str(kTlsKeyAlgKey, kTlsKeyAlgEcP256) != ESP_OK) {
    set_tls_error("keygen_nvs_failed");
    rc = -1;
    goto done;
  }
  (void)nvs_erase_http_key(kTlsCertPemKey);
  g_cfg.cert_pem[0] = '\0';
  clear_cert_metadata();
  set_tls_error("none");
  ESP_LOGI(kTag, "tls.keygen generated local %s private key; export CSR with tls.csr", kTlsKeyAlgEcP256);

done:
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  if (rc != 0) {
    ESP_LOGE(kTag, "tls.keygen failed rc=-0x%04X", static_cast<unsigned>(-rc));
    return ESP_FAIL;
  }
  return ESP_OK;
}

bool make_csr(char* out, std::size_t out_size) {
  if (!out || out_size == 0 || g_cfg.key_pem[0] == '\0') {
    return false;
  }

  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_pk_context key;
  mbedtls_x509write_csr csr;
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_pk_init(&key);
  mbedtls_x509write_csr_init(&csr);

  int rc = 0;
  char subject[kTlsSubjectBufferSize] = {};
  const char* personalization = "luce-http-tls-csr";
  rc = mbedtls_ctr_drbg_seed(&ctr_drbg, tls_hardware_rng, nullptr,
                             reinterpret_cast<const unsigned char*>(personalization),
                             std::strlen(personalization));
  if (rc != 0) {
    set_tls_error("csr_rng_failed");
    goto done;
  }

  rc = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(g_cfg.key_pem),
                            std::strlen(g_cfg.key_pem) + 1, nullptr, 0);
  if (rc != 0) {
    set_tls_error("csr_key_parse_failed");
    goto done;
  }

  mbedtls_x509write_csr_set_key(&csr, &key);
  mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
  make_csr_subject(subject, sizeof(subject));
  rc = mbedtls_x509write_csr_set_subject_name(&csr, subject);
  if (rc != 0) {
    set_tls_error("csr_subject_failed");
    goto done;
  }
  rc = mbedtls_x509write_csr_set_key_usage(&csr, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
  if (rc != 0) {
    set_tls_error("csr_key_usage_failed");
    goto done;
  }
  rc = mbedtls_x509write_csr_pem(&csr, reinterpret_cast<unsigned char*>(out), out_size);
  if (rc != 0) {
    set_tls_error("csr_write_failed");
    goto done;
  }

done:
  mbedtls_x509write_csr_free(&csr);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  return rc == 0;
}

esp_err_t http_tls_csr_for_cli() {
  if (g_cfg.key_pem[0] == '\0') {
    ESP_LOGW(kTag, "tls.csr unavailable: key missing; run tls.keygen");
    return ESP_ERR_INVALID_STATE;
  }
  char csr[kTlsCsrPemBufferSize] = {};
  if (!make_csr(csr, sizeof(csr))) {
    ESP_LOGE(kTag, "tls.csr failed: %s", g_tls_last_error);
    return ESP_FAIL;
  }
  ESP_LOGI(kTag, "tls.csr begin");
  char* line = std::strtok(csr, "\n");
  while (line != nullptr) {
    ESP_LOGI(kTag, "%s", line);
    line = std::strtok(nullptr, "\n");
  }
  ESP_LOGI(kTag, "tls.csr end");
  return ESP_OK;
}

esp_err_t http_tls_cert_begin_for_cli() {
  ESP_RETURN_ON_ERROR(nvs_set_http_str(kTlsCertStagedKey, ""), kTag, "stage cert begin");
  ESP_LOGI(kTag, "tls.cert.begin ok");
  return ESP_OK;
}

esp_err_t http_tls_cert_append_for_cli(const char* line) {
  if (!line || line[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }
  char staged[kTlsCertPemBufferSize] = {};
  auto nvs = luce::nvs::Handle::Open(kHttpNs, NVS_READONLY);
  if (nvs.ok()) {
    (void)nvs.read_string(kTlsCertStagedKey, staged, sizeof(staged), "");
  }
  const std::size_t current = std::strlen(staged);
  const std::size_t add = std::strlen(line);
  if (current + add + 2 >= sizeof(staged)) {
    ESP_LOGE(kTag, "tls.cert.append refused: staged cert too large");
    return ESP_ERR_NO_MEM;
  }
  std::strncat(staged, line, sizeof(staged) - std::strlen(staged) - 1);
  std::strncat(staged, "\n", sizeof(staged) - std::strlen(staged) - 1);
  ESP_RETURN_ON_ERROR(nvs_set_http_str(kTlsCertStagedKey, staged), kTag, "stage cert append");
  ESP_LOGI(kTag, "tls.cert.append ok bytes=%u", static_cast<unsigned>(std::strlen(staged)));
  return ESP_OK;
}

esp_err_t http_tls_cert_commit_for_cli() {
  if (g_cfg.key_pem[0] == '\0') {
    ESP_LOGE(kTag, "tls.cert.commit refused: key missing; run tls.keygen first");
    return ESP_ERR_INVALID_STATE;
  }

  char staged[kTlsCertPemBufferSize] = {};
  auto nvs = luce::nvs::Handle::Open(kHttpNs, NVS_READONLY);
  if (!nvs.ok()) {
    return nvs.error();
  }
  if (!nvs.read_string(kTlsCertStagedKey, staged, sizeof(staged), "") || staged[0] == '\0') {
    ESP_LOGE(kTag, "tls.cert.commit refused: staged certificate missing");
    return ESP_ERR_NOT_FOUND;
  }

  char subject[kTlsSubjectBufferSize] = {};
  char issuer[kTlsSubjectBufferSize] = {};
  char fingerprint[kTlsFingerprintBufferSize] = {};
  if (!validate_cert_matches_key(staged, subject, sizeof(subject), issuer, sizeof(issuer), fingerprint, sizeof(fingerprint))) {
    ESP_LOGE(kTag, "tls.cert.commit refused: %s", g_tls_last_error);
    return ESP_ERR_INVALID_ARG;
  }

  ESP_RETURN_ON_ERROR(nvs_set_http_str(kTlsCertPemKey, staged), kTag, "promote cert");
  (void)nvs_erase_http_key(kTlsCertStagedKey);
  std::snprintf(g_cfg.cert_pem, sizeof(g_cfg.cert_pem), "%s", staged);
  std::snprintf(g_cfg.cert_subject, sizeof(g_cfg.cert_subject), "%s", subject);
  std::snprintf(g_cfg.cert_issuer, sizeof(g_cfg.cert_issuer), "%s", issuer);
  std::snprintf(g_cfg.cert_fingerprint, sizeof(g_cfg.cert_fingerprint), "%s", fingerprint);
  set_tls_error("none");
  ESP_LOGI(kTag, "tls.cert.commit ok fingerprint=%s subject='%s'", g_cfg.cert_fingerprint, g_cfg.cert_subject);
  return ESP_OK;
}

esp_err_t http_tls_reset_for_cli() {
  (void)nvs_erase_http_key(kTlsCertStagedKey);
  (void)nvs_erase_http_key(kTlsCertPemKey);
  (void)nvs_erase_http_key(kTlsKeyPemKey);
  (void)nvs_erase_http_key(kTlsKeyAlgKey);
  std::memset(g_cfg.key_pem, 0, sizeof(g_cfg.key_pem));
  std::memset(g_cfg.cert_pem, 0, sizeof(g_cfg.cert_pem));
  clear_cert_metadata();
  set_tls_error("none");
  ESP_LOGW(kTag, "tls.reset erased local key and certificate; reboot recommended");
  return ESP_OK;
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
