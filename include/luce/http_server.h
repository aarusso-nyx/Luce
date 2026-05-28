#pragma once

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
#else
inline const char* http_state_name() { return "DISABLED"; }
inline bool http_tls_dev_mode() { return false; }
inline bool http_tls_cert_present() { return false; }
inline bool http_tls_key_present() { return false; }
inline void http_tls_status_for_cli() {}
#endif  // LUCE_HAS_HTTP
