#pragma once

#include "luce_build.h"

void http_startup();
void http_status_for_cli();
bool http_is_enabled();
bool http_is_running();

#if LUCE_HAS_HTTP
const char* http_state_name();
#else
inline const char* http_state_name() { return "DISABLED"; }
#endif  // LUCE_HAS_HTTP
