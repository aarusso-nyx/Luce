// leds.cpp: implements LedsClass with blocking blink/pulse on ESP32
#include <driver/ledc.h>
#include <esp_err.h>

#include "config.h"
#include "leds.h"

using namespace config;

// LED pin mapping
const uint8_t LedsClass::pins[LedsClass::NUM_LEDS] = {
  LED_PIN_0,
  LED_PIN_1,
  LED_PIN_2
};

void LedsClass::begin() {
  // Configure one PWM timer for all LEDs
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode       = LEDC_HIGH_SPEED_MODE;
  timer_conf.duty_resolution  = LEDC_TIMER_8_BIT;
  timer_conf.timer_num        = LEDC_TIMER_0;
  timer_conf.freq_hz          = 5000;
  timer_conf.clk_cfg          = LEDC_AUTO_CLK;
  esp_err_t err = ledc_timer_config(&timer_conf);
  if (err != ESP_OK) {
    // Timer config failed
    return;
  }
  // Configure one channel per LED
  for (uint8_t i = 0; i < NUM_LEDS; ++i) {
    ledc_channel_config_t ch_conf = {};
    ch_conf.speed_mode     = LEDC_HIGH_SPEED_MODE;
    ch_conf.channel        = static_cast<ledc_channel_t>(i);
    ch_conf.timer_sel      = LEDC_TIMER_0;
    ch_conf.intr_type      = LEDC_INTR_DISABLE;
    ch_conf.gpio_num       = pins[i];
    ch_conf.duty           = 0;
    ch_conf.hpoint         = 0;
    err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
      // Channel config failed
      continue;
    }
    // Ensure LED off
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, ch_conf.channel, 0);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, ch_conf.channel);
  }
  stateMask = 0;
}

void LedsClass::set(uint8_t idx, bool on) {
  if (idx >= NUM_LEDS) return;
  stateMask = on ? (stateMask | (1 << idx))
                 : (stateMask & ~(1 << idx));
  // Set PWM duty (255=full on, 0=off)
  ledc_set_duty(LEDC_HIGH_SPEED_MODE,
               static_cast<ledc_channel_t>(idx), on ? 0xFF : 0);
  ledc_update_duty(LEDC_HIGH_SPEED_MODE,
                  static_cast<ledc_channel_t>(idx));
}

bool LedsClass::get(uint8_t idx) const {
  if (idx >= NUM_LEDS) return false;
  return (stateMask >> idx) & 1;
}

uint8_t LedsClass::getMask() const {
  return stateMask;
}

void LedsClass::blink(uint8_t idx, uint8_t times) {
  if (idx >= NUM_LEDS) return;
  bool orig = get(idx);
  for (uint8_t t = 0; t < times; ++t) {
    // Full on
    ledc_set_duty(LEDC_HIGH_SPEED_MODE,
                 static_cast<ledc_channel_t>(idx), 0xFF);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE,
                    static_cast<ledc_channel_t>(idx));
    vTaskDelay(pdMS_TO_TICKS(100));
    // Off
    ledc_set_duty(LEDC_HIGH_SPEED_MODE,
                 static_cast<ledc_channel_t>(idx), 0);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE,
                    static_cast<ledc_channel_t>(idx));
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // Restore original state
  ledc_set_duty(LEDC_HIGH_SPEED_MODE,
               static_cast<ledc_channel_t>(idx), orig ? 0xFF : 0);
  ledc_update_duty(LEDC_HIGH_SPEED_MODE,
                  static_cast<ledc_channel_t>(idx));
}

void LedsClass::pulse(uint8_t idx) {
  if (idx >= NUM_LEDS) return;
  bool orig = get(idx);
  for (int v = 0; v <= 255; v += 5) {
    ledc_set_duty(LEDC_HIGH_SPEED_MODE,
                 static_cast<ledc_channel_t>(idx), v);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE,
                    static_cast<ledc_channel_t>(idx));
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  for (int v = 255; v >= 0; v -= 5) {
    ledc_set_duty(LEDC_HIGH_SPEED_MODE,
                 static_cast<ledc_channel_t>(idx), v);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE,
                    static_cast<ledc_channel_t>(idx));
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // Restore original state
  ledc_set_duty(LEDC_HIGH_SPEED_MODE,
               static_cast<ledc_channel_t>(idx), orig ? 0xFF : 0);
  ledc_update_duty(LEDC_HIGH_SPEED_MODE,
                  static_cast<ledc_channel_t>(idx));
}

// global instance
LedsClass Leds;