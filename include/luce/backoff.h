#pragma once

#include <cstdint>

namespace luce {
namespace backoff {

inline std::uint32_t bounded_jitter(std::uint32_t span, std::uint32_t seed) {
  if (span == 0) {
    return 0;
  }
  std::uint32_t value = seed;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  return value % (span + 1u);
}

inline std::uint32_t next_backoff_ms(std::uint32_t attempt, std::uint32_t min_ms,
                                     std::uint32_t max_ms, std::uint32_t jitter_seed) {
  if (max_ms == 0 || min_ms == 0) {
    return 0;
  }
  if (min_ms > max_ms) {
    min_ms = max_ms;
  }

  std::uint32_t base = min_ms;
  for (std::uint32_t i = 0; i < attempt && base < max_ms; ++i) {
    if (base > max_ms / 2u) {
      base = max_ms;
    } else {
      base *= 2u;
    }
  }

  const std::uint32_t jitter_span = base / 4u;
  const std::uint32_t jitter = bounded_jitter(jitter_span, jitter_seed ^ attempt);
  if (base > max_ms - jitter) {
    return max_ms;
  }
  return base + jitter;
}

} // namespace backoff
} // namespace luce
