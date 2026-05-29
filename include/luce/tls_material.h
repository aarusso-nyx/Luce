#pragma once

#include <cstddef>
#include <cstring>

#include "luce/nvs_helpers.h"

namespace luce {
namespace tls {

inline esp_err_t load_ca_pem_from_nvs(const char* ns, const char* key, const char* source,
                                      char* out, std::size_t out_size) {
  if (out == nullptr || out_size == 0 || ns == nullptr || key == nullptr || source == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  out[0] = '\0';

  if (std::strcmp(source, "nvs") != 0) {
    return ESP_ERR_INVALID_STATE;
  }

  auto nvs = luce::nvs::Handle::Open(ns, NVS_READONLY);
  if (!nvs.ok()) {
    return nvs.error();
  }

  const auto status = luce::nvs::read_string_status(nvs.raw(), key, out, out_size, "");
  if (status == luce::nvs::StringReadStatus::kOk && out[0] != '\0') {
    return ESP_OK;
  }
  if (status == luce::nvs::StringReadStatus::kMissing ||
      (status == luce::nvs::StringReadStatus::kOk && out[0] == '\0')) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  return ESP_ERR_INVALID_STATE;
}

} // namespace tls
} // namespace luce
