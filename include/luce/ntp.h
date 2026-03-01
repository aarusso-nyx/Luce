#pragma once

#include "luce_build.h"

#if LUCE_HAS_NTP

void ntp_startup();
void ntp_status_for_cli();
const char* ntp_state_name_current();

#else

inline void ntp_startup() {}
inline void ntp_status_for_cli() {}
inline const char* ntp_state_name_current() { return "DISABLED"; }

#endif  // LUCE_HAS_NTP
