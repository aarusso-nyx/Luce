#pragma once

#include <cstdint>

#include "esp_log.h"

namespace luce {
namespace runtime {

inline constexpr std::uint32_t clamp_u32(std::uint32_t value, std::uint32_t min_val, std::uint32_t max_val) {
  if (value < min_val) {
    return min_val;
  }
  if (value > max_val) {
    return max_val;
  }
  return value;
}

template <typename StateT>
inline void set_state(StateT& state, StateT next, const char* (*state_name)(StateT), const char* lifecycle_tag,
                      const char* reason = nullptr) {
  if (state == next) {
    return;
  }
  state = next;
  if (reason && *reason) {
    ESP_LOGI(lifecycle_tag, "state=%s reason=%s", state_name(state), reason);
  } else {
    ESP_LOGI(lifecycle_tag, "state=%s", state_name(state));
  }
}

}  // namespace runtime
}  // namespace luce
