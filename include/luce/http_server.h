#pragma once

#include "esp_err.h"
#include "luce_build.h"

void http_startup();
void http_status_for_cli();
bool http_is_enabled();
bool http_is_running();

#if LUCE_HAS_HTTP
const char* http_state_name();
// TLS material introspection. cert_pem / key_pem bodies are deliberately
// not exposed — only their presence is reported. Used by tls.status CLI
// and the /api/info endpoint.
bool http_tls_dev_mode();
bool http_tls_cert_present();
bool http_tls_key_present();
void http_tls_status_for_cli();
// Wipe the NVS-provisioned cert+key. Used by the tls.clear serial CLI
// command. Does NOT stop the running HTTPS server — the freshly-cleared
// material only takes effect on the next boot or after http_startup re-
// runs. Returns ESP_OK on success.
esp_err_t http_tls_clear_provisioned();
#else
inline const char* http_state_name() { return "DISABLED"; }
inline bool http_tls_dev_mode() { return false; }
inline bool http_tls_cert_present() { return false; }
inline bool http_tls_key_present() { return false; }
inline void http_tls_status_for_cli() {}
inline esp_err_t http_tls_clear_provisioned() { return ESP_OK; }
#endif  // LUCE_HAS_HTTP
