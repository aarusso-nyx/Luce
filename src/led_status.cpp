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
constexpr TickType_t kNormalBlinkOnTicks = pdMS_TO_TICKS(300);
constexpr TickType_t kNormalBlinkOffTicks = pdMS_TO_TICKS(300);
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
std::uint8_t g_status_mask = 0;
LedManualMode g_manual_mode[3] = {LedManualMode::kAuto, LedManualMode::kAuto, LedManualMode::kAuto};
bool g_manual_phase_on[3] = {true, true, true};
TickType_t g_manual_deadline[3] = {0, 0, 0};

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

bool run_manual_mode_for_index(std::uint8_t idx, TickType_t now, LedManualMode mode) {
  if (idx > 2u) {
    return false;
  }
  switch (mode) {
    case LedManualMode::kAuto:
      return false;
    case LedManualMode::kOff:
      return false;
    case LedManualMode::kOn:
      return true;
    case LedManualMode::kBlinkNormal:
    case LedManualMode::kBlinkFast:
    case LedManualMode::kBlinkSlow:
    case LedManualMode::kFlash: {
      TickType_t on_ticks = kNormalBlinkOnTicks;
      TickType_t off_ticks = kNormalBlinkOffTicks;
      if (mode == LedManualMode::kBlinkFast) {
        on_ticks = kFastBlinkOnTicks;
        off_ticks = kFastBlinkOffTicks;
      } else if (mode == LedManualMode::kBlinkSlow) {
        on_ticks = kSlowBlinkOnTicks;
        off_ticks = kSlowBlinkOffTicks;
      } else if (mode == LedManualMode::kFlash) {
        on_ticks = kConnectOnTicks;
        off_ticks = kConnectOffTicks;
      }

      if (g_manual_deadline[idx] == 0) {
        g_manual_phase_on[idx] = true;
        g_manual_deadline[idx] = now + on_ticks;
        return true;
      }
      if (now >= g_manual_deadline[idx]) {
        g_manual_phase_on[idx] = !g_manual_phase_on[idx];
        g_manual_deadline[idx] = now + (g_manual_phase_on[idx] ? on_ticks : off_ticks);
      }
      return g_manual_phase_on[idx];
    }
    default:
      return false;
  }
}

void apply_manual_overrides(TickType_t now, bool& led0, bool& led1, bool& led2) {
  portENTER_CRITICAL(&g_state_lock);
  for (std::uint8_t idx = 0; idx < 3; ++idx) {
    const LedManualMode mode = g_manual_mode[idx];
    if (mode == LedManualMode::kAuto) {
      g_manual_deadline[idx] = 0;
      continue;
    }
    const bool value = run_manual_mode_for_index(idx, now, mode);
    if (idx == 0u) {
      led0 = value;
    } else if (idx == 1u) {
      led1 = value;
    } else {
      led2 = value;
    }
  }
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
  DeviceLedMode next_mode = DeviceLedMode::kOn;
  if (snap.device_booting) {
    // Keep steady ON during early boot.
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

    apply_manual_overrides(loop_now, led0, led1, led2);

    const std::uint8_t next_mask = static_cast<std::uint8_t>((led0 ? 0x01u : 0x00u) | (led1 ? 0x02u : 0x00u) |
                                                            (led2 ? 0x04u : 0x00u));
    portENTER_CRITICAL(&g_state_lock);
    g_status_mask = next_mask;
    portEXIT_CRITICAL(&g_state_lock);

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

std::uint8_t led_status_current_mask() {
  portENTER_CRITICAL(&g_state_lock);
  const std::uint8_t mask = g_status_mask;
  portEXIT_CRITICAL(&g_state_lock);
  return mask;
}

bool led_status_set_manual(std::uint8_t index, bool on) {
  return led_status_set_manual_mode(index, on ? LedManualMode::kOn : LedManualMode::kOff);
}

bool led_status_set_manual_mode(std::uint8_t index, LedManualMode mode) {
  if (index > 2u) {
    return false;
  }
  portENTER_CRITICAL(&g_state_lock);
  g_manual_mode[index] = mode;
  g_manual_phase_on[index] = true;
  g_manual_deadline[index] = 0;
  portEXIT_CRITICAL(&g_state_lock);
  return true;
}

bool led_status_clear_manual(std::uint8_t index) {
  if (index > 2u) {
    return false;
  }
  portENTER_CRITICAL(&g_state_lock);
  g_manual_mode[index] = LedManualMode::kAuto;
  g_manual_phase_on[index] = true;
  g_manual_deadline[index] = 0;
  portEXIT_CRITICAL(&g_state_lock);
  return true;
}

void led_status_clear_manual_all() {
  portENTER_CRITICAL(&g_state_lock);
  for (std::uint8_t idx = 0; idx < 3; ++idx) {
    g_manual_mode[idx] = LedManualMode::kAuto;
    g_manual_phase_on[idx] = true;
    g_manual_deadline[idx] = 0;
  }
  portEXIT_CRITICAL(&g_state_lock);
}

std::uint8_t led_status_manual_enabled_mask() {
  std::uint8_t mask = 0;
  portENTER_CRITICAL(&g_state_lock);
  for (std::uint8_t idx = 0; idx < 3; ++idx) {
    if (g_manual_mode[idx] != LedManualMode::kAuto) {
      mask = static_cast<std::uint8_t>(mask | (1u << idx));
    }
  }
  portEXIT_CRITICAL(&g_state_lock);
  return mask;
}

std::uint8_t led_status_manual_value_mask() {
  std::uint8_t value_mask = 0;
  portENTER_CRITICAL(&g_state_lock);
  const std::uint8_t current_mask = g_status_mask;
  for (std::uint8_t idx = 0; idx < 3; ++idx) {
    if (g_manual_mode[idx] != LedManualMode::kAuto && ((current_mask >> idx) & 0x1u)) {
      value_mask = static_cast<std::uint8_t>(value_mask | (1u << idx));
    }
  }
  portEXIT_CRITICAL(&g_state_lock);
  return value_mask;
}

LedManualMode led_status_manual_mode(std::uint8_t index) {
  if (index > 2u) {
    return LedManualMode::kAuto;
  }
  portENTER_CRITICAL(&g_state_lock);
  const LedManualMode mode = g_manual_mode[index];
  portEXIT_CRITICAL(&g_state_lock);
  return mode;
}
