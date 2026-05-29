#pragma once

#include "esp_err.h"
#include "luce_build.h"

void http_startup();
void http_status_for_cli();
bool http_is_enabled();
bool http_is_running();

#if LUCE_HAS_HTTP
const char* http_state_name();
// Compatibility aliases for the HTTPS PKI role. The private key is generated
// and kept on-device; only CSR/certificate status and metadata are exposed.
void http_tls_status_for_cli();
esp_err_t http_tls_keygen_for_cli();
esp_err_t http_tls_csr_for_cli();
esp_err_t http_tls_cert_begin_for_cli();
esp_err_t http_tls_cert_append_for_cli(const char* line);
esp_err_t http_tls_cert_commit_for_cli();
esp_err_t http_tls_reset_for_cli();
#else
inline const char* http_state_name() { return "DISABLED"; }
inline void http_tls_status_for_cli() {}
inline esp_err_t http_tls_keygen_for_cli() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t http_tls_csr_for_cli() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t http_tls_cert_begin_for_cli() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t http_tls_cert_append_for_cli(const char*) { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t http_tls_cert_commit_for_cli() { return ESP_ERR_NOT_SUPPORTED; }
inline esp_err_t http_tls_reset_for_cli() { return ESP_ERR_NOT_SUPPORTED; }
#endif  // LUCE_HAS_HTTP
