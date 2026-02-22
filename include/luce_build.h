#pragma once

#ifndef LUCE_STAGE
#define LUCE_STAGE 0
#endif

#define LUCE_HAS_NVS ((LUCE_STAGE >= 1) ? 1 : 0)
#define LUCE_HAS_I2C ((LUCE_STAGE >= 2) ? 1 : 0)
#define LUCE_HAS_LCD ((LUCE_STAGE >= 3) ? 1 : 0)
#define LUCE_HAS_CLI ((LUCE_STAGE >= 4) ? 1 : 0)
#define LUCE_HAS_WIFI ((LUCE_STAGE >= 5) ? 1 : 0)

#ifndef LUCE_PROJECT_VERSION
#define LUCE_PROJECT_VERSION "0.0.0-stage0"
#endif

#ifndef LUCE_GIT_SHA
#define LUCE_GIT_SHA "placeholder"
#endif
