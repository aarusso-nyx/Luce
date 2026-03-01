#pragma once

#include <cstddef>

#include "luce_build.h"

void ota_startup();
void ota_status_for_cli();
bool ota_is_enabled();
bool ota_is_running();
const char* ota_state_name();
void ota_build_status_payload(char* out, std::size_t out_size);

#if LUCE_HAS_OTA
void ota_request_check();
void ota_request_check_with_url(const char* url);
#else
inline void ota_request_check() {}
inline void ota_request_check_with_url(const char*) {}
inline bool ota_is_enabled() {
  return false;
}
inline bool ota_is_running() {
  return false;
}
inline const char* ota_state_name() {
  return "DISABLED";
}
inline void ota_build_status_payload(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  std::snprintf(out, out_size, "{\"enabled\":false,\"state\":\"DISABLED\",\"running\":false}");
}
#endif
