#include "luce/i2c_io.h"

#include <ctime>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "luce/dht_sensor.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "luce/boot_diagnostics.h"
#include "luce/http_server.h"
#include "luce/mdns.h"
#include "luce/mqtt.h"
#include "luce/net_wifi.h"
#include "luce/ntp.h"
#include "luce/led_status.h"
#include "luce/task_budgets.h"

constexpr const char* kTag = "luce_boot";

constexpr i2c_port_t kI2CPort = I2C_NUM_0;
constexpr gpio_num_t kI2CSclPin = GPIO_NUM_22;
constexpr gpio_num_t kI2CSdaPin = GPIO_NUM_23;
// ITB (GPIO19) is used for MCP interrupt signaling; ITA is intentionally unused on this hardware.
constexpr gpio_num_t kMcpIntPin = GPIO_NUM_19;
constexpr uint32_t kI2CClockHz = 100000;
constexpr uint8_t kMcpAddress = 0x20;

// MCP23017 register map for BANK=0
constexpr uint8_t kIocon = 0x0A;
constexpr uint8_t kIodira = 0x00;
constexpr uint8_t kIodirb = 0x01;
constexpr uint8_t kGppua = 0x0C;
constexpr uint8_t kGppub = 0x0D;
constexpr uint8_t kGpioa = 0x12;
constexpr uint8_t kGpiob = 0x13;

// Relay channels are active LOW on MCP GPIOA for this board revision:
// written bit=1 means OFF, bit=0 means ON.
constexpr bool kRelayActiveHigh = false;
constexpr uint8_t kRelayOffValue = kRelayActiveHigh ? 0x00 : 0xFF;
constexpr uint8_t kI2CSampleAddressStart = 0x08;
constexpr uint8_t kI2CSampleAddressEnd = 0x77;
constexpr uint8_t kButtonDebounceThreshold = 3;
constexpr TickType_t kButtonSamplePeriod = pdMS_TO_TICKS(40);
constexpr TickType_t kIntSamplePeriod = pdMS_TO_TICKS(200);
constexpr bool kButtonActiveLow = true;
constexpr gpio_num_t kDhtDataPin = GPIO_NUM_4;
constexpr adc_channel_t kLightSensorChannel = ADC_CHANNEL_6;
constexpr uint16_t kLightBitThreshold = 2048;
constexpr TickType_t kSensorReadPeriod = pdMS_TO_TICKS(10000);
constexpr TickType_t kLcdStatusUpdateTicks = pdMS_TO_TICKS(1000);

// LCD I2C backpack is fixed at 0x27 on the current hardware map and 3.3V bus-compatible.
constexpr uint8_t kLcdAddress = 0x27;

bool g_i2c_initialized = false;
bool g_mcp_available = false;
uint8_t g_relay_mask = kRelayOffValue;
uint8_t g_button_mask = 0x00;
bool g_light_sensor_ready = false;
adc_oneshot_unit_handle_t g_light_sensor_handle = nullptr;
float g_last_temperature_c = 0.0f;
float g_last_humidity_percent = 0.0f;
int g_last_light_raw = 0;
bool g_last_dht_read_ok = false;
bool g_lcd_present = false;
TaskHandle_t g_i2c_task = nullptr;


const char* init_status_name(InitPathStatus status) {
  switch (status) {
    case InitPathStatus::kSuccess:
      return "success";
    case InitPathStatus::kFailure:
      return "failure";
    case InitPathStatus::kUnknown:
    default:
      return "unknown";
  }
}

InitPathResult init_result_success() {
  return InitPathResult{true, ESP_OK, InitPathStatus::kSuccess};
}

InitPathResult init_result_failure(esp_err_t error) {
  return InitPathResult{false, error, InitPathStatus::kFailure};
}

void init_light_sensor() {
  if (g_light_sensor_ready) {
    return;
  }

  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = ADC_UNIT_1;
  unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
  if (adc_oneshot_new_unit(&unit_cfg, &g_light_sensor_handle) != ESP_OK) {
    ESP_LOGW(kTag, "Light sensor ADC unit init failed");
    return;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  chan_cfg.atten = ADC_ATTEN_DB_12;
  if (adc_oneshot_config_channel(g_light_sensor_handle, kLightSensorChannel, &chan_cfg) == ESP_OK) {
    g_light_sensor_ready = true;
  } else {
    ESP_LOGW(kTag, "Light sensor ADC channel config failed");
    adc_oneshot_del_unit(g_light_sensor_handle);
    g_light_sensor_handle = nullptr;
  }
}

void log_sensor_readings() {
  init_light_sensor();
  int light_raw = 0;
  if (g_light_sensor_ready) {
    int converted = 0;
    const esp_err_t raw_rc = adc_oneshot_read(g_light_sensor_handle, kLightSensorChannel, &converted);
    if (raw_rc == ESP_OK) {
      light_raw = converted;
    }
  }
  float humidity = 0.0f;
  float temperature = 0.0f;
  g_last_light_raw = light_raw;
  if (dht21_22_read_with_retries(kDhtDataPin, temperature, humidity)) {
    g_last_dht_read_ok = true;
    g_last_temperature_c = temperature;
    g_last_humidity_percent = humidity;
    ESP_LOGI(kTag, "DHT21/22 temp=%.1fC hum=%.1f%% light=%d", temperature, humidity, light_raw);
  } else {
    g_last_dht_read_ok = false;
    ESP_LOGW(kTag, "DHT21/22 read failed; light=%d", light_raw);
  }
}

bool read_sensor_snapshot(I2cSensorSnapshot& snapshot) {
  init_light_sensor();
  int light_raw = 0;
  if (g_light_sensor_ready) {
    int converted = 0;
    if (adc_oneshot_read(g_light_sensor_handle, kLightSensorChannel, &converted) == ESP_OK) {
      light_raw = converted;
    }
  }

  float humidity = 0.0f;
  float temperature = 0.0f;
  const bool dht_ok = dht21_22_read_with_retries(kDhtDataPin, temperature, humidity);
  if (dht_ok) {
    g_last_dht_read_ok = true;
    g_last_temperature_c = temperature;
    g_last_humidity_percent = humidity;
  } else {
    g_last_dht_read_ok = false;
  }
  g_last_light_raw = light_raw;

  snapshot.temperature_c = g_last_temperature_c;
  snapshot.humidity_percent = g_last_humidity_percent;
  snapshot.light_raw = g_last_light_raw;
  snapshot.dht_ok = g_last_dht_read_ok;
  return dht_ok;
}

constexpr uint8_t kLcdPcfRsBit = 0;
constexpr uint8_t kLcdPcfRwBit = 1;
constexpr uint8_t kLcdPcfEnBit = 2;
constexpr uint8_t kLcdPcfBacklightBit = 3;
constexpr uint8_t kLcdCols = 20;
constexpr uint8_t kLcdRows = 4;

class Pcf8574Hd44780 {
 public:
  Pcf8574Hd44780(i2c_port_t port, uint8_t address) : port_(port), address_(address) {}

  bool begin() {
    vTaskDelay(pdMS_TO_TICKS(50));

    if (!write_nibble(0x03, false) || !write_nibble(0x03, false) || !write_nibble(0x03, false) ||
        !write_nibble(0x02, false)) {
      return false;
    }
    if (!send_command(0x28) || !send_command(0x08) || !send_command(0x0C) ||
        !send_command(0x06) || !send_command(0x01)) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3));
    return true;
  }

  void set_mcp_ok(bool ok) { mcp_ok_ = ok; }

  bool write_status_lines(uint8_t relay_mask, uint8_t button_mask) {
    char line1[21] = {0};
    char line2[21] = {0};
    char line3[21] = {0};
    char line4[21] = {0};

    const uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
    std::snprintf(line1, sizeof(line1), "LUCE S3 %lus", (unsigned long)uptime_s);
    std::snprintf(line2, sizeof(line2), "I2C:%s MCP:%s", "ok", mcp_ok_ ? "ok" : "no");
    std::snprintf(line3, sizeof(line3), "REL:0x%02X", relay_mask);
    std::snprintf(line4, sizeof(line4), "BTN:0x%02X", button_mask);

    return write_line(0, line1) && write_line(1, line2) && write_line(2, line3) &&
           write_line(3, line4);
  }

  bool write_text_line(uint8_t row, const char* text) {
    char safe_text[21] = {0};
    std::size_t text_len = 0;
    if (text) {
      text_len = std::strlen(text);
      if (text_len > kLcdCols) {
        text_len = kLcdCols;
      }
    }
    for (std::size_t idx = 0; idx < kLcdCols; ++idx) {
      safe_text[idx] = (idx < text_len) ? text[idx] : ' ';
    }
    return write_line(row, safe_text);
  }

  bool write_text(uint8_t row, const char* text) { return write_text_line(row, text); }

 private:
  bool send_command(uint8_t cmd) { return send_byte(cmd, false); }

  bool send_byte(uint8_t value, bool is_data) {
    const uint8_t hi = (value >> 4) & 0x0F;
    const uint8_t lo = value & 0x0F;
    return write_nibble(hi, is_data) && write_nibble(lo, is_data);
  }

  bool write_nibble(uint8_t nibble, bool is_data) {
    const uint8_t half_byte = static_cast<uint8_t>((nibble & 0x0F) << 4);
    uint8_t frame = half_byte;
    frame &= static_cast<uint8_t>(~(1u << kLcdPcfRwBit));
    if (is_data) {
      frame |= static_cast<uint8_t>(1u << kLcdPcfRsBit);
    }
    frame |= static_cast<uint8_t>(1u << kLcdPcfBacklightBit);
    return pulse_en(frame);
  }

  bool pulse_en(uint8_t frame) {
    if (!write_pcf(frame | (1u << kLcdPcfEnBit))) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    if (!write_pcf(frame & ~(1u << kLcdPcfEnBit))) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    return true;
  }

  bool write_pcf(uint8_t value) {
    return i2c_master_write_to_device(port_, address_, &value, sizeof(value), pdMS_TO_TICKS(100)) ==
           ESP_OK;
  }

  bool set_cursor(uint8_t row, uint8_t col) {
    static constexpr uint8_t kRowAddress[] = {0x00, 0x40, 0x14, 0x54};
    if (row >= kLcdRows || col >= kLcdCols) {
      return false;
    }
    return send_command(static_cast<uint8_t>(0x80 | (kRowAddress[row] + col)));
  }

  bool write_line(uint8_t row, const char* text) {
    char padded[kLcdCols + 1] = {0};
    for (std::size_t idx = 0; idx < kLcdCols; ++idx) {
      padded[idx] = ' ';
    }
    if (text) {
      for (std::size_t idx = 0; idx < kLcdCols && text[idx] != '\0'; ++idx) {
        padded[idx] = text[idx];
      }
    }

    if (!set_cursor(row, 0)) {
      return false;
    }

    for (std::size_t idx = 0; idx < kLcdCols; ++idx) {
      if (!send_byte(static_cast<uint8_t>(padded[idx]), true)) {
        return false;
      }
    }
    return true;
  }

  i2c_port_t port_;
  uint8_t address_;
  bool mcp_ok_ = false;
};

Pcf8574Hd44780 g_lcd(kI2CPort, kLcdAddress);

void update_lcd_status() {
  if (!g_lcd_present) {
    return;
  }

  char ssid[33] = {0};
  char utc_line[20 + 1] = {0};
  char line1[21] = {0};
  char line2[21] = {0};
  char line3[21] = {0};
  char line4[21] = {0};

  wifi_get_ssid(ssid, sizeof(ssid));
  const char* wifi_state = wifi_is_enabled() ? (wifi_is_ip_ready() ? "ok" : "conn") : "fail";
  std::snprintf(line1, sizeof(line1), "SSID: %s", wifi_state);
  std::snprintf(line2, sizeof(line2), "0x%02X | 0x%02X", g_button_mask, g_relay_mask);

  if (g_last_dht_read_ok) {
    const uint8_t light_bit = g_last_light_raw >= static_cast<int>(kLightBitThreshold) ? 1u : 0u;
    std::snprintf(line3, sizeof(line3), "DHT %.1f/%.1f L:%u", g_last_temperature_c, g_last_humidity_percent,
                  light_bit);
  } else {
    std::snprintf(line3, sizeof(line3), "DHT -/- L:%u", (g_last_light_raw >= static_cast<int>(kLightBitThreshold)) ? 1u : 0u);
  }

  const std::time_t utc_now = std::time(nullptr);
  tm gmt{};
  const tm* gmt_ptr = std::gmtime(&utc_now);
  if (gmt_ptr != nullptr) {
    gmt = *gmt_ptr;
    std::strftime(utc_line, sizeof(utc_line), "%Y-%m-%dT%H:%M:%SZ", &gmt);
  } else {
    std::snprintf(utc_line, sizeof(utc_line), "n/a");
  }
  std::snprintf(line4, sizeof(line4), "%s", utc_line);

  if (!g_lcd.write_text_line(0, line1) || !g_lcd.write_text_line(1, line2) || !g_lcd.write_text_line(2, line3) ||
      !g_lcd.write_text_line(3, line4)) {
    ESP_LOGW(kTag, "LCD status update failed");
  }
}

void update_lcd_status_if_present() {
  if (g_lcd_present) {
    update_lcd_status();
  }
}

esp_err_t i2c_probe_device(uint8_t address, TickType_t timeout_ticks) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (!cmd) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = i2c_master_start(cmd);
  if (err == ESP_OK) {
    err = i2c_master_write_byte(cmd, static_cast<uint8_t>((address << 1) | I2C_MASTER_WRITE), true);
  }
  if (err == ESP_OK) {
    err = i2c_master_stop(cmd);
  }
  if (err == ESP_OK) {
    err = i2c_master_cmd_begin(kI2CPort, cmd, timeout_ticks);
  }

  i2c_cmd_link_delete(cmd);
  return err;
}

bool init_i2c() {
  i2c_config_t config;
  std::memset(&config, 0, sizeof(config));
  config.mode = I2C_MODE_MASTER;
  config.sda_io_num = kI2CSdaPin;
  config.scl_io_num = kI2CSclPin;
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.master.clk_speed = kI2CClockHz;

  esp_err_t err = i2c_param_config(kI2CPort, &config);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "I2C config failed: %s", esp_err_to_name(err));
    return false;
  }

  err = i2c_driver_install(kI2CPort, I2C_MODE_MASTER, 0, 0, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "I2C install failed: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(kTag, "I2C init: port=%d SCL=%d SDA=%d", kI2CPort, kI2CSclPin, kI2CSdaPin);
  return true;
}

InitPathResult init_i2c_contract() {
  const bool ok = init_i2c();
  return ok ? init_result_success() : init_result_failure(ESP_FAIL);
}

InitPathResult run_i2c_scan_flow(I2cScanResult& scan, const char* context, bool attach_lcd) {
  scan = I2cScanResult{};
  InitPathResult contract = init_i2c_contract();
  if (!contract.ok) {
    if (context) {
      ESP_LOGW(kTag, "%s: init_i2c() failed (%s)", context, init_status_name(contract.status));
    }
    return contract;
  }
  g_i2c_initialized = true;

  scan = scan_i2c_bus();
  if (context) {
    ESP_LOGI(kTag, "%s: summary found=%d mcp=%d lcd=%d", context, scan.found_count,
             scan.mcp ? 1 : 0, scan.lcd ? 1 : 0);
  }
  if (!attach_lcd) {
    g_lcd_present = false;
    return init_result_success();
  }

  if (scan.lcd) {
    const bool lcd_started = g_lcd.begin();
    if (!lcd_started) {
      ESP_LOGW(kTag, "LCD detected at 0x%02X but initialization failed; continuing without LCD",
               kLcdAddress);
    } else {
      g_lcd.set_mcp_ok(false);
      g_lcd_present = true;
      ESP_LOGI(kTag, "LCD initialized at 0x%02X", kLcdAddress);
      update_lcd_status();
    }
  }
  return init_result_success();
}

I2cScanResult scan_i2c_bus() {
  I2cScanResult result;
  char list_buf[sizeof(result.addresses)] = {0};
  std::size_t next_addr_offset = 0;

  for (uint8_t addr = kI2CSampleAddressStart; addr <= kI2CSampleAddressEnd; ++addr) {
    if (i2c_probe_device(addr) == ESP_OK) {
      result.found_count += 1;
      result.mcp = result.mcp || (addr == kMcpAddress);
      result.lcd = result.lcd || (addr == kLcdAddress);
      if (next_addr_offset < sizeof(list_buf)) {
        if (next_addr_offset != 0) {
          const std::size_t space_left = sizeof(list_buf) - next_addr_offset - 1;
          if (space_left > 0) {
            list_buf[next_addr_offset++] = ',';
            list_buf[next_addr_offset] = '\0';
          }
        }
        char token[6] = {0};
        std::snprintf(token, sizeof(token), "0x%02X", addr);
        const std::size_t token_len = std::strlen(token);
        const std::size_t space_left = sizeof(list_buf) - next_addr_offset - 1;
        const std::size_t copy_len = (token_len < space_left) ? token_len : (space_left > 0 ? space_left : 0);
        if (copy_len > 0) {
          std::memcpy(&list_buf[next_addr_offset], token, copy_len);
          next_addr_offset += copy_len;
          list_buf[next_addr_offset] = '\0';
        }
      }
      ESP_LOGI(kTag, "I2C found 0x%02X", addr);
    }
  }

  if (next_addr_offset == 0) {
    std::snprintf(result.addresses, sizeof(result.addresses), "none");
  } else {
    std::snprintf(result.addresses, sizeof(result.addresses), "%s", list_buf);
  }

  if (result.found_count == 0) {
    ESP_LOGW(kTag, "I2C scan: no devices detected on bus");
  } else {
    ESP_LOGI(kTag, "I2C scan summary: %d device(s) detected", result.found_count);
  }

  if (result.mcp) {
    ESP_LOGI(kTag, "I2C scan expects MCP23017 at 0x20: found");
  } else {
    ESP_LOGW(kTag, "I2C scan expects MCP23017 at 0x20: not found");
  }

  if (result.lcd) {
    ESP_LOGI(kTag, "I2C scan expects LCD at 0x%02X: found", kLcdAddress);
  } else {
    ESP_LOGW(kTag, "I2C scan expects LCD at 0x%02X: not found", kLcdAddress);
  }

  return result;
}

esp_err_t mcp_write_reg(uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  return i2c_master_write_to_device(kI2CPort, kMcpAddress, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

esp_err_t mcp_read_reg(uint8_t reg, uint8_t* value) {
  if (!value) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_master_write_read_device(kI2CPort, kMcpAddress, &reg, sizeof(reg), value,
                                     sizeof(*value), pdMS_TO_TICKS(100));
}

bool init_mcp23017(Mcp23017State& state) {
  ESP_LOGI(kTag, "MCP23017 init: start");
  state = {};
  if (i2c_probe_device(kMcpAddress, pdMS_TO_TICKS(100)) != ESP_OK) {
    ESP_LOGW(kTag, "MCP23017 not detected at 0x%02X", kMcpAddress);
    return false;
  }

  constexpr uint8_t kIoconValue = 0x00;
  const esp_err_t errors[] = {
      mcp_write_reg(kIocon, kIoconValue),
      mcp_write_reg(kIodira, 0x00),
      mcp_write_reg(kIodirb, 0xFF),
      mcp_write_reg(kGppub, 0xFF),
      mcp_write_reg(kGpioa, kRelayOffValue),
      mcp_write_reg(kGppua, 0x00),
      mcp_write_reg(kGpiob, 0x00),
  };

  for (std::size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    const esp_err_t err = errors[i];
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "MCP23017 init register write failed: %s", esp_err_to_name(err));
      return false;
    }
  }

  state.connected = true;
  state.relay_mask = kRelayOffValue;
  g_mcp_available = true;
  g_relay_mask = kRelayOffValue;
  g_button_mask = 0x00;
  ESP_LOGI(kTag,
           "MCP23017 configured: relays OFF, buttons pullups enabled, IOCON=0x%02X",
           kIoconValue);
  return true;
}

esp_err_t set_relay_mask(Mcp23017State& state, uint8_t mask) {
  const esp_err_t err = mcp_write_reg(kGpioa, mask);
  if (err == ESP_OK) {
    state.relay_mask = mask;
    g_relay_mask = mask;
    ESP_LOGI(kTag, "Relay mask set: 0x%02X", mask);
    update_lcd_status();
  }
  return err;
}

esp_err_t set_relay_mask_safe(uint8_t mask) {
  if (!g_i2c_initialized || !g_mcp_available) {
    return ESP_ERR_INVALID_STATE;
  }
  Mcp23017State state;
  state.connected = g_mcp_available;
  state.relay_mask = g_relay_mask;
  return set_relay_mask(state, mask);
}

uint8_t relay_mask_for_channel_state(int channel, bool on, uint8_t current_mask) {
  const uint8_t bit = static_cast<uint8_t>(1u << channel);
  if (kRelayActiveHigh) {
    return on ? static_cast<uint8_t>(current_mask | bit) : static_cast<uint8_t>(current_mask & ~bit);
  }
  return on ? static_cast<uint8_t>(current_mask & ~bit) : static_cast<uint8_t>(current_mask | bit);
}

bool read_button_inputs(uint8_t* value) {
  return mcp_read_reg(kGpiob, value) == ESP_OK;
}

void configure_int_pin() {
  gpio_config_t conf {};
  std::memset(&conf, 0, sizeof(conf));
  conf.pin_bit_mask = (1ULL << kMcpIntPin);
  conf.mode = GPIO_MODE_INPUT;
  conf.pull_up_en = GPIO_PULLUP_ENABLE;
  conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  conf.intr_type = GPIO_INTR_DISABLE;

  const esp_err_t err = gpio_config(&conf);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "MCP INT pin config failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(kTag, "MCP INTB pin configured on GPIO%d", kMcpIntPin);
  }
}

void run_i2c_diagnostics() {
  luce_log_heap_integrity("run_i2c_diagnostics_enter");
  I2cScanResult scan{};
  const InitPathResult scan_result = run_i2c_scan_flow(scan, nullptr, true);
  if (!scan_result.ok) {
    ESP_LOGW(kTag, "I/O diagnostics abort: I2C bus init failed");
    led_status_set_device_ready(false, false, false);
    return;
  }

  g_lcd_present = scan_result.ok && g_lcd_present;
  led_status_set_device_ready(true, scan.mcp, scan.lcd);

  Mcp23017State mcp_state;
  if (!init_mcp23017(mcp_state)) {
    ESP_LOGW(kTag, "I/O diagnostics degraded: MCP23017 missing or unresponsive");
    led_status_set_device_ready(true, false, scan.lcd);
    update_lcd_status_if_present();
    g_mcp_available = false;
    g_relay_mask = kRelayOffValue;
    g_button_mask = 0x00;
    while (true) {
      update_lcd_status_if_present();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  configure_int_pin();
  g_mcp_available = mcp_state.connected;
  g_relay_mask = mcp_state.relay_mask;
  g_button_mask = 0x00;

  if (set_relay_mask(mcp_state, kRelayOffValue) != ESP_OK) {
    ESP_LOGW(kTag, "I/O diagnostics degraded: cannot write initial relay state");
    g_mcp_available = false;
    return;
  }

  g_lcd.set_mcp_ok(scan.mcp);
  update_lcd_status_if_present();

  uint8_t debounce_counts[8] = {0};
  uint8_t debounced_buttons = 0x00;
  uint8_t last_reported_buttons = 0x00;
  uint8_t current_buttons = 0x00;
  uint8_t relay_mask = kRelayOffValue;
  bool have_button_snapshot = false;
  TickType_t last_button_tick = 0;
  TickType_t last_int_tick = 0;
  TickType_t last_sensor_tick = 0;
  int last_int_level = gpio_get_level(kMcpIntPin);
  TickType_t last_lcd_tick = 0;

  relay_mask = g_relay_mask;

  ESP_LOGI(kTag, "Entering I/O runtime diagnostics loop (button-toggle relay control)");
  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if ((now - last_button_tick) >= kButtonSamplePeriod) {
      last_button_tick = now;
      if (read_button_inputs(&current_buttons)) {
        if (!have_button_snapshot) {
          debounced_buttons = current_buttons;
          last_reported_buttons = current_buttons;
          have_button_snapshot = true;
          ESP_LOGI(kTag, "Button init mask: 0x%02X", current_buttons);
        } else {
          bool changed = false;
          for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint8_t bit_mask = static_cast<uint8_t>(1u << bit);
            bool raw = (current_buttons & (1u << bit)) != 0;
            bool stable = (debounced_buttons & (1u << bit)) != 0;

            if (raw == stable) {
              debounce_counts[bit] = 0;
              continue;
            }

            if (++debounce_counts[bit] >= kButtonDebounceThreshold) {
              debounced_buttons ^= bit_mask;
              debounce_counts[bit] = 0;
              changed = true;
              const bool next_pressed =
                  kButtonActiveLow ? ((debounced_buttons & bit_mask) == 0) : ((debounced_buttons & bit_mask) != 0);
              if (next_pressed) {
                const uint8_t next_relay_mask = relay_mask ^ bit_mask;
                const bool relay_on =
                    kRelayActiveHigh ? ((next_relay_mask & bit_mask) != 0u) : ((next_relay_mask & bit_mask) == 0u);
                led_status_notify_user_input();
                ESP_LOGI(kTag, "Toggling Relay %u %s", static_cast<unsigned>(bit), relay_on ? "ON" : "OFF");
                if (set_relay_mask(mcp_state, next_relay_mask) == ESP_OK) {
                  relay_mask = next_relay_mask;
                  g_relay_mask = next_relay_mask;
                } else {
                  led_status_notify_user_error();
                  ESP_LOGW(kTag, "Button %u relay write failed", static_cast<unsigned>(bit));
                }
              }
            }
          }
          if (changed && debounced_buttons != last_reported_buttons) {
            g_button_mask = debounced_buttons;
            last_reported_buttons = debounced_buttons;
            update_lcd_status_if_present();
          }
        }
      } else {
        led_status_notify_user_error();
        ESP_LOGW(kTag, "Button read failed; retaining last stable mask");
      }
    }

    if ((now - last_int_tick) >= kIntSamplePeriod) {
      last_int_tick = now;
      const int level = gpio_get_level(kMcpIntPin);
      if (level != last_int_level) {
        ESP_LOGI(kTag, "MCP INTB changed to %d", level);
        last_int_level = level;
      }
    }

    if ((now - last_sensor_tick) >= kSensorReadPeriod) {
      last_sensor_tick = now;
      log_sensor_readings();
    }

    if ((now - last_lcd_tick) >= kLcdStatusUpdateTicks) {
      last_lcd_tick = now;
      update_lcd_status_if_present();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void io_task(void*) {
  run_i2c_diagnostics();
}

void io_startup() {
  if (g_i2c_task != nullptr) {
    return;
  }
  if (!luce::start_task_once(g_i2c_task, io_task, luce::task_budget::kTaskIoDiagnostics)) {
    ESP_LOGW(kTag, "Failed to create I2C diagnostics task");
  }
}

bool i2c_lcd_write_text(const char* text) {
  if (!g_lcd_present) {
    ESP_LOGW(kTag, "CLI command lcd_print: LCD not initialized or absent");
    return false;
  }
  if (!text || !*text) {
    ESP_LOGW(kTag, "CLI command lcd_print: missing text argument");
    return false;
  }
  const bool ok = g_lcd.write_text(0, text);
  ESP_LOGI(kTag, "CLI command lcd_print: rc=%s text='%s'", ok ? "OK" : "ERR", text);
  return ok;
}
