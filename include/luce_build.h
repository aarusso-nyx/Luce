#pragma once

#ifndef LUCE_NET_CORE
#define LUCE_NET_CORE 0
#endif

#ifndef LUCE_NET_MQTT
#define LUCE_NET_MQTT 0
#endif

#ifndef LUCE_NET_HTTP
#define LUCE_NET_HTTP 0
#endif

// Baseline hardware capabilities are always present in the canonical firmware build.
#define LUCE_HAS_WIFI (LUCE_NET_CORE ? 1 : 0)
#define LUCE_HAS_NTP (LUCE_NET_CORE ? 1 : 0)
#define LUCE_HAS_MDNS (LUCE_NET_CORE ? 1 : 0)
#define LUCE_HAS_TCP_CLI (LUCE_NET_CORE ? 1 : 0)

#define LUCE_HAS_MQTT (LUCE_NET_MQTT ? 1 : 0)
#define LUCE_HAS_HTTP (LUCE_NET_HTTP ? 1 : 0)

#if LUCE_NET_CORE
#if LUCE_NET_MQTT
#if LUCE_NET_HTTP
#define LUCE_STRATEGY_NAME "NET1"
#else
#define LUCE_STRATEGY_NAME "MQTT"
#endif
#elif LUCE_NET_HTTP
#define LUCE_STRATEGY_NAME "HTTP"
#else
#define LUCE_STRATEGY_NAME "NET0"
#endif
#else
#define LUCE_STRATEGY_NAME "CORE"
#endif

#ifndef LUCE_PROJECT_VERSION
#define LUCE_PROJECT_VERSION "0.0.0-core"
#endif

#ifndef LUCE_GIT_SHA
#define LUCE_GIT_SHA "placeholder"
#endif
