#pragma once

#include "luce_build.h"

void mdns_startup();
void mdns_status_for_cli();

#if LUCE_HAS_MDNS
const char* mdns_state_name();
#else
inline const char* mdns_state_name() { return "DISABLED"; }
#endif  // LUCE_HAS_MDNS
