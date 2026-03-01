#include "luce/dht_sensor.h"

#include <cstdint>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

namespace {

constexpr std::uint8_t kExpectedBits = 40;
constexpr std::uint8_t kStartLowUs = 20;
constexpr std::uint32_t kResponseTimeoutUs = 120;
constexpr std::uint32_t kMaxBitEdgeTimeoutUs = 200;
constexpr std::uint8_t kBitThresholdUs = 48;
constexpr std::uint32_t kRetryBackoffUs = 50000;

bool wait_for_level(gpio_num_t pin, int target_level, std::uint32_t timeout_us) {
  const uint64_t deadline_us = esp_timer_get_time() + timeout_us;
  while (gpio_get_level(pin) != target_level) {
    if (esp_timer_get_time() >= deadline_us) {
      return false;
    }
  }
  return true;
}

bool read_once(gpio_num_t data_pin, float& temperature_c, float& humidity_percent) {
  std::uint8_t raw[5] = {0, 0, 0, 0, 0};

  gpio_set_direction(data_pin, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode(data_pin, GPIO_PULLUP_ONLY);
  gpio_set_level(data_pin, 0);
  ets_delay_us(1000 * kStartLowUs);

  gpio_set_level(data_pin, 1);
  ets_delay_us(40);
  gpio_set_direction(data_pin, GPIO_MODE_INPUT);

  ets_intr_lock();
  if (!wait_for_level(data_pin, 0, kResponseTimeoutUs)) {
    ets_intr_unlock();
    return false;
  }
  if (!wait_for_level(data_pin, 1, kResponseTimeoutUs)) {
    ets_intr_unlock();
    return false;
  }
  if (!wait_for_level(data_pin, 0, kResponseTimeoutUs)) {
    ets_intr_unlock();
    return false;
  }

  for (std::uint8_t bit = 0; bit < kExpectedBits; ++bit) {
    if (!wait_for_level(data_pin, 1, kMaxBitEdgeTimeoutUs)) {
      ets_intr_unlock();
      return false;
    }
    const uint64_t high_start = esp_timer_get_time();
    if (!wait_for_level(data_pin, 0, kMaxBitEdgeTimeoutUs)) {
      ets_intr_unlock();
      return false;
    }
    const std::uint64_t high_width_us = esp_timer_get_time() - high_start;
    if (high_width_us > kBitThresholdUs) {
      raw[bit / 8] |= static_cast<std::uint8_t>(1u << (7 - (bit & 7)));
    }
  }
  ets_intr_unlock();

  const std::uint8_t checksum = static_cast<std::uint8_t>(raw[0] + raw[1] + raw[2] + raw[3]);
  if (checksum != raw[4]) {
    return false;
  }

  const std::uint16_t humidity_raw = static_cast<std::uint16_t>(raw[0] << 8u | raw[1]);
  std::uint16_t temp_raw = static_cast<std::uint16_t>(raw[2] << 8u | raw[3]);
  const bool temp_negative = (temp_raw & 0x8000u) != 0u;
  temp_raw &= 0x7FFFu;

  humidity_percent = static_cast<float>(humidity_raw) / 10.0f;
  temperature_c = static_cast<float>(temp_raw) / 10.0f;
  if (temp_negative) {
    temperature_c = -temperature_c;
  }
  return true;
}

}  // namespace

bool dht21_22_read_with_retries(gpio_num_t data_pin, float& temperature_c, float& humidity_percent,
                                int attempts) {
  if (attempts <= 0) {
    attempts = 1;
  }

  for (int i = 0; i < attempts; ++i) {
    if (read_once(data_pin, temperature_c, humidity_percent)) {
      return true;
    }
    ets_delay_us(kRetryBackoffUs);
  }
  return false;
}
