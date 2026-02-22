
// http.h: handles HTTP API and static file serving
#pragma once
// http.h: uses ESP-IDF HTTP server for API and static file serving
#pragma once
#include "config.h"  // for SensorEventData
#include <esp_http_server.h>
#include <esp_netif.h>

// Initialize HTTP server (API, static files, captive portal, WebSocket)
esp_err_t httpInit();

// Call regularly in loop (no-op)
void httpLoop();

// Broadcast sensor readings to all WebSocket clients
void broadcastSensorData(const SensorEventData& s);
// Broadcast a relay state change to all WebSocket clients
void broadcastRelayEvent(uint8_t idx, bool state);
// Broadcast a log message to all WebSocket clients
void broadcastLog(const char* message);

// end of header