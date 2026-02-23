#pragma once

#include "luce_build.h"

#if LUCE_HAS_NTP

void ntp_startup();
void ntp_status_for_cli();

#else

inline void ntp_startup() {}
inline void ntp_status_for_cli() {}

#endif  // LUCE_HAS_NTP

