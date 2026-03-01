#pragma once

#include "luce_build.h"

void mqtt_startup();
void mqtt_status_for_cli();
void mqtt_pubtest_for_cli();
bool mqtt_is_enabled();
bool mqtt_is_connected();
bool mqtt_is_running();

#if LUCE_HAS_MQTT
const char* mqtt_state_name();
#else
inline const char* mqtt_state_name() { return "DISABLED"; }
#endif
