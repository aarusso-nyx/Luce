// util.h: Common helpers for time formatting, CLI prefixes, and error handling
#pragma once

#include <time.h>

// #include <esp_timer.h>
// // Replacement for Arduino millis()
// inline uint64_t millis() { return esp_timer_get_time() / 1000ULL; }

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "leds.h"

#include <string>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <algorithm>
// Shared helper for safe formatting into std::string (max size enforced)
// Helper: format into a std::string using heap buffer to minimize stack usage
inline std::string utilFormatSafe(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  // Determine required length
  va_list argsCopy;
  va_copy(argsCopy, args);
  int len = vsnprintf(nullptr, 0, fmt, argsCopy);
  va_end(argsCopy);
  if (len <= 0) {
    va_end(args);
    return std::string();
  }
  // Allocate string buffer
  std::string s;
  s.resize(static_cast<size_t>(len));
  // Write formatted data
  vsnprintf(&s[0], len + 1, fmt, args);
  va_end(args);
  return s;
}
// Trim leading and trailing whitespace from a std::string
inline void utilTrim(std::string& s) {
  auto notSpaceFront = [](int ch) { return !std::isspace(ch); };
  auto it = std::find_if(s.begin(), s.end(), notSpaceFront);
  s.erase(s.begin(), it);
  auto it2 = std::find_if(s.rbegin(), s.rend(), notSpaceFront).base();
  s.erase(it2, s.end());
}

// Get current time in ISO8601 format (e.g. "2023-07-12T15:04:05Z")
inline void utilGetIsoTime(char* buf, size_t len) {
  time_t t = time(nullptr);
  struct tm tmst;
  localtime_r(&t, &tmst);
  strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tmst);
}

// Build CLI or log prefix: optional timestamp and task name
inline void utilBuildPrefix(char* buf, size_t len,
                            bool printTimestamp,
                            bool printTaskName) {
  char iso[32]; utilGetIsoTime(iso, sizeof(iso));
  const char* raw = pcTaskGetName(NULL);
  const char* task = (raw && strcmp(raw, "loopTask") != 0) ? raw : "MAIN";
  size_t pos = 0;
  if (printTimestamp) {
    pos += snprintf(buf + pos, len - pos, "[%s] ", iso);
  }
  if (printTaskName) {
    pos += snprintf(buf + pos, len - pos, "[%s] ", task);
  }
  buf[pos] = '\0';
}
// Get uptime since system start in format "dd, hh:mm:ss (seconds)"
inline void utilGetUptimeFormatted(char* buf, size_t len) {
  // FreeRTOS tick count to milliseconds
  uint64_t ticks = xTaskGetTickCount();
  uint64_t ms = ticks * portTICK_PERIOD_MS;
  uint64_t seconds = ms / 1000;
  uint32_t days = seconds / 86400;
  uint32_t rem = seconds % 86400;
  uint32_t hours = rem / 3600;
  rem %= 3600;
  uint32_t minutes = rem / 60;
  uint32_t secs = rem % 60;
  snprintf(buf, len, "%lu, %02lu:%02lu:%02lu (%llu)", days, hours, minutes, secs, seconds);
}

// Safe panic: blink LED0 indefinitely to signal unrecoverable error
inline void utilSafePanic() {
  for (;;) {
    Leds.blink(0, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}