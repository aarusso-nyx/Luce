#include "luce/led_status.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "luce/cli_tcp.h"
#include "luce/http_server.h"
#include "luce/mdns.h"
#include "luce/mqtt.h"
#include "luce/net_wifi.h"
#include "luce/ntp.h"
#include "luce/task_budgets.h"

namespace {

constexpr const char* kTag = "[LED]";
constexpr gpio_num_t kLedDeviceStatus = GPIO_NUM_25;
constexpr gpio_num_t kLedNetworkStatus = GPIO_NUM_26;
constexpr gpio_num_t kLedOperationStatus = GPIO_NUM_27;

constexpr TickType_t kLoopPeriodTicks = pdMS_TO_TICKS(25);
constexpr TickType_t kFastBlinkOnTicks = pdMS_TO_TICKS(120);
constexpr TickType_t kFastBlinkOffTicks = pdMS_TO_TICKS(120);
constexpr TickType_t kSlowBlinkOnTicks = pdMS_TO_TICKS(220);
constexpr TickType_t kSlowBlinkOffTicks = pdMS_TO_TICKS(720);
constexpr TickType_t kConnectOnTicks = pdMS_TO_TICKS(180);
constexpr TickType_t kConnectOffTicks = pdMS_TO_TICKS(180);
constexpr TickType_t kUserPulseOnTicks = pdMS_TO_TICKS(100);
constexpr TickType_t kUserPulseOffTicks = pdMS_TO_TICKS(250);
constexpr TickType_t kNetSequenceGapTicks = pdMS_TO_TICKS(320);

constexpr std::uint8_t kMaxQueuedUserPulses = 16;
constexpr std::uint8_t kUserInputPulseCount = 1;
constexpr std::uint8_t kUserErrorPulseCount = 2;

enum class DeviceLedMode : std::uint8_t {
  kOff = 0,
  kOn,
  kFastBlink,
  kSlowBlink,
  kStartup,
};

struct BlinkState {
  bool active = false;
  bool on = false;
  bool phase_on = false;
  uint8_t blinks_left = 0;
  TickType_t phase_deadline = 0;
  TickType_t on_ticks = kFastBlinkOnTicks;
  TickType_t off_ticks = kFastBlinkOffTicks;
};

struct NetworkFailureState {
  bool active = false;
  bool gap_phase = false;
  uint8_t service_codes[5] = {0, 0, 0, 0, 0};
  std::size_t service_count = 0;
  std::size_t service_index = 0;
  uint8_t blinks_left = 0;
  bool on_phase = true;
  TickType_t phase_deadline = 0;
};

struct LedStatusSnapshot {
  bool device_booting = true;
  bool i2c_present = false;
  bool mcp_present = false;
  bool lcd_present = false;
  bool alert_active = false;
  std::uint8_t user_input_events = 0;
  std::uint8_t user_error_events = 0;
};

portMUX_TYPE g_state_lock = portMUX_INITIALIZER_UNLOCKED;

bool g_i2c_present = false;
bool g_mcp_present = false;
bool g_lcd_present = false;
bool g_device_booting = true;
bool g_alert_active = false;
std::uint8_t g_user_input_events = 0;
std::uint8_t g_user_error_events = 0;

DeviceLedMode g_device_mode = DeviceLedMode::kStartup;
BlinkState g_device_blink;

BlinkState g_operation_blink;
BlinkState g_network_connect_blink;

NetworkFailureState g_net_fail_state;

void snapshot_state(LedStatusSnapshot& out) {
  portENTER_CRITICAL(&g_state_lock);
  out.device_booting = g_device_booting;
  out.i2c_present = g_i2c_present;
  out.mcp_present = g_mcp_present;
  out.lcd_present = g_lcd_present;
  out.alert_active = g_alert_active;
  out.user_input_events = g_user_input_events;
  out.user_error_events = g_user_error_events;
  g_user_input_events = 0;
  g_user_error_events = 0;
  portEXIT_CRITICAL(&g_state_lock);
}

void set_blink_state(BlinkState& state, bool active, bool on, TickType_t now, TickType_t on_ticks,
                     TickType_t off_ticks) {
  state.active = active;
  state.on = on;
  state.phase_on = on;
  state.on_ticks = on_ticks;
  state.off_ticks = off_ticks;
  state.blinks_left = 0;
  state.phase_deadline = now + (on ? on_ticks : off_ticks);
}

bool run_blink_state(BlinkState& state, TickType_t now) {
  if (!state.active) {
    return state.on;
  }
  if (state.blinks_left == 0) {
    state.active = false;
    state.on = false;
    return false;
  }
  if (now < state.phase_deadline) {
    return state.on;
  }

  if (state.phase_on) {
    state.phase_on = false;
    state.on = false;
    state.phase_deadline = now + state.off_ticks;
  } else {
    if (state.blinks_left != 0xFF) {
      --state.blinks_left;
    }
    state.phase_on = true;
    state.on = true;
    state.phase_deadline = now + state.on_ticks;
  }
  return state.on;
}

void start_infinite_blink(BlinkState& state, bool initial_on, TickType_t now, TickType_t on_ticks, TickType_t off_ticks) {
  state.active = true;
  state.phase_on = true;
  state.on = initial_on;
  state.on_ticks = on_ticks;
  state.off_ticks = off_ticks;
  state.blinks_left = 0xFF;
  state.phase_deadline = now + (initial_on ? on_ticks : off_ticks);
}

void start_pulse_blink(BlinkState& state, std::uint8_t blinks, TickType_t now, TickType_t on_ticks,
                      TickType_t off_ticks) {
  if (blinks == 0) {
    return;
  }
  if (state.active && state.blinks_left != 0xFF) {
    const std::uint16_t sum = static_cast<std::uint16_t>(state.blinks_left) + blinks;
    state.blinks_left = (sum > 20) ? 20 : static_cast<std::uint8_t>(sum);
    return;
  }
  if (!state.active) {
    state.active = true;
    state.phase_on = true;
    state.on = true;
    state.blinks_left = blinks;
    state.on_ticks = on_ticks;
    state.off_ticks = off_ticks;
    state.phase_deadline = now + on_ticks;
    return;
  }
  // state.active but in infinite mode (defensive fallback)
  state.active = true;
  state.phase_on = true;
  state.on = true;
  state.blinks_left = blinks;
  state.phase_deadline = now + on_ticks;
}

void request_user_feedback(std::uint8_t input_events, std::uint8_t error_events) {
  const TickType_t now = xTaskGetTickCount();
  if (error_events > 0) {
    start_pulse_blink(g_operation_blink, kUserErrorPulseCount * ((error_events > 3) ? 3 : error_events),
                      now, kUserPulseOnTicks, kUserPulseOffTicks);
  } else if (input_events > 0 && !g_operation_blink.active) {
    start_pulse_blink(g_operation_blink, kUserInputPulseCount, now, kUserPulseOnTicks, kUserPulseOffTicks);
  } else if (input_events > 0) {
    const std::uint16_t sum = static_cast<std::uint16_t>(g_operation_blink.blinks_left) +
                              static_cast<std::uint16_t>(input_events * kUserInputPulseCount);
    g_operation_blink.blinks_left = static_cast<std::uint8_t>((sum > 20) ? 20 : sum);
  }
}

void update_device_led(const LedStatusSnapshot& snap, TickType_t now, bool& led) {
  DeviceLedMode next_mode = DeviceLedMode::kOff;
  if (snap.device_booting) {
    next_mode = DeviceLedMode::kOn;
  } else if (!snap.i2c_present) {
    next_mode = DeviceLedMode::kOff;
  } else if (!snap.mcp_present) {
    next_mode = DeviceLedMode::kFastBlink;
  } else if (!snap.lcd_present) {
    next_mode = DeviceLedMode::kSlowBlink;
  } else {
    next_mode = DeviceLedMode::kOn;
  }

  if (next_mode != g_device_mode) {
    g_device_mode = next_mode;
    if (next_mode == DeviceLedMode::kOn || next_mode == DeviceLedMode::kStartup) {
      set_blink_state(g_device_blink, false, true, now, 0, 0);
    } else if (next_mode == DeviceLedMode::kOff) {
      set_blink_state(g_device_blink, false, false, now, 0, 0);
    } else if (next_mode == DeviceLedMode::kFastBlink) {
      start_infinite_blink(g_device_blink, true, now, kFastBlinkOnTicks, kFastBlinkOffTicks);
    } else if (next_mode == DeviceLedMode::kSlowBlink) {
      start_infinite_blink(g_device_blink, true, now, kSlowBlinkOnTicks, kSlowBlinkOffTicks);
    }
  }
  led = run_blink_state(g_device_blink, now);
}

void collect_network_failures(std::array<uint8_t, 5>& codes, std::size_t& count) {
  count = 0;
  if (ntp_is_enabled() && !ntp_is_synced()) {
    codes[count++] = 6;
  }
  if (mdns_is_enabled() && !mdns_is_running()) {
    codes[count++] = 5;
  }
  if (cli_net_is_enabled() && !cli_net_is_listening()) {
    codes[count++] = 4;
  }
  if (mqtt_is_enabled() && !mqtt_is_connected()) {
    codes[count++] = 3;
  }
  if (http_is_enabled() && !http_is_running()) {
    codes[count++] = 2;
  }
}

bool same_failures(const NetworkFailureState& previous, const std::array<uint8_t, 5>& next, std::size_t next_count) {
  if (previous.service_count != next_count) {
    return false;
  }
  for (std::size_t idx = 0; idx < next_count; ++idx) {
    if (previous.service_codes[idx] != next[idx]) {
      return false;
    }
  }
  return true;
}

void ensure_net_sequence(const std::array<uint8_t, 5>& failures, std::size_t count, TickType_t now) {
  if (count == 0) {
    g_net_fail_state.active = false;
    return;
  }
  if (same_failures(g_net_fail_state, failures, count)) {
    return;
  }
  std::memcpy(g_net_fail_state.service_codes, failures.data(), count * sizeof(uint8_t));
  g_net_fail_state.service_count = count;
  g_net_fail_state.service_index = 0;
  g_net_fail_state.blinks_left = failures[0];
  g_net_fail_state.on_phase = true;
  g_net_fail_state.gap_phase = false;
  g_net_fail_state.active = true;
  g_net_fail_state.phase_deadline = now + kFastBlinkOnTicks;
}

void run_network_pattern(TickType_t now, bool& led) {
  if (!wifi_is_enabled()) {
    led = false;
    g_net_fail_state.active = false;
    return;
  }

  if (!wifi_is_connected()) {
    if (!wifi_is_connecting()) {
      led = false;
      g_net_fail_state.active = false;
      return;
    }
    if (!g_network_connect_blink.active) {
      start_infinite_blink(g_network_connect_blink, true, now, kConnectOnTicks, kConnectOffTicks);
    }
    led = run_blink_state(g_network_connect_blink, now);
    g_net_fail_state.active = false;
    return;
  }
  g_network_connect_blink.active = false;
  g_network_connect_blink.on = false;

  std::array<uint8_t, 5> failures{};
  std::size_t failure_count = 0;
  collect_network_failures(failures, failure_count);
  if (failure_count == 0) {
    led = true;
    g_net_fail_state.active = false;
    return;
  }
  ensure_net_sequence(failures, failure_count, now);
  if (!g_net_fail_state.active) {
    led = true;
    return;
  }

  if (now >= g_net_fail_state.phase_deadline) {
    if (g_net_fail_state.gap_phase) {
      g_net_fail_state.gap_phase = false;
      g_net_fail_state.service_index = (g_net_fail_state.service_index + 1) % g_net_fail_state.service_count;
      g_net_fail_state.blinks_left = g_net_fail_state.service_codes[g_net_fail_state.service_index];
      g_net_fail_state.on_phase = true;
      g_net_fail_state.phase_deadline = now + kFastBlinkOnTicks;
    } else if (g_net_fail_state.on_phase) {
      g_net_fail_state.on_phase = false;
      g_net_fail_state.phase_deadline = now + kFastBlinkOffTicks;
    } else {
      if (g_net_fail_state.blinks_left > 1) {
        --g_net_fail_state.blinks_left;
        g_net_fail_state.on_phase = true;
        g_net_fail_state.phase_deadline = now + kFastBlinkOnTicks;
      } else {
        g_net_fail_state.gap_phase = true;
        g_net_fail_state.phase_deadline = now + kNetSequenceGapTicks;
      }
    }
  }
  led = g_net_fail_state.on_phase && !g_net_fail_state.gap_phase;
}

void led_status_task(void*) {
  led_status_set_device_ready(false, false, false);
  const TickType_t now = xTaskGetTickCount();
  set_blink_state(g_device_blink, false, false, now, 0, 0);
  set_blink_state(g_operation_blink, false, false, now, 0, 0);
  g_net_fail_state.active = false;
  g_net_fail_state.gap_phase = false;

  gpio_reset_pin(kLedDeviceStatus);
  gpio_reset_pin(kLedNetworkStatus);
  gpio_reset_pin(kLedOperationStatus);
  gpio_set_direction(kLedDeviceStatus, GPIO_MODE_OUTPUT);
  gpio_set_direction(kLedNetworkStatus, GPIO_MODE_OUTPUT);
  gpio_set_direction(kLedOperationStatus, GPIO_MODE_OUTPUT);

  g_device_mode = DeviceLedMode::kStartup;

  while (true) {
    const TickType_t loop_now = xTaskGetTickCount();
    LedStatusSnapshot snap {};
    snapshot_state(snap);
    request_user_feedback(snap.user_input_events, snap.user_error_events);

    bool led0 = false;
    bool led1 = false;
    bool led2 = false;

    update_device_led(snap, loop_now, led0);
    run_network_pattern(loop_now, led1);

    if (snap.alert_active) {
      led2 = true;
    } else {
      if (g_operation_blink.active) {
        led2 = run_blink_state(g_operation_blink, loop_now);
      }
    }

    gpio_set_level(kLedDeviceStatus, led0 ? 1 : 0);
    gpio_set_level(kLedNetworkStatus, led1 ? 1 : 0);
    gpio_set_level(kLedOperationStatus, led2 ? 1 : 0);
    vTaskDelay(kLoopPeriodTicks);
  }
}

}  // namespace

void led_status_startup() {
  const TickType_t now = xTaskGetTickCount();
  const gpio_num_t leds[] = {kLedDeviceStatus, kLedNetworkStatus, kLedOperationStatus};
  for (gpio_num_t pin : leds) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
  }
  if (!luce::start_task(led_status_task, luce::task_budget::kTaskLed)) {
    ESP_LOGW(kTag, "Failed to create LED status task");
  }
  set_blink_state(g_device_blink, false, true, now, 0, 0);
  set_blink_state(g_network_connect_blink, false, false, now, 0, 0);
  g_device_mode = DeviceLedMode::kStartup;
}

void led_status_terminate() {
  // Best-effort stop; task lifecycle is intentionally not forcibly deleted in firmware.
  gpio_set_level(kLedDeviceStatus, 0);
  gpio_set_level(kLedNetworkStatus, 0);
  gpio_set_level(kLedOperationStatus, 0);
}

void led_status_set_device_ready(bool i2c_present, bool mcp_present, bool lcd_present) {
  portENTER_CRITICAL(&g_state_lock);
  g_device_booting = false;
  g_i2c_present = i2c_present;
  g_mcp_present = mcp_present;
  g_lcd_present = lcd_present;
  portEXIT_CRITICAL(&g_state_lock);
}

void led_status_set_alert(bool active) {
  portENTER_CRITICAL(&g_state_lock);
  g_alert_active = active;
  portEXIT_CRITICAL(&g_state_lock);
}

void led_status_notify_user_input() {
  portENTER_CRITICAL(&g_state_lock);
  const std::uint16_t queued = g_user_input_events + 1;
  g_user_input_events = static_cast<std::uint8_t>((queued > kMaxQueuedUserPulses) ? kMaxQueuedUserPulses : queued);
  portEXIT_CRITICAL(&g_state_lock);
}

void led_status_notify_user_error() {
  portENTER_CRITICAL(&g_state_lock);
  const std::uint16_t queued = g_user_error_events + 1;
  g_user_error_events = static_cast<std::uint8_t>((queued > kMaxQueuedUserPulses) ? kMaxQueuedUserPulses : queued);
  portEXIT_CRITICAL(&g_state_lock);
}
