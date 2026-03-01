#pragma once

#include <cstdint>

enum class LedManualMode : std::uint8_t {
  kAuto = 0,
  kOff,
  kOn,
  kBlinkNormal,
  kBlinkFast,
  kBlinkSlow,
  kFlash,
};

void led_status_startup();
void led_status_terminate();

void led_status_set_device_ready(bool i2c_present, bool mcp_present, bool lcd_present);
void led_status_set_alert(bool active);

void led_status_notify_user_input();
void led_status_notify_user_error();

std::uint8_t led_status_current_mask();
bool led_status_set_manual(std::uint8_t index, bool on);
bool led_status_set_manual_mode(std::uint8_t index, LedManualMode mode);
bool led_status_clear_manual(std::uint8_t index);
void led_status_clear_manual_all();
std::uint8_t led_status_manual_enabled_mask();
std::uint8_t led_status_manual_value_mask();
LedManualMode led_status_manual_mode(std::uint8_t index);
