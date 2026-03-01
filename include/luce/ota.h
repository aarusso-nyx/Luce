#pragma once

#include <cstddef>

#include "luce_build.h"

void ota_startup();
void ota_status_for_cli();
bool ota_is_enabled();
bool ota_is_running();
const char* ota_state_name();
void ota_request_check();
void ota_request_check_with_url(const char* url);
void ota_build_status_payload(char* out, std::size_t out_size);
