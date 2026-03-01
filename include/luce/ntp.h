#pragma once

#include "luce_build.h"

void ntp_startup();
void ntp_status_for_cli();
const char* ntp_state_name_current();
bool ntp_is_enabled();
bool ntp_is_synced();
