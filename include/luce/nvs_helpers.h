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

// RAII wrapper around `nvs_handle_t`. Acquire with Handle::Open(), check
// with `ok()`, and let the destructor call nvs_close on every exit path.
//
// Existing call-sites that already pair nvs_open / nvs_close cleanly do
// not need to migrate; adopt this for sites that have multiple early-
// return paths (e.g. multi-step get/set/commit sequences) where the
// manual close is duplicated or easy to forget on a new path.
//
// Move-only. Cannot be copied (would double-close).
class Handle {
 public:
  static Handle Open(const char* ns, nvs_open_mode_t mode) {
    Handle h;
    h.err_ = nvs_open(ns, mode, &h.handle_);
    h.open_ = (h.err_ == ESP_OK);
    return h;
  }

  Handle() noexcept = default;
  ~Handle() {
    if (open_) {
      nvs_close(handle_);
      open_ = false;
    }
  }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  Handle(Handle&& other) noexcept
      : handle_(other.handle_), err_(other.err_), open_(other.open_) {
    other.handle_ = 0;
    other.open_ = false;
    other.err_ = ESP_ERR_INVALID_STATE;
  }

  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      if (open_) {
        nvs_close(handle_);
      }
      handle_ = other.handle_;
      err_ = other.err_;
      open_ = other.open_;
      other.handle_ = 0;
      other.open_ = false;
      other.err_ = ESP_ERR_INVALID_STATE;
    }
    return *this;
  }

  bool ok() const { return open_; }
  esp_err_t error() const { return err_; }
  nvs_handle_t raw() const { return handle_; }

  // Convenience read helpers — delegate to the free functions above so
  // calling code can mix-and-match (e.g. an existing helper that takes a
  // raw nvs_handle_t can still be called via h.raw()).
  bool read_u8(const char* key, std::uint8_t& out, std::uint8_t fallback) const {
    return luce::nvs::read_u8(handle_, key, out, fallback);
  }
  bool read_u16(const char* key, std::uint16_t& out, std::uint16_t fallback) const {
    return luce::nvs::read_u16(handle_, key, out, fallback);
  }
  bool read_u32(const char* key, std::uint32_t& out, std::uint32_t fallback) const {
    return luce::nvs::read_u32(handle_, key, out, fallback);
  }
  bool read_string(const char* key, char* out, std::size_t out_size,
                   const char* fallback) const {
    return luce::nvs::read_string(handle_, key, out, out_size, fallback);
  }

 private:
  nvs_handle_t handle_ = 0;
  esp_err_t err_ = ESP_ERR_INVALID_STATE;
  bool open_ = false;
};

}  // namespace nvs
}  // namespace luce
