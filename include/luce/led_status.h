#pragma once

#include <cstdint>

void led_status_startup();
void led_status_terminate();

void led_status_set_device_ready(bool i2c_present, bool mcp_present, bool lcd_present);
void led_status_set_alert(bool active);

void led_status_notify_user_input();
void led_status_notify_user_error();

std::uint8_t led_status_current_mask();
