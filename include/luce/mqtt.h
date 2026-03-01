#pragma once

#include "luce_build.h"

void mqtt_startup();
void mqtt_status_for_cli();
void mqtt_pubtest_for_cli();

#if LUCE_HAS_MQTT
const char* mqtt_state_name();
#else
inline const char* mqtt_state_name() { return "DISABLED"; }
#endif
