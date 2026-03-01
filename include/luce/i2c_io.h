#pragma once

#include <cstdint>

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
  char addresses[64] = {0};
};

struct I2cSensorSnapshot {
  float temperature_c = 0.0f;
  float humidity_percent = 0.0f;
  int light_raw = 0;
  int voltage_raw = 0;
  bool dht_ok = false;
};

struct Mcp23017State {
  bool connected = false;
  uint8_t relay_mask = 0xFF;
};

constexpr std::uint8_t kMcpRegGpioA = 0x12;
constexpr std::uint8_t kMcpRegGpioB = 0x13;

extern bool g_i2c_initialized;
extern bool g_mcp_available;
extern uint8_t g_relay_mask;
extern uint8_t g_button_mask;
extern bool g_lcd_present;

const char* init_status_name(InitPathStatus status);
InitPathResult init_result_success();
InitPathResult init_result_failure(esp_err_t error);

I2cScanResult scan_i2c_bus();
InitPathResult run_i2c_scan_flow(I2cScanResult& scan, const char* context, bool attach_lcd);

esp_err_t i2c_probe_device(uint8_t address, TickType_t timeout_ticks = pdMS_TO_TICKS(20));
esp_err_t mcp_read_reg(uint8_t reg, uint8_t* value);
esp_err_t set_relay_mask_safe(uint8_t mask);
uint8_t relay_mask_for_channel_state(int channel, bool on, uint8_t current_mask);
bool read_button_inputs(uint8_t* value);
void configure_int_pin();
void io_startup();
bool read_sensor_snapshot(I2cSensorSnapshot& snapshot);
void io_set_relay_night_mask(std::uint8_t night_mask);
void io_set_light_threshold(std::uint16_t threshold);
void io_apply_relay_policy();
std::uint8_t io_relay_night_mask();
std::uint16_t io_light_threshold();
void io_lcd_show_logs_page();
void io_lcd_log_page_next();
void io_lcd_log_page_prev();
void io_lcd_log_page_reset();

bool i2c_lcd_write_text(const char* text);
