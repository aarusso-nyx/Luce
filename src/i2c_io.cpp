#include "luce/i2c_io.h"

#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "luce/dht_sensor.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "nvs.h"
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
constexpr std::uint8_t kRelayAllMask = 0xFF;
constexpr uint8_t kI2CSampleAddressStart = 0x08;
constexpr uint8_t kI2CSampleAddressEnd = 0x77;
constexpr uint8_t kButtonDebounceThreshold = 3;
constexpr TickType_t kButtonSamplePeriod = pdMS_TO_TICKS(40);
constexpr TickType_t kIntSamplePeriod = pdMS_TO_TICKS(200);
constexpr bool kButtonActiveLow = true;
constexpr gpio_num_t kDhtDataPin = GPIO_NUM_4;
constexpr adc_channel_t kLightSensorChannel = ADC_CHANNEL_6;
constexpr adc_channel_t kVoltageSensorChannel = ADC_CHANNEL_7;
constexpr uint16_t kLightBitThreshold = 2048;
constexpr uint16_t kLightThresholdFallback = kLightBitThreshold;
constexpr TickType_t kSensorReadPeriod = pdMS_TO_TICKS(10000);
constexpr TickType_t kLcdStatusUpdateTicks = pdMS_TO_TICKS(1000);
constexpr TickType_t kLcdPageRotationTicks = pdMS_TO_TICKS(4000);
constexpr std::size_t kLcdLogBufferDepth = 16;
constexpr std::size_t kLcdLogLineStorage = 80;
constexpr std::size_t kLcdLogDisplayRows = 3;
constexpr const char* kRelaysNs = "relays";
constexpr const char* kRelaysStateKey = "state";
constexpr const char* kRelaysStateFormatKey = "state_fmt";
constexpr const char* kRelaysNightKey = "night_mask";
constexpr const char* kSensorNs = "sensor";
constexpr const char* kSensorThresholdKey = "threshold";
constexpr std::uint8_t kRelaysStateFormatV1 = 1u;
constexpr TickType_t kNightUpdatePeriod = pdMS_TO_TICKS(60000);

// LCD I2C backpack is fixed at 0x27 on the current hardware map and 3.3V bus-compatible.
constexpr uint8_t kLcdAddress = 0x27;

bool g_i2c_initialized = false;
bool g_mcp_available = false;
uint8_t g_relay_mask = 0x00;
uint8_t g_relay_night_mask = 0x00;
uint8_t g_button_mask = 0x00;
bool g_light_sensor_ready = false;
adc_oneshot_unit_handle_t g_light_sensor_handle = nullptr;
float g_last_temperature_c = 0.0f;
float g_last_humidity_percent = 0.0f;
int g_last_light_raw = 0;
int g_last_voltage_raw = 0;
bool g_last_dht_read_ok = false;
bool g_lcd_present = false;
TaskHandle_t g_i2c_task = nullptr;
std::uint16_t g_light_threshold = kLightThresholdFallback;
bool g_is_day = true;
uint8_t g_relay_output_mask = kRelayOffValue;
portMUX_TYPE g_lcd_log_lock = portMUX_INITIALIZER_UNLOCKED;
char g_lcd_log_lines[kLcdLogBufferDepth][kLcdLogLineStorage] = {0};
std::size_t g_lcd_log_count = 0;
std::size_t g_lcd_log_head = 0;
std::size_t g_lcd_log_view_offset = 0;
vprintf_like_t g_lcd_prev_vprintf = nullptr;
bool g_lcd_log_hook_ready = false;

std::size_t lcd_log_max_view_offset(std::size_t stored_count) {
  return (stored_count > kLcdLogDisplayRows) ? (stored_count - kLcdLogDisplayRows) : 0u;
}

void clamp_lcd_log_view_offset(std::size_t stored_count) {
  const std::size_t max_offset = lcd_log_max_view_offset(stored_count);
  if (g_lcd_log_view_offset > max_offset) {
    g_lcd_log_view_offset = max_offset;
  }
}

void lcd_log_adjust_view_offset(int step) {
  if (g_lcd_log_count <= kLcdLogDisplayRows) {
    g_lcd_log_view_offset = 0u;
    return;
  }
  const std::size_t max_offset = lcd_log_max_view_offset(g_lcd_log_count);
  if (step > 0) {
    const std::size_t positive = static_cast<std::size_t>(step);
    const std::size_t remaining = max_offset - g_lcd_log_view_offset;
    g_lcd_log_view_offset += (positive < remaining) ? positive : remaining;
  } else if (step < 0) {
    const std::size_t negative = static_cast<std::size_t>(-step);
    g_lcd_log_view_offset = (negative < g_lcd_log_view_offset) ? (g_lcd_log_view_offset - negative) : 0u;
  }
}

void sanitize_for_lcd_line(const char* text, char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  if (!text || text[0] == '\0') {
    out[0] = '\0';
    return;
  }

  std::size_t out_idx = 0;
  for (std::size_t in_idx = 0; text[in_idx] != '\0' && out_idx + 1 < out_size; ++in_idx) {
    const char ch = text[in_idx];
    if (ch == '\r' || ch == '\n') {
      break;
    }
    if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7F) {
      continue;
    }
    out[out_idx++] = ch;
  }
  out[out_idx] = '\0';
}

void lcd_log_append(const char* text) {
  if (!text || text[0] == '\0') {
    return;
  }

  char cleaned[kLcdLogLineStorage] = {0};
  sanitize_for_lcd_line(text, cleaned, sizeof(cleaned));
  if (cleaned[0] == '\0') {
    return;
  }

  portENTER_CRITICAL(&g_lcd_log_lock);
  std::strncpy(g_lcd_log_lines[g_lcd_log_head], cleaned, sizeof(g_lcd_log_lines[g_lcd_log_head]) - 1);
  g_lcd_log_lines[g_lcd_log_head][sizeof(g_lcd_log_lines[g_lcd_log_head]) - 1] = '\0';

  g_lcd_log_head = (g_lcd_log_head + 1) % kLcdLogBufferDepth;
  if (g_lcd_log_count < kLcdLogBufferDepth) {
    ++g_lcd_log_count;
  }
  clamp_lcd_log_view_offset(g_lcd_log_count);
  portEXIT_CRITICAL(&g_lcd_log_lock);
}

int lcd_log_vprintf(const char* format, va_list args) {
  int rc = 0;
  if (format && format[0] != '\0') {
    char formatted[kLcdLogLineStorage] = {0};
    va_list args_copy;
    va_copy(args_copy, args);
    const int wrote = std::vsnprintf(formatted, sizeof(formatted), format, args_copy);
    va_end(args_copy);
    if (wrote > 0) {
      lcd_log_append(formatted);
    }
  }

  if (g_lcd_prev_vprintf != nullptr) {
    rc = g_lcd_prev_vprintf(format, args);
  }
  return rc;
}

void ensure_lcd_log_capture() {
  if (g_lcd_log_hook_ready) {
    return;
  }

  g_lcd_prev_vprintf = esp_log_set_vprintf(lcd_log_vprintf);
  g_lcd_log_hook_ready = true;
}

std::uint8_t relay_state_to_output_mask(std::uint8_t relay_mask) {
  if (kRelayActiveHigh) {
    return static_cast<uint8_t>(relay_mask & kRelayAllMask);
  }
  return static_cast<uint8_t>(relay_mask ^ kRelayAllMask);
}

std::uint8_t relay_output_to_state_mask(std::uint8_t relay_mask) {
  if (kRelayActiveHigh) {
    return static_cast<uint8_t>(relay_mask & kRelayAllMask);
  }
  return static_cast<uint8_t>(relay_mask ^ kRelayAllMask);
}

std::uint8_t apply_night_policy(std::uint8_t requested_mask) {
  if (g_is_day) {
    return static_cast<uint8_t>(requested_mask & static_cast<uint8_t>(~g_relay_night_mask));
  }
  return requested_mask;
}


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
  const bool light_ok = adc_oneshot_config_channel(g_light_sensor_handle, kLightSensorChannel, &chan_cfg) == ESP_OK;
  const bool voltage_ok = adc_oneshot_config_channel(g_light_sensor_handle, kVoltageSensorChannel, &chan_cfg) == ESP_OK;
  if (!light_ok) {
    ESP_LOGW(kTag, "Light sensor ADC channel config failed");
  }
  if (!voltage_ok) {
    ESP_LOGW(kTag, "Voltage sensor ADC channel config failed");
  }
  if (!light_ok && !voltage_ok) {
    adc_oneshot_del_unit(g_light_sensor_handle);
    g_light_sensor_handle = nullptr;
    return;
  }
  g_light_sensor_ready = true;
}

void log_sensor_readings() {
  init_light_sensor();
  int light_raw = 0;
  int voltage_raw = 0;
  if (g_light_sensor_ready) {
    int converted = 0;
    const esp_err_t raw_rc = adc_oneshot_read(g_light_sensor_handle, kLightSensorChannel, &converted);
    if (raw_rc == ESP_OK) {
      light_raw = converted;
    }
    if (adc_oneshot_read(g_light_sensor_handle, kVoltageSensorChannel, &converted) == ESP_OK) {
      voltage_raw = converted;
    }
  }
  g_last_voltage_raw = voltage_raw;
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
  int voltage_raw = 0;
  if (g_light_sensor_ready) {
    int converted = 0;
    if (adc_oneshot_read(g_light_sensor_handle, kLightSensorChannel, &converted) == ESP_OK) {
      light_raw = converted;
    }
    if (adc_oneshot_read(g_light_sensor_handle, kVoltageSensorChannel, &converted) == ESP_OK) {
      voltage_raw = converted;
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
  g_last_voltage_raw = voltage_raw;
  g_last_light_raw = light_raw;

  snapshot.temperature_c = g_last_temperature_c;
  snapshot.humidity_percent = g_last_humidity_percent;
  snapshot.light_raw = g_last_light_raw;
  snapshot.voltage_raw = g_last_voltage_raw;
  snapshot.dht_ok = g_last_dht_read_ok;
  return dht_ok;
}

constexpr uint8_t kLcdPcfRsBit = 0;
constexpr uint8_t kLcdPcfRwBit = 1;
constexpr uint8_t kLcdPcfEnBit = 2;
constexpr uint8_t kLcdPcfBacklightBit = 3;
constexpr uint8_t kLcdCols = 20;
constexpr uint8_t kLcdRows = 4;

enum class LcdPage : std::uint8_t {
  kSummary = 0,
  kSensors = 1,
  kNetwork = 2,
  kRelays = 3,
  kSystem = 4,
  kSystem2 = 5,
  kLogs = 6,
};

constexpr std::uint8_t kLcdPageCount = 7;

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
LcdPage g_lcd_page = LcdPage::kSummary;
TickType_t g_lcd_page_tick = 0;

void write_lcd_row(std::uint8_t row, const char* text) {
  if (!g_lcd_present) {
    return;
  }
  g_lcd.write_text_line(row, text ? text : "");
}

void trim_to_lcd(char* out, std::size_t out_size, const char* text) {
  if (!out || out_size == 0) {
    return;
  }
  if (!text) {
    out[0] = '\0';
    return;
  }
  std::snprintf(out, out_size, "%.*s", static_cast<int>(out_size - 1), text);
}

void write_lcd_line(std::uint8_t row, const char* text) {
  char line[kLcdCols + 1] = {0};
  trim_to_lcd(line, sizeof(line), text);
  for (std::size_t idx = std::strlen(line); idx < kLcdCols; ++idx) {
    line[idx] = ' ';
  }
  write_lcd_row(row, line);
}

void render_lcd_summary_page() {
  if (!g_lcd_present) {
    return;
  }

  char line0[21] = {0};
  char line1[21] = {0};
  char line2[21] = {0};
  char line3[21] = {0};
  char ip[16] = {0};
  wifi_copy_ip_str(ip, sizeof(ip));

  std::snprintf(line0, sizeof(line0), "LUCE %-11s", LUCE_STRATEGY_NAME);
  const char* net_state = wifi_is_enabled() ? (wifi_is_ip_ready() ? "OK" : "CN") : "OFF";
  std::snprintf(line1, sizeof(line1), "W:%-3s I:%-11.11s", net_state, ip);
  std::snprintf(line2, sizeof(line2), "R:0x%02X N:0x%02X", g_relay_mask, g_relay_night_mask);
  if (g_last_dht_read_ok) {
    const uint8_t light_state = g_last_light_raw >= static_cast<int>(kLightBitThreshold) ? 1u : 0u;
    std::snprintf(line3, sizeof(line3), "T %.1f H%.1f L:%u", g_last_temperature_c, g_last_humidity_percent, light_state);
  } else {
    const uint8_t light_state = g_last_light_raw >= static_cast<int>(kLightBitThreshold) ? 1u : 0u;
    std::snprintf(line3, sizeof(line3), "DHT --.- --.- L:%u", light_state);
  }
  write_lcd_line(0, line0);
  write_lcd_line(1, line1);
  write_lcd_line(2, line2);
  write_lcd_line(3, line3);
}

void render_lcd_sensors_page() {
  if (!g_lcd_present) {
    return;
  }

  char line0[21] = {0};
  char line1[21] = {0};
  char line2[21] = {0};
  char line3[21] = {0};
  const char* day_state = (g_last_light_raw > static_cast<int>(g_light_threshold)) ? "DAY" : "NITE";

  std::snprintf(line0, sizeof(line0), "L:%4d V:%4d", g_last_light_raw, g_last_voltage_raw);
  std::snprintf(line1, sizeof(line1), "Thr:%4u %s", static_cast<unsigned>(g_light_threshold), day_state);
  std::snprintf(line2, sizeof(line2), "T:%.1fC H:%.1f%%", g_last_temperature_c, g_last_humidity_percent);
  std::snprintf(line3, sizeof(line3), "LED raw:0x%02X", g_relay_output_mask);
  write_lcd_line(0, line0);
  write_lcd_line(1, line1);
  write_lcd_line(2, line2);
  write_lcd_line(3, line3);
}

void render_lcd_network_page() {
  if (!g_lcd_present) {
    return;
  }

  char line0[21] = {0};
  char line1[21] = {0};
  char line2[21] = {0};
  char line3[21] = {0};
  char ssid[33] = {0};
  char ip[16] = {0};
  int rssi = 0;

  wifi_get_ssid(ssid, sizeof(ssid));
  wifi_copy_ip_str(ip, sizeof(ip));
  wifi_get_rssi(&rssi);

  std::snprintf(line0, sizeof(line0), "SSID:%-14.14s", ssid);
  std::snprintf(line1, sizeof(line1), "IP:%-15.15s", ip);
  std::snprintf(line2, sizeof(line2), "WIFI:%s RSSI:%4d", wifi_is_connected() ? "ON" : "OFF", rssi);
  std::snprintf(line3, sizeof(line3), "MQ:%s NTP:%s", mqtt_is_connected() ? "ON" : "OFF",
                ntp_is_synced() ? "ON" : "OFF");
  write_lcd_line(0, line0);
  write_lcd_line(1, line1);
  write_lcd_line(2, line2);
  write_lcd_line(3, line3);
}

void render_lcd_relays_page() {
  if (!g_lcd_present) {
    return;
  }

  char line0[21] = {0};
  char line1[21] = {0};
  char line2[21] = {0};
  char line3[21] = {0};
  std::uint8_t on_count = 0;
  for (std::size_t idx = 0; idx < 8; ++idx) {
    if ((g_relay_mask & (1u << idx)) != 0u) {
      ++on_count;
    }
  }
  const char* day_state = g_is_day ? "DAY" : "NITE";
  std::snprintf(line0, sizeof(line0), "State:0x%02X (on:%u)", g_relay_mask, static_cast<unsigned>(on_count));
  std::snprintf(line1, sizeof(line1), "Night:0x%02X", g_relay_night_mask);
  std::snprintf(line2, sizeof(line2), "Bits %c%c%c%c%c%c%c%c", (g_relay_mask & 0x80u) ? '1' : '0',
                (g_relay_mask & 0x40u) ? '1' : '0', (g_relay_mask & 0x20u) ? '1' : '0',
                (g_relay_mask & 0x10u) ? '1' : '0', (g_relay_mask & 0x08u) ? '1' : '0',
                (g_relay_mask & 0x04u) ? '1' : '0', (g_relay_mask & 0x02u) ? '1' : '0',
                (g_relay_mask & 0x01u) ? '1' : '0');
  std::snprintf(line3, sizeof(line3), "Policy:%s", day_state);
  write_lcd_line(0, line0);
  write_lcd_line(1, line1);
  write_lcd_line(2, line2);
  write_lcd_line(3, line3);
}

void render_lcd_system_page() {
  if (!g_lcd_present) {
    return;
  }

  char line0[21] = {0};
  char line1[21] = {0};
  char line2[21] = {0};
  char line3[21] = {0};
  const std::uint64_t uptime = static_cast<std::uint64_t>(esp_timer_get_time() / 1000000ULL);
  const char* reset_reason = luce_reset_reason_to_string(esp_reset_reason());
  std::snprintf(line0, sizeof(line0), "RST:%-16.16s", reset_reason);
  std::snprintf(line1, sizeof(line1), "UP:%llus", static_cast<unsigned long long>(uptime));
  const char* wifi_state = wifi_is_enabled() ? (wifi_is_ip_ready() ? "OK" : "C") : "OFF";
  std::snprintf(line2, sizeof(line2), "WIFI:%s", wifi_state);
  std::snprintf(line3, sizeof(line3), "MQ:%s MDNS:%s", mqtt_is_connected() ? "ON" : "OFF",
                mdns_is_running() ? "ON" : "OFF");
  write_lcd_line(0, line0);
  write_lcd_line(1, line1);
  write_lcd_line(2, line2);
  write_lcd_line(3, line3);
}

void render_lcd_system2_page() {
  if (!g_lcd_present) {
    return;
  }

  char line0[21] = {0};
  char line1[21] = {0};
  char line2[21] = {0};
  char line3[21] = {0};

  const esp_app_desc_t* app_desc = esp_app_get_description();
  char app_version[16] = "n/a";
  if (app_desc && app_desc->version[0] != '\0') {
    std::snprintf(app_version, sizeof(app_version), "%.*s", 15, app_desc->version);
  }
  const uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT) >> 10;
  const uint32_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) >> 10;
  char sha_display[9] = "n/a";
  if (LUCE_GIT_SHA[0] != '\0') {
    std::snprintf(sha_display, sizeof(sha_display), "%.*s", 8, LUCE_GIT_SHA);
  }

  std::snprintf(line0, sizeof(line0), "Ver:%-15.15s", app_version);
  std::snprintf(line1, sizeof(line1), "IDF:%-16.16s", esp_get_idf_version());
  std::snprintf(line2, sizeof(line2), "H:%luK/%luK", static_cast<unsigned long>(free_heap),
                static_cast<unsigned long>(min_heap));
  std::snprintf(line3, sizeof(line3), "SHA:%-8s", sha_display);
  write_lcd_line(0, line0);
  write_lcd_line(1, line1);
  write_lcd_line(2, line2);
  write_lcd_line(3, line3);
}

void render_lcd_logs_page() {
  if (!g_lcd_present) {
    return;
  }

  portENTER_CRITICAL(&g_lcd_log_lock);
  const std::size_t stored_count = g_lcd_log_count;
  const std::size_t oldest_index = (g_lcd_log_head + kLcdLogBufferDepth - stored_count) % kLcdLogBufferDepth;
  clamp_lcd_log_view_offset(stored_count);
  const std::size_t max_offset = lcd_log_max_view_offset(stored_count);
  const std::size_t start = (stored_count <= kLcdLogDisplayRows)
                               ? 0u
                               : (stored_count - kLcdLogDisplayRows - g_lcd_log_view_offset);
  char header[21] = {0};
  if (max_offset == 0u) {
    std::snprintf(header, sizeof(header), "LOGS");
  } else {
    const std::size_t page_total = max_offset + 1u;
    const std::size_t page_index = (max_offset - g_lcd_log_view_offset) + 1u;
    std::snprintf(header, sizeof(header), "LOGS %02u/%02u", static_cast<unsigned>(page_index),
                  static_cast<unsigned>(page_total));
  }

  write_lcd_line(0, header);
  for (std::size_t row = 0; row < kLcdLogDisplayRows; ++row) {
    if ((start + row) >= stored_count) {
      write_lcd_line(1 + row, "");
      continue;
    }
    const std::size_t index = (oldest_index + start + row) % kLcdLogBufferDepth;
    write_lcd_line(1 + row, g_lcd_log_lines[index]);
  }
  portEXIT_CRITICAL(&g_lcd_log_lock);
}

void update_lcd_status() {
  if (!g_lcd_present) {
    return;
  }

  switch (g_lcd_page) {
    case LcdPage::kSummary:
      render_lcd_summary_page();
      break;
    case LcdPage::kSensors:
      render_lcd_sensors_page();
      break;
    case LcdPage::kNetwork:
      render_lcd_network_page();
      break;
    case LcdPage::kRelays:
      render_lcd_relays_page();
      break;
    case LcdPage::kSystem:
      render_lcd_system_page();
      break;
    case LcdPage::kSystem2:
      render_lcd_system2_page();
      break;
    case LcdPage::kLogs:
      render_lcd_logs_page();
      break;
    default:
      g_lcd_page = LcdPage::kSummary;
      render_lcd_summary_page();
      break;
  }
}

void rotate_lcd_page_if_needed(TickType_t now) {
  if (g_lcd_page_tick == 0) {
    g_lcd_page_tick = now;
    return;
  }
  if ((now - g_lcd_page_tick) >= kLcdPageRotationTicks) {
    const LcdPage next_page =
        static_cast<LcdPage>((static_cast<std::uint8_t>(g_lcd_page) + 1u) % kLcdPageCount);
    g_lcd_page = next_page;
    if (g_lcd_page == LcdPage::kLogs) {
      portENTER_CRITICAL(&g_lcd_log_lock);
      g_lcd_log_view_offset = 0u;
      portEXIT_CRITICAL(&g_lcd_log_lock);
    }
    g_lcd_page_tick = now;
  }
}

void update_lcd_status_if_present() {
  if (g_lcd_present) {
    update_lcd_status();
  }
}

void io_lcd_show_logs_page() {
  if (!g_lcd_present) {
    return;
  }
  g_lcd_page = LcdPage::kLogs;
  g_lcd_log_view_offset = 0u;
  g_lcd_page_tick = xTaskGetTickCount();
  portENTER_CRITICAL(&g_lcd_log_lock);
  clamp_lcd_log_view_offset(g_lcd_log_count);
  portEXIT_CRITICAL(&g_lcd_log_lock);
  render_lcd_logs_page();
}

void io_lcd_log_page_next() {
  if (!g_lcd_present || g_lcd_page != LcdPage::kLogs) {
    return;
  }
  portENTER_CRITICAL(&g_lcd_log_lock);
  lcd_log_adjust_view_offset(1);
  portEXIT_CRITICAL(&g_lcd_log_lock);
  update_lcd_status_if_present();
}

void io_lcd_log_page_prev() {
  if (!g_lcd_present || g_lcd_page != LcdPage::kLogs) {
    return;
  }
  portENTER_CRITICAL(&g_lcd_log_lock);
  lcd_log_adjust_view_offset(-1);
  portEXIT_CRITICAL(&g_lcd_log_lock);
  update_lcd_status_if_present();
}

void io_lcd_log_page_reset() {
  if (!g_lcd_present || g_lcd_page != LcdPage::kLogs) {
    return;
  }
  portENTER_CRITICAL(&g_lcd_log_lock);
  g_lcd_log_view_offset = 0u;
  portEXIT_CRITICAL(&g_lcd_log_lock);
  update_lcd_status_if_present();
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
  g_relay_mask = 0x00;
  g_relay_output_mask = kRelayOffValue;
  g_button_mask = 0x00;
  ESP_LOGI(kTag,
           "MCP23017 configured: relays OFF, buttons pullups enabled, IOCON=0x%02X",
           kIoconValue);
  return true;
}

esp_err_t set_relay_mask_internal(Mcp23017State& state, uint8_t state_mask, bool persist_state) {
  const std::uint8_t requested_mask = static_cast<std::uint8_t>(state_mask & kRelayAllMask);
  const std::uint8_t filtered_mask = apply_night_policy(requested_mask);
  const std::uint8_t output_mask = relay_state_to_output_mask(filtered_mask);
  const esp_err_t err = mcp_write_reg(kGpioa, output_mask);
  if (err == ESP_OK) {
    state.relay_mask = output_mask;
    g_relay_mask = requested_mask;
    g_relay_output_mask = output_mask;
    if (persist_state) {
      nvs_handle_t handle {};
      if (nvs_open(kRelaysNs, NVS_READWRITE, &handle) == ESP_OK) {
        const bool state_saved =
            nvs_set_u8(handle, kRelaysStateFormatKey, kRelaysStateFormatV1) == ESP_OK &&
            nvs_set_u32(handle, kRelaysStateKey, static_cast<std::uint32_t>(requested_mask)) == ESP_OK;
        if (state_saved) {
          (void)nvs_commit(handle);
        } else {
          ESP_LOGW(kTag, "Failed to persist relay state");
        }
        nvs_close(handle);
      } else {
        ESP_LOGW(kTag, "Relay namespace open failed; relay state not persisted");
      }
    }
    update_lcd_status();
    ESP_LOGI(kTag, "Relay mask set: 0x%02X (effective 0x%02X)", requested_mask, output_mask);
  }
  return err;
}

esp_err_t set_relay_mask(Mcp23017State& state, uint8_t mask) {
  return set_relay_mask_internal(state, mask, true);
}

esp_err_t set_relay_mask_safe(uint8_t mask) {
  if (!g_i2c_initialized || !g_mcp_available) {
    return ESP_ERR_INVALID_STATE;
  }
  Mcp23017State state;
  state.connected = g_mcp_available;
  state.relay_mask = g_relay_output_mask;
  return set_relay_mask(state, mask);
}

uint8_t relay_mask_for_channel_state(int channel, bool on, uint8_t current_mask) {
  const uint8_t bit = static_cast<uint8_t>(1u << channel);
  return on ? static_cast<uint8_t>(current_mask | bit) : static_cast<uint8_t>(current_mask & ~bit);
}

bool read_button_inputs(uint8_t* value) {
  return mcp_read_reg(kGpiob, value) == ESP_OK;
}

std::uint8_t load_relay_state_from_nvs(std::uint8_t fallback) {
  nvs_handle_t handle {};
  if (nvs_open(kRelaysNs, NVS_READONLY, &handle) != ESP_OK) {
    return fallback;
  }

  std::uint32_t state_u32 = fallback;
  if (nvs_get_u32(handle, kRelaysStateKey, &state_u32) != ESP_OK) {
    nvs_close(handle);
    return fallback;
  }
  std::uint8_t format = 0;
  const bool is_state_format = (nvs_get_u8(handle, kRelaysStateFormatKey, &format) == ESP_OK) &&
                               (format == kRelaysStateFormatV1);
  nvs_close(handle);
  const std::uint8_t stored_mask = static_cast<std::uint8_t>(state_u32 & 0xFFu);
  return is_state_format ? stored_mask : relay_output_to_state_mask(stored_mask);
}

std::uint8_t load_relay_night_mask_from_nvs() {
  nvs_handle_t handle {};
  if (nvs_open(kRelaysNs, NVS_READONLY, &handle) != ESP_OK) {
    return 0x00;
  }
  std::uint8_t night_mask = 0x00;
  (void)nvs_get_u8(handle, kRelaysNightKey, &night_mask);
  nvs_close(handle);
  return night_mask;
}

void load_light_threshold_from_nvs() {
  nvs_handle_t handle {};
  if (nvs_open(kSensorNs, NVS_READONLY, &handle) != ESP_OK) {
    g_light_threshold = kLightThresholdFallback;
    return;
  }
  std::uint16_t threshold = kLightThresholdFallback;
  (void)nvs_get_u16(handle, kSensorThresholdKey, &threshold);
  nvs_close(handle);
  g_light_threshold = threshold;
}

void io_set_relay_night_mask(std::uint8_t night_mask) {
  g_relay_night_mask = night_mask;
  nvs_handle_t handle {};
  if (nvs_open(kRelaysNs, NVS_READWRITE, &handle) == ESP_OK) {
    if (nvs_set_u8(handle, kRelaysNightKey, night_mask) == ESP_OK) {
      (void)nvs_commit(handle);
    } else {
      ESP_LOGW(kTag, "Failed to persist relays/night mask");
    }
    nvs_close(handle);
  } else {
    ESP_LOGW(kTag, "Relay namespace open failed; relays/night not persisted");
  }
  io_apply_relay_policy();
}

void io_set_light_threshold(std::uint16_t threshold) {
  g_light_threshold = threshold;
  nvs_handle_t handle {};
  if (nvs_open(kSensorNs, NVS_READWRITE, &handle) == ESP_OK) {
    if (nvs_set_u16(handle, kSensorThresholdKey, threshold) == ESP_OK) {
      (void)nvs_commit(handle);
    } else {
      ESP_LOGW(kTag, "Failed to persist sensor threshold");
    }
    nvs_close(handle);
  } else {
    ESP_LOGW(kTag, "Sensor namespace open failed; threshold not persisted");
  }
  io_apply_relay_policy();
}

void io_apply_relay_policy() {
  if (!g_i2c_initialized || !g_mcp_available) {
    return;
  }
  g_is_day = (g_last_light_raw > static_cast<int>(g_light_threshold));
  Mcp23017State state;
  state.connected = g_mcp_available;
  state.relay_mask = g_relay_output_mask;
  (void)set_relay_mask_internal(state, g_relay_mask, false);
}

std::uint8_t io_relay_night_mask() {
  return g_relay_night_mask;
}

std::uint16_t io_light_threshold() {
  return g_light_threshold;
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
    g_relay_mask = 0x00;
    g_button_mask = 0x00;
    while (true) {
      update_lcd_status_if_present();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  configure_int_pin();
  g_mcp_available = mcp_state.connected;
  g_relay_mask = relay_output_to_state_mask(mcp_state.relay_mask);
  g_relay_output_mask = mcp_state.relay_mask;
  g_button_mask = 0x00;
  g_relay_night_mask = load_relay_night_mask_from_nvs();
  load_light_threshold_from_nvs();
  log_sensor_readings();
  g_is_day = g_last_light_raw > static_cast<int>(g_light_threshold);

  const std::uint8_t startup_mask = load_relay_state_from_nvs(0x00);
  if (set_relay_mask(mcp_state, startup_mask) != ESP_OK) {
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
  uint8_t relay_mask = g_relay_mask;
  bool have_button_snapshot = false;
  TickType_t last_button_tick = 0;
  TickType_t last_int_tick = 0;
  TickType_t last_sensor_tick = 0;
  TickType_t last_night_tick = 0;
  int last_int_level = gpio_get_level(kMcpIntPin);
  TickType_t last_lcd_tick = 0;

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
                if (g_lcd_page == LcdPage::kLogs && bit <= 1u) {
                  portENTER_CRITICAL(&g_lcd_log_lock);
                  if (bit == 0u) {
                    lcd_log_adjust_view_offset(1);
                    ESP_LOGI(kTag, "Log page scroll older");
                  } else {
                    lcd_log_adjust_view_offset(-1);
                    ESP_LOGI(kTag, "Log page scroll newer");
                  }
                  portEXIT_CRITICAL(&g_lcd_log_lock);
                } else {
                  const uint8_t next_relay_mask = relay_mask ^ bit_mask;
                  const bool relay_on = ((next_relay_mask & bit_mask) != 0u);
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

    if ((now - last_night_tick) >= kNightUpdatePeriod) {
      last_night_tick = now;
      io_apply_relay_policy();
    }

    if ((now - last_lcd_tick) >= kLcdStatusUpdateTicks) {
      last_lcd_tick = now;
      rotate_lcd_page_if_needed(now);
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
  ensure_lcd_log_capture();
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
