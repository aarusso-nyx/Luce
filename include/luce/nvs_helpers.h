#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "nvs.h"

namespace luce {
namespace nvs {

inline bool read_u8(nvs_handle_t handle, const char* key, std::uint8_t& out, std::uint8_t fallback) {
  std::uint8_t value = fallback;
  if (nvs_get_u8(handle, key, &value) == ESP_OK) {
    out = value;
    return true;
  }
  out = fallback;
  return false;
}

inline bool read_u16(nvs_handle_t handle, const char* key, std::uint16_t& out, std::uint16_t fallback) {
  std::uint16_t value = fallback;
  if (nvs_get_u16(handle, key, &value) == ESP_OK) {
    out = value;
    return true;
  }
  out = fallback;
  return false;
}

inline bool read_u32(nvs_handle_t handle, const char* key, std::uint32_t& out, std::uint32_t fallback) {
  std::uint32_t value = fallback;
  if (nvs_get_u32(handle, key, &value) == ESP_OK) {
    out = value;
    return true;
  }
  out = fallback;
  return false;
}

inline bool read_string(nvs_handle_t handle, const char* key, char* out, std::size_t out_size, const char* fallback) {
  if (!out || out_size == 0) {
    return false;
  }
  std::size_t needed = 0;
  if (nvs_get_str(handle, key, nullptr, &needed) == ESP_OK && needed > 0) {
    if (needed >= out_size) {
      needed = out_size - 1;
    }
    if (nvs_get_str(handle, key, out, &needed) == ESP_OK) {
      return true;
    }
  }
  std::snprintf(out, out_size, "%s", fallback ? fallback : "");
  return false;
}

inline void log_nvs_u8(const char* tag, const char* key, std::uint8_t value, bool found, std::uint8_t fallback) {
  if (found) {
    ESP_LOGI(tag, "key=%s value=%u", key, static_cast<unsigned>(value));
  } else {
    ESP_LOGW(tag, "key=%s missing; using default=%u", key, static_cast<unsigned>(fallback));
  }
}

inline void log_nvs_u32(const char* tag, const char* key, std::uint32_t value, bool found, std::uint32_t fallback) {
  if (found) {
    ESP_LOGI(tag, "key=%s value=%lu", key, static_cast<unsigned long>(value));
  } else {
    ESP_LOGW(tag, "key=%s missing; using default=%lu", key, static_cast<unsigned long>(fallback));
  }
}

inline void log_nvs_string(const char* tag, const char* key, const char* value, bool found, const char* fallback,
                           bool quote_value, bool mask_value = false) {
  const char* const shown = (mask_value ? "********" : (value != nullptr ? value : ""));
  const char* const default_value = mask_value ? "********" : (fallback != nullptr ? fallback : "");
  if (found) {
    if (quote_value) {
      ESP_LOGI(tag, "key=%s value='%s'", key, shown);
      return;
    }
    ESP_LOGI(tag, "key=%s value=%s", key, shown);
    return;
  }
  if (quote_value) {
    ESP_LOGW(tag, "key=%s missing; using default='%s'", key, default_value);
    return;
  }
  ESP_LOGW(tag, "key=%s missing; using default=%s", key, default_value);
}

}  // namespace nvs
}  // namespace luce
