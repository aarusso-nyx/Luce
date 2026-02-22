// network.h: Wi-Fi, SNTP, and mDNS initialization via ESP-IDF
#pragma once
#include <stdbool.h>
// Initialize network (Wi-Fi, SNTP, mDNS) with device name; return false on failure
bool networkInit(const char* deviceName);