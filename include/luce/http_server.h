#pragma once

#include "luce_build.h"

void http_startup();
void http_status_for_cli();

#if LUCE_HAS_HTTP
const char* http_state_name();
#else
inline const char* http_state_name() { return "DISABLED"; }
#endif  // LUCE_HAS_HTTP
