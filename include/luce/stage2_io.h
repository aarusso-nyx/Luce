#pragma once

#include <cstdint>

#include "luce_build.h"

#if LUCE_HAS_I2C

#include <cstddef>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

enum class InitPathStatus : uint8_t { kUnknown = 0, kSuccess, kFailure };

struct InitPathResult {
  bool ok;
  esp_err_t error;
  InitPathStatus status;
};

struct I2cScanResult {
  bool mcp = false;
  bool lcd = false;
  int found_count = 0;
};

struct Mcp23017State {
  bool connected = false;
  uint8_t relay_mask = 0xFF;
};

extern bool g_i2c_initialized;
extern bool g_mcp_available;
extern uint8_t g_relay_mask;
extern uint8_t g_button_mask;

#if LUCE_HAS_LCD
extern bool g_lcd_present;
#endif

const char* init_status_name(InitPathStatus status);
InitPathResult init_result_success();
InitPathResult init_result_failure(esp_err_t error);

I2cScanResult scan_i2c_bus();
InitPathResult run_i2c_scan_flow(I2cScanResult& scan, const char* context, bool attach_lcd);

esp_err_t i2c_probe_device(uint8_t address, TickType_t timeout_ticks = pdMS_TO_TICKS(20));
esp_err_t mcp_read_reg(uint8_t reg, uint8_t* value);
uint8_t relay_mask_for_channel(int channel);
esp_err_t set_relay_mask_safe(uint8_t mask);
uint8_t relay_mask_for_channel_state(int channel, bool on, uint8_t current_mask);
bool read_button_inputs(uint8_t* value);
void configure_int_pin();
void run_stage2_diagnostics();

bool stage2_lcd_write_text(const char* text);

#endif  // LUCE_HAS_I2C
