#pragma once

#include <cstddef>
#include <cstdint>

#include "luce_build.h"

void wifi_startup();
void wifi_status_for_cli();
void wifi_scan_for_cli();
bool wifi_is_enabled();
bool wifi_is_connecting();
bool wifi_is_ip_ready();
bool wifi_is_connected();
void wifi_get_ssid(char* out, std::size_t out_size);
void wifi_copy_ip_str(char* out, std::size_t out_size);
void wifi_get_rssi(int* rssi_out);
