#pragma once

#include "luce_build.h"

#if LUCE_HAS_NVS

void dump_nvs_entries();
void update_boot_state_record();

#endif  // LUCE_HAS_NVS
