#pragma once

#include <cstdint>

#include "luce_build.h"

#if LUCE_HAS_WIFI
// Wi-Fi lifecycle entry points (compile-time gated via LUCE_HAS_WIFI).
// Stubs are provided in the #else branch for callers in earlier stages.

void wifi_startup();
void wifi_status_for_cli();
void wifi_scan_for_cli();

#else

inline void wifi_startup() {}
inline void wifi_status_for_cli() {}
inline void wifi_scan_for_cli() {}

#endif
