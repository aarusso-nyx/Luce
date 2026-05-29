#pragma once

#include <cstdint>

namespace luce {
namespace relay {

constexpr std::uint8_t kAllRelaysMask = 0xFFu;

inline std::uint8_t state_to_output_mask(std::uint8_t state_mask, bool active_high = false) {
  const std::uint8_t masked = static_cast<std::uint8_t>(state_mask & kAllRelaysMask);
  return active_high ? masked : static_cast<std::uint8_t>(masked ^ kAllRelaysMask);
}

inline std::uint8_t output_to_state_mask(std::uint8_t output_mask, bool active_high = false) {
  const std::uint8_t masked = static_cast<std::uint8_t>(output_mask & kAllRelaysMask);
  return active_high ? masked : static_cast<std::uint8_t>(masked ^ kAllRelaysMask);
}

inline std::uint8_t apply_night_policy(std::uint8_t state_mask, std::uint8_t night_mask,
                                       bool is_day) {
  if (!is_day) {
    return state_mask;
  }
  return static_cast<std::uint8_t>(state_mask & static_cast<std::uint8_t>(~night_mask));
}

} // namespace relay
} // namespace luce
