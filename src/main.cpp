#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>
#include <cerrno>
#include <cstdlib>

#include "luce_build.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if LUCE_HAS_NVS
#include "nvs.h"
#include "nvs_flash.h"
#endif

#if LUCE_HAS_I2C
#include "driver/i2c.h"
#endif

#if LUCE_HAS_CLI
#include "driver/uart.h"
#endif

namespace {

constexpr const char* kTag = "luce_boot";
void blink_alive();

#ifndef LUCE_STAGE4_DIAG
#define LUCE_STAGE4_DIAG 1
#endif

#ifndef LUCE_STAGE4_LCD
#define LUCE_STAGE4_LCD 1
#endif

#ifndef LUCE_STAGE4_CLI
#define LUCE_STAGE4_CLI 1
#endif

#ifndef LUCE_DEBUG_STAGE4_DIAG
#define LUCE_DEBUG_STAGE4_DIAG 1
#endif

constexpr std::size_t kDiagTaskStackWords = 8192;
constexpr std::size_t kCliTaskStackWords = 6144;
constexpr std::size_t kBlinkTaskStackWords = 2048;

TaskHandle_t g_diag_task = nullptr;
TaskHandle_t g_cli_task = nullptr;

extern bool g_i2c_initialized;
extern bool g_mcp_available;
extern uint8_t g_relay_mask;
extern uint8_t g_button_mask;
#if LUCE_HAS_LCD
extern bool g_lcd_present;
#endif

enum class InitPathStatus : uint8_t {
  kUnknown = 0,
  kSuccess,
  kFailure,
};

struct InitPathResult {
  bool ok;
  esp_err_t error;
  InitPathStatus status;
};

enum class McpMaskFormat : uint8_t {
  kRuntime = 0,
  kStatusCommand,
};

const char* reset_reason_to_string(esp_reset_reason_t reason);
std::size_t init_path_reset_reason_line(char* out, std::size_t out_size,
                                       esp_reset_reason_t reason);

void log_heap_integrity(const char* context) {
#if LUCE_DEBUG_STAGE4_DIAG
  if (!context) {
    context = "unknown";
  }
  const bool ok = heap_caps_check_integrity_all(true);
  ESP_LOGI(kTag, "Heap integrity (%s): %s", context, ok ? "OK" : "CORRUPTED");
#else
  (void)context;
#endif
}

std::size_t format_mcp_mask_line(char* out, std::size_t out_size, uint8_t relay_mask,
                                uint8_t button_mask, McpMaskFormat format = McpMaskFormat::kRuntime) {
  switch (format) {
    case McpMaskFormat::kStatusCommand:
      return std::snprintf(out, out_size, "relay_mask=0x%02X button_mask=0x%02X", relay_mask,
                           button_mask);
    case McpMaskFormat::kRuntime:
    default:
      return std::snprintf(out, out_size, "REL:0x%02X BTN:0x%02X", relay_mask, button_mask);
  }
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

void log_startup_banner() {
  char reason_line[48];
  init_path_reset_reason_line(reason_line, sizeof(reason_line), esp_reset_reason());

  ESP_LOGI(kTag, "LUCE STAGE%d", LUCE_STAGE);
  ESP_LOGI(kTag, "Build timestamp: %s %s", __DATE__, __TIME__);
  ESP_LOGI(kTag, "Project version: %s", LUCE_PROJECT_VERSION);
  ESP_LOGI(kTag, "Git SHA: %s", LUCE_GIT_SHA);
  ESP_LOGI(kTag, "Reset reason: %s", reason_line);
}

void log_status_health_lines() {
  char reason_line[48];
  char mask_line[48];
  init_path_reset_reason_line(reason_line, sizeof(reason_line), esp_reset_reason());
  format_mcp_mask_line(mask_line, sizeof(mask_line), g_relay_mask, g_button_mask,
                       McpMaskFormat::kStatusCommand);

  ESP_LOGI(kTag, "status: stage=%d reset=%s uptime=%llus", LUCE_STAGE, reason_line,
           static_cast<long long>(esp_timer_get_time() / 1000000ULL));
  ESP_LOGI(kTag, "status: heap_free=%u min_free=%u", heap_caps_get_free_size(MALLOC_CAP_8BIT),
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "status: task_watermark_words=%u", uxTaskGetStackHighWaterMark(nullptr));
  ESP_LOGI(kTag, "status: feature i2c=%d lcd=%d cli=%d", LUCE_HAS_I2C, LUCE_HAS_LCD, LUCE_HAS_CLI);
#if LUCE_HAS_I2C
  ESP_LOGI(kTag, "status: i2c_init=%d mcp=%d %s", g_i2c_initialized, g_mcp_available, mask_line);
#endif
}

std::size_t init_path_reset_reason_line(char* out, std::size_t out_size, esp_reset_reason_t reason) {
  return std::snprintf(out, out_size, "%s (%d)", reset_reason_to_string(reason),
                       static_cast<int>(reason));
}

void log_runtime_status_line(uint64_t uptime_s, bool i2c_ok, bool mcp_ok, uint8_t relay_mask,
                            uint8_t button_mask) {
  char mask_line[32];
  format_mcp_mask_line(mask_line, sizeof(mask_line), relay_mask, button_mask);
  ESP_LOGI(kTag, "LUCE S3 %lus | I2C:%s MCP:%s %s", uptime_s, i2c_ok ? "ok" : "no",
           mcp_ok ? "ok" : "no", mask_line);
}

void log_stage4_watermarks(const char* context) {
#if LUCE_DEBUG_STAGE4_DIAG
  if (!context) {
    context = "unknown";
  }
  UBaseType_t cli = 0;
  UBaseType_t diag = 0;
#if LUCE_HAS_CLI
  if (g_cli_task) {
    cli = uxTaskGetStackHighWaterMark(g_cli_task);
  }
#endif
#if LUCE_HAS_I2C
  if (g_diag_task) {
    diag = uxTaskGetStackHighWaterMark(g_diag_task);
  }
#endif
  const UBaseType_t now = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGI(kTag, "Stack watermark (%s): cli=%u diag=%u current=%u", context, cli, diag, now);
#else
  (void)context;
#endif
}

const char* reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SOFTWARE";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "POWER_GLITCH";
    default:
      return "UNKNOWN";
  }
}

void print_chip_info() {
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  ESP_LOGI(kTag, "Chip: model=%d revision=%d cores=%d", chip_info.model,
           chip_info.revision, chip_info.cores);
  ESP_LOGI(kTag, "Features: %s%s%s%s%s", chip_info.features & CHIP_FEATURE_WIFI_BGN ? "WIFI " : "",
           chip_info.features & CHIP_FEATURE_BT ? "BT " : "",
           chip_info.features & CHIP_FEATURE_BLE ? "BLE " : "",
           chip_info.features & CHIP_FEATURE_EMB_FLASH ? "EMB_FLASH " : "",
           chip_info.features & CHIP_FEATURE_EMB_PSRAM ? "EMB_PSRAM " : "");
  ESP_LOGI(kTag, "CPU frequency: %u MHz", esp_clk_cpu_freq() / 1000000ULL);
}

void print_app_info() {
  const esp_app_desc_t* app_desc = esp_app_get_description();
  if (!app_desc) {
    ESP_LOGW(kTag, "esp_app_get_description() returned null");
    return;
  }
  ESP_LOGI(kTag, "App version: %s", app_desc->version);
  ESP_LOGI(kTag, "Project name: %s", app_desc->project_name);
  ESP_LOGI(kTag, "Secure version: %d", app_desc->secure_version);
  ESP_LOGI(kTag, "App compile time: %s %s", app_desc->time, app_desc->date);
  ESP_LOGI(kTag, "ESP-IDF version: %s", app_desc->idf_ver);
  ESP_LOGI(kTag, "ELF SHA256: %s", esp_app_get_elf_sha256_str());
}

void print_partition_summary() {
  ESP_LOGI(kTag, "Partition map:");
  esp_partition_iterator_t part_it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  if (!part_it) {
    ESP_LOGW(kTag, "  no partition table entries found");
    return;
  }

  for (esp_partition_iterator_t it = part_it; it;) {
    const esp_partition_t* partition = esp_partition_get(it);
    if (partition) {
      ESP_LOGI(kTag, "  type=%d subtype=%d label=%s offset=0x%08" PRIx32 " size=0x%08" PRIx32,
               partition->type, partition->subtype, partition->label, partition->address, partition->size);
    }
    it = esp_partition_next(it);
    if (!it) {
      break;
    }
  }
  esp_partition_iterator_release(part_it);
}

void print_heap_stats() {
  ESP_LOGI(kTag, "Heap free: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "Heap min-free: %u bytes",
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "Task watermark (current): %u words", uxTaskGetStackHighWaterMark(nullptr));
}

#if LUCE_HAS_NVS
const char* nvs_type_name(nvs_type_t type) {
  switch (type) {
    case NVS_TYPE_U8:
      return "U8";
    case NVS_TYPE_I8:
      return "I8";
    case NVS_TYPE_U16:
      return "U16";
    case NVS_TYPE_I16:
      return "I16";
    case NVS_TYPE_U32:
      return "U32";
    case NVS_TYPE_I32:
      return "I32";
    case NVS_TYPE_U64:
      return "U64";
    case NVS_TYPE_I64:
      return "I64";
    case NVS_TYPE_STR:
      return "STR";
    case NVS_TYPE_BLOB:
      return "BLOB";
    default:
      return "OTHER";
  }
}
void dump_nvs_value(nvs_handle_t handle, const nvs_entry_info_t& info) {
  switch (info.type) {
    case NVS_TYPE_U8: {
      uint8_t v = 0;
      if (nvs_get_u8(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u8): %u", v);
      }
      break;
    }
    case NVS_TYPE_I8: {
      int8_t v = 0;
      if (nvs_get_i8(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i8): %d", v);
      }
      break;
    }
    case NVS_TYPE_U16: {
      uint16_t v = 0;
      if (nvs_get_u16(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u16): %u", v);
      }
      break;
    }
    case NVS_TYPE_I16: {
      int16_t v = 0;
      if (nvs_get_i16(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i16): %d", v);
      }
      break;
    }
    case NVS_TYPE_U32: {
      uint32_t v = 0;
      if (nvs_get_u32(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u32): %lu", (unsigned long)v);
      }
      break;
    }
    case NVS_TYPE_I32: {
      int32_t v = 0;
      if (nvs_get_i32(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i32): %ld", (long)v);
      }
      break;
    }
    case NVS_TYPE_U64: {
      uint64_t v = 0;
      if (nvs_get_u64(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u64): 0x%016" PRIx64, v);
      }
      break;
    }
    case NVS_TYPE_I64: {
      int64_t v = 0;
      if (nvs_get_i64(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i64): %lld", (long long)v);
      }
      break;
    }
    case NVS_TYPE_STR: {
      size_t required = 0;
      if (nvs_get_str(handle, info.key, nullptr, &required) == ESP_OK && required > 0) {
        char str_val[129] = {0};
        size_t capacity = required < sizeof(str_val) ? required : sizeof(str_val);
        if (capacity > 0) {
          if (nvs_get_str(handle, info.key, str_val, &capacity) == ESP_OK) {
            ESP_LOGI(kTag, "    value (str): %s", str_val);
          }
        }
      }
      break;
    }
    case NVS_TYPE_BLOB: {
      size_t required = 0;
      if (nvs_get_blob(handle, info.key, nullptr, &required) == ESP_OK && required > 0) {
        char blob_preview[33] = {0};
        if (required > 0) {
          uint8_t data[32];
          size_t copy_size = required < sizeof(data) ? required : sizeof(data);
          if (nvs_get_blob(handle, info.key, data, &copy_size) == ESP_OK) {
            for (size_t i = 0; i < copy_size; ++i) {
              std::snprintf(blob_preview + (i * 2), 3, "%02x", data[i]);
            }
            ESP_LOGI(kTag, "    value (blob, %u bytes): %s%s", (unsigned)required,
                     blob_preview, required > sizeof(data) ? "..." : "");
          }
        }
      }
      break;
    }
    default:
      ESP_LOGW(kTag, "    value unsupported for type=%s", nvs_type_name(info.type));
      break;
  }
}

void dump_nvs_entries() {
  nvs_iterator_t iterator = nullptr;
  esp_err_t err = nvs_entry_find(nullptr, nullptr, NVS_TYPE_ANY, &iterator);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "NVS: no entries found");
    return;
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS entry scan failed: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(kTag, "NVS entries (namespace/key/type/value):");
  nvs_handle_t current = 0;
  std::string current_ns;
  while (true) {
    nvs_entry_info_t entry;
    if (nvs_entry_info(iterator, &entry) != ESP_OK) {
      break;
    }

    if (current_ns != entry.namespace_name) {
      if (current != 0) {
        nvs_close(current);
        current = 0;
      }
      if (nvs_open(entry.namespace_name, NVS_READONLY, &current) != ESP_OK) {
        ESP_LOGW(kTag, "NVS: failed to open namespace '%s'", entry.namespace_name);
        current = 0;
      }
      current_ns.assign(entry.namespace_name);
    }

    ESP_LOGI(kTag, "  ns=%s key=%s type=%s", entry.namespace_name, entry.key,
             nvs_type_name(entry.type));
    if (current != 0) {
      dump_nvs_value(current, entry);
    }

    err = nvs_entry_next(&iterator);
    if (err != ESP_OK) {
      break;
    }
  }

  if (current != 0) {
    nvs_close(current);
  }
  nvs_release_iterator(iterator);
  if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
    ESP_LOGW(kTag, "NVS entry scan ended with error: %s", esp_err_to_name(err));
  }
}

void update_boot_state_record() {
  ESP_LOGI(kTag, "NVS init: starting");
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "NVS init: nvs_flash_init returned %s; erasing and retrying", esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }

  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS init failed: %s", esp_err_to_name(err));
    return;
  }

  nvs_handle_t handle;
  err = nvs_open("boot", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS open failed: %s", esp_err_to_name(err));
    return;
  }

  uint32_t boot_count = 0;
  err = nvs_get_u32(handle, "boot_count", &boot_count);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    boot_count = 0;
  } else if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS get boot_count failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  boot_count += 1;
  err = nvs_set_u32(handle, "boot_count", boot_count);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS set boot_count failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  const uint32_t reset_reason = static_cast<uint32_t>(esp_reset_reason());
  err = nvs_set_u32(handle, "last_reset_reason", reset_reason);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS set last_reset_reason failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  err = nvs_commit(handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS commit failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }
  nvs_close(handle);

  ESP_LOGI(kTag, "NVS state: boot_count=%lu last_reset_reason=%lu", (unsigned long)boot_count,
           (unsigned long)reset_reason);
  dump_nvs_entries();
}
#endif  // LUCE_HAS_NVS

#if LUCE_HAS_I2C
constexpr i2c_port_t kI2CPort = I2C_NUM_0;
constexpr gpio_num_t kI2CSclPin = GPIO_NUM_22;
constexpr gpio_num_t kI2CSdaPin = GPIO_NUM_23;
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

constexpr bool kRelayActiveHigh = false;
constexpr uint8_t kRelayOffValue = kRelayActiveHigh ? 0x00 : 0xFF;
constexpr uint8_t kI2CSampleAddressStart = 0x08;
constexpr uint8_t kI2CSampleAddressEnd = 0x77;
constexpr uint8_t kButtonDebounceThreshold = 3;
constexpr TickType_t kRelayStepDelay = pdMS_TO_TICKS(250);
constexpr TickType_t kButtonSamplePeriod = pdMS_TO_TICKS(40);
constexpr TickType_t kIntSamplePeriod = pdMS_TO_TICKS(200);

struct Mcp23017State {
  bool connected = false;
  uint8_t relay_mask = kRelayOffValue;
};

bool g_i2c_initialized = false;
bool g_mcp_available = false;
uint8_t g_relay_mask = kRelayOffValue;
uint8_t g_button_mask = 0x00;
#if LUCE_HAS_LCD
bool g_lcd_present = false;
#endif

struct I2cScanResult {
  bool mcp = false;
  bool lcd = false;
  int found_count = 0;
};

I2cScanResult scan_i2c_bus();

constexpr uint8_t kLcdAddress = 0x27;

#if LUCE_HAS_LCD
// Common I2C backpack mapping assumed:
// P0=RS, P1=RW, P2=EN, P3=BL, P4=DB4, P5=DB5, P6=DB6, P7=DB7
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
    size_t text_len = 0;
    if (text) {
      text_len = std::strlen(text);
      if (text_len > kLcdCols) {
        text_len = kLcdCols;
      }
    }
    for (size_t idx = 0; idx < kLcdCols; ++idx) {
      if (idx < text_len) {
        safe_text[idx] = text[idx];
      } else {
        safe_text[idx] = ' ';
      }
    }
    return write_line(row, safe_text);
  }

  bool write_text(uint8_t row, const char* text) {
    return write_text_line(row, text);
  }

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
    for (size_t idx = 0; idx < kLcdCols; ++idx) {
      padded[idx] = ' ';
    }
    if (text) {
      for (size_t idx = 0; idx < kLcdCols && text[idx] != '\0'; ++idx) {
        padded[idx] = text[idx];
      }
    }

    if (!set_cursor(row, 0)) {
      return false;
    }

    for (uint8_t idx = 0; idx < kLcdCols; ++idx) {
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
#endif  // LUCE_HAS_LCD

esp_err_t i2c_probe_device(uint8_t address, TickType_t timeout_ticks = pdMS_TO_TICKS(20)) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (!cmd) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = i2c_master_start(cmd);
  if (err == ESP_OK) {
    err = i2c_master_write_byte(cmd, static_cast<uint8_t>((address << 1) | I2C_MASTER_WRITE),
                                true);
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

uint8_t relay_mask_for_channel(int channel) {
  const uint8_t bit = static_cast<uint8_t>(1u << channel);
  return kRelayActiveHigh ? bit : static_cast<uint8_t>(kRelayOffValue & ~bit);
}

esp_err_t mcp_write_reg(uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  return i2c_master_write_to_device(kI2CPort, kMcpAddress, payload, sizeof(payload),
                                   pdMS_TO_TICKS(100));
}

esp_err_t mcp_read_reg(uint8_t reg, uint8_t* value) {
  if (!value) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_master_write_read_device(kI2CPort, kMcpAddress, &reg, sizeof(reg), value,
                                     sizeof(*value), pdMS_TO_TICKS(100));
}

void update_lcd_status() {
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  if (!g_lcd_present) {
    return;
  }
  if (!g_mcp_available) {
    g_lcd.write_status_lines(kRelayOffValue, g_button_mask);
    return;
  }
  g_lcd.write_status_lines(g_relay_mask, g_button_mask);
#endif
#endif
}

bool init_i2c() {
  // Contract:
  // - ok=true only after successful i2c_param_config() and driver install.
  // - on failure, g_i2c_initialized must remain false and caller may retry.
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
    ESP_LOGI(kTag, "%s: summary found=%d mcp=%d lcd=%d", context, scan.found_count, scan.mcp,
             scan.lcd);
  }
#if LUCE_HAS_LCD
  if (attach_lcd) {
    g_lcd_present = false;
  }
#endif
  if (!attach_lcd) {
    return init_result_success();
  }

#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  if (scan.lcd) {
    const bool lcd_present = g_lcd.begin();
    if (!lcd_present) {
      ESP_LOGW(kTag, "LCD detected at 0x%02X but initialization failed; continuing without LCD",
               kLcdAddress);
    } else {
      g_lcd.set_mcp_ok(false);
      g_lcd_present = true;
      ESP_LOGI(kTag, "LCD initialized at 0x%02X", kLcdAddress);
      g_lcd.write_status_lines(kRelayOffValue, 0x00);
    }
  }
#else
  (void)attach_lcd;
#endif
#else
  (void)attach_lcd;
#endif
  return init_result_success();
}

I2cScanResult scan_i2c_bus() {
  I2cScanResult result;

  for (uint8_t addr = kI2CSampleAddressStart; addr <= kI2CSampleAddressEnd; ++addr) {
    if (i2c_probe_device(addr) == ESP_OK) {
      result.found_count += 1;
      result.mcp = result.mcp || (addr == kMcpAddress);
      result.lcd = result.lcd || (addr == kLcdAddress);
      ESP_LOGI(kTag, "I2C found 0x%02X", addr);
    }
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

bool init_mcp23017(Mcp23017State& state) {
  // Contract:
  // - ok=true only when MCP presence probe and all setup writes succeed.
  // - on failure, caller must treat relay/button state as unavailable.
  ESP_LOGI(kTag, "MCP23017 init: start");
  state = {};
  if (i2c_probe_device(kMcpAddress, pdMS_TO_TICKS(100)) != ESP_OK) {
    ESP_LOGW(kTag, "MCP23017 not detected at 0x%02X", kMcpAddress);
    return false;
  }

  constexpr uint8_t kIoconValue = 0x00;
  const esp_err_t errors[] = {
      mcp_write_reg(kIocon, kIoconValue),
      mcp_write_reg(kIodira, 0x00),  // Port A outputs
      mcp_write_reg(kIodirb, 0xFF),  // Port B inputs
      mcp_write_reg(kGppub, 0xFF),   // Pull-ups on buttons
      mcp_write_reg(kGpioa, kRelayOffValue),
      mcp_write_reg(kGppua, 0x00),
      mcp_write_reg(kGpiob, 0x00),
  };

  for (const esp_err_t err : errors) {
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
  ESP_LOGI(kTag, "MCP23017 configured: relays OFF, buttons pullups enabled, IOCON=0x%02X", kIoconValue);
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

#if LUCE_HAS_CLI
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
#endif  // LUCE_HAS_CLI

bool read_button_inputs(uint8_t* value) {
  return mcp_read_reg(kGpiob, value) == ESP_OK;
}

void configure_int_pin() {
  gpio_config_t conf;
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

void run_stage2_diagnostics() {
  log_heap_integrity("run_stage2_diagnostics_enter");
  I2cScanResult scan{};
  const InitPathResult scan_result = run_i2c_scan_flow(scan, nullptr, true);
  if (!scan_result.ok) {
    ESP_LOGW(kTag, "Stage2 abort: I2C bus init failed");
    return;
  }
  g_i2c_initialized = true;
  bool lcd_present = false;

#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  g_lcd_present = scan_result.ok && g_lcd_present;
  lcd_present = g_lcd_present;
#else
  (void)scan;
  (void)lcd_present;
#endif
#else
  (void)scan;
  (void)lcd_present;
#endif

  Mcp23017State mcp_state;
  if (!init_mcp23017(mcp_state)) {
    ESP_LOGW(kTag, "Stage2 diagnostics degraded: MCP23017 missing or unresponsive");
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
    if (lcd_present) {
      g_lcd.write_status_lines(kRelayOffValue, 0x00);
    }
#endif
#endif
    g_mcp_available = false;
    g_relay_mask = kRelayOffValue;
    g_button_mask = 0x00;
    update_lcd_status();
    while (true) {
      static TickType_t last_status_tick = 0;
      const TickType_t now = xTaskGetTickCount();
      if ((now - last_status_tick) >= pdMS_TO_TICKS(2000)) {
        last_status_tick = now;
        const uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
        log_runtime_status_line(uptime_s, true, false, kRelayOffValue, 0x00);
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
      if (lcd_present && !g_lcd.write_status_lines(kRelayOffValue, 0x00)) {
        ESP_LOGW(kTag, "LCD update failed (LCD-only mode)");
      }
#else
      (void)lcd_present;
#endif
#endif
      }

      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  configure_int_pin();
  g_mcp_available = mcp_state.connected;
  g_relay_mask = mcp_state.relay_mask;
  g_button_mask = 0x00;

  if (set_relay_mask(mcp_state, kRelayOffValue) != ESP_OK) {
    ESP_LOGW(kTag, "Stage2 diagnostics degraded: cannot write initial relay state");
    g_mcp_available = false;
    return;
  }


#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  g_lcd.set_mcp_ok(scan.mcp);
  if (lcd_present) {
    g_lcd.write_status_lines(mcp_state.relay_mask, 0x00);
  }
#endif
#endif

  uint8_t debounce_counts[8] = {0};
  uint8_t debounced_buttons = 0x00;
  uint8_t last_reported_buttons = 0x00;
  uint8_t current_buttons = 0x00;
  uint8_t channel = 0;
  uint8_t relay_mask = kRelayOffValue;
  uint8_t button_mask = 0x00;
  bool have_button_snapshot = false;
  TickType_t last_relay_tick = 0;
  TickType_t last_button_tick = 0;
  TickType_t last_int_tick = 0;
  TickType_t last_status_tick = 0;
  int last_int_level = gpio_get_level(kMcpIntPin);

  ESP_LOGI(kTag,
           "Entering Stage2 runtime diagnostics loop (1 ch at a time relay sweep + button sample)");
  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if ((now - last_relay_tick) >= kRelayStepDelay) {
      relay_mask = relay_mask_for_channel(channel);
      if (set_relay_mask(mcp_state, relay_mask) == ESP_OK) {
        ESP_LOGI(kTag, "Relay channel=%d sweep mask=0x%02X", static_cast<int>(channel), relay_mask);
        g_relay_mask = relay_mask;
      }
      channel = static_cast<uint8_t>((channel + 1) % 8);
      last_relay_tick = now;
    }

    if ((now - last_button_tick) >= kButtonSamplePeriod) {
      last_button_tick = now;
      if (read_button_inputs(&current_buttons)) {
        if (!have_button_snapshot) {
          debounced_buttons = current_buttons;
          last_reported_buttons = current_buttons;
          button_mask = debounced_buttons;
          have_button_snapshot = true;
          ESP_LOGI(kTag, "Button init mask: 0x%02X (1=pressed? check wiring polarity)", current_buttons);
        } else {
          bool changed = false;
          for (uint8_t bit = 0; bit < 8; ++bit) {
            bool raw = (current_buttons & (1u << bit)) != 0;
            bool stable = (debounced_buttons & (1u << bit)) != 0;

            if (raw == stable) {
              debounce_counts[bit] = 0;
              continue;
            }

            if (++debounce_counts[bit] >= kButtonDebounceThreshold) {
              debounced_buttons ^= (1u << bit);
              debounce_counts[bit] = 0;
              changed = true;
            }
          }
          if (changed && debounced_buttons != last_reported_buttons) {
            button_mask = debounced_buttons;
            g_button_mask = debounced_buttons;
            ESP_LOGI(kTag, "Button mask change: 0x%02X", debounced_buttons);
            last_reported_buttons = debounced_buttons;
          }
        }
      } else {
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

    if ((now - last_status_tick) >= pdMS_TO_TICKS(2000)) {
      last_status_tick = now;
      const uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
      log_runtime_status_line(uptime_s, true, true, relay_mask, button_mask);
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
      if (lcd_present) {
        if (!g_lcd.write_status_lines(relay_mask, button_mask)) {
          ESP_LOGW(kTag, "LCD update failed");
        }
      }
#endif
#endif
    }

    log_stage4_watermarks("diag_loop");
    if ((now - last_status_tick) >= pdMS_TO_TICKS(10000)) {
      log_heap_integrity("diag_loop_health");
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#if LUCE_HAS_CLI
void cli_trim(char* line) {
  char* write = line;
  for (const char* read = line; *read != '\0'; ++read) {
    if (*read == '\r' || *read == '\n') {
      continue;
    }
    if (write != read) {
      *write = *read;
    }
    ++write;
  }
  *write = '\0';
}

bool parse_u32_with_base(const char* text, int base, uint32_t* value, char* token_context = nullptr) {
  if (!text || !*text || !value) {
    return false;
  }
  if (token_context) {
    std::strncpy(token_context, text, 31);
    token_context[31] = '\0';
  }

  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text, &end, base);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *value = static_cast<uint32_t>(parsed);
  return true;
}

void log_cli_arguments(const char* command, int argc, char* argv[]) {
  ESP_LOGI(kTag, "CLI cmd='%s' argc=%d", command, argc);
  for (int i = 0; i < argc; ++i) {
    ESP_LOGI(kTag, "CLI arg[%d]='%s'", i, argv[i]);
  }
}

void cli_print_help() {
  ESP_LOGI(kTag, "CLI commands: help, status, nvs_dump, i2c_scan, mcp_read, relay_set, relay_mask, buttons, lcd_print, reboot");
  ESP_LOGI(kTag, "  - mcp_read <gpioa|gpiob>");
  ESP_LOGI(kTag, "  - relay_set <0..7> <0|1>");
  ESP_LOGI(kTag, "  - relay_mask <hex> (8-bit register value)");
  ESP_LOGI(kTag, "  - buttons");
  ESP_LOGI(kTag, "  - lcd_print <text> (if LCD enabled)");
}

void cli_cmd_status() {
  log_status_health_lines();
}

void cli_cmd_nvs_dump() {
#if LUCE_HAS_NVS
  ESP_LOGI(kTag, "CLI command nvs_dump: executing");
  dump_nvs_entries();
  ESP_LOGI(kTag, "CLI command nvs_dump: done");
#else
  ESP_LOGW(kTag, "CLI command nvs_dump: unsupported (LUCE_HAS_NVS=0)");
#endif
}

void cli_cmd_i2c_scan() {
#if LUCE_HAS_I2C
  I2cScanResult scan{};
  const InitPathResult scan_result = run_i2c_scan_flow(scan, "CLI command i2c_scan", false);
  g_i2c_initialized = scan_result.ok;
  if (!scan_result.ok) {
    ESP_LOGW(kTag, "CLI command i2c_scan: summary unavailable (scan not initialized)");
  }
#else
  ESP_LOGW(kTag, "CLI command i2c_scan: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_mcp_read(char* port) {
#if LUCE_HAS_I2C
  if (!g_mcp_available) {
    ESP_LOGW(kTag, "CLI command mcp_read: MCP unavailable");
    return;
  }
  uint8_t value = 0x00;
  const uint8_t reg = (std::strcmp(port, "gpioa") == 0 || std::strcmp(port, "a") == 0) ? kGpioa
                                                                                       : kGpiob;
  const esp_err_t err = mcp_read_reg(reg, &value);
  ESP_LOGI(kTag, "CLI command mcp_read %s rc=%s value=0x%02X", port, esp_err_to_name(err), value);
#else
  (void)port;
  ESP_LOGW(kTag, "CLI command mcp_read: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_relay_set(int channel, int on_off) {
#if LUCE_HAS_I2C
  const uint8_t new_mask = relay_mask_for_channel_state(channel, on_off != 0, g_relay_mask);
  const esp_err_t err = set_relay_mask_safe(new_mask);
  if (err == ESP_OK) {
    g_relay_mask = new_mask;
  }
  ESP_LOGI(kTag, "CLI command relay_set: ch=%d value=%d new_mask=0x%02X rc=%s", channel, on_off, new_mask,
           esp_err_to_name(err));
#else
  (void)channel;
  (void)on_off;
  ESP_LOGW(kTag, "CLI command relay_set: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_relay_mask(uint32_t value) {
#if LUCE_HAS_I2C
  const uint8_t mask = static_cast<uint8_t>(value & 0xFF);
  const esp_err_t err = set_relay_mask_safe(mask);
  if (err == ESP_OK) {
    g_relay_mask = mask;
  }
  ESP_LOGI(kTag, "CLI command relay_mask: mask=0x%02X rc=%s", mask, esp_err_to_name(err));
#else
  (void)value;
  ESP_LOGW(kTag, "CLI command relay_mask: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_buttons() {
#if LUCE_HAS_I2C
  if (!g_mcp_available) {
    ESP_LOGW(kTag, "CLI command buttons: MCP unavailable");
    return;
  }
  uint8_t value = 0x00;
  const esp_err_t err = mcp_read_reg(kGpiob, &value);
  ESP_LOGI(kTag, "CLI command buttons: rc=%s gpiob=0x%02X", esp_err_to_name(err), value);
#else
  ESP_LOGW(kTag, "CLI command buttons: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_lcd_print(char* text) {
#if LUCE_HAS_LCD
  if (!g_lcd_present) {
    ESP_LOGW(kTag, "CLI command lcd_print: LCD not initialized or absent");
    return;
  }
  if (!text || !*text) {
    ESP_LOGW(kTag, "CLI command lcd_print: missing text argument");
    return;
  }
  const bool ok = g_lcd.write_text(0, text);
  ESP_LOGI(kTag, "CLI command lcd_print: rc=%s text='%s'", ok ? "OK" : "ERR", text);
#else
  (void)text;
  ESP_LOGW(kTag, "CLI command lcd_print: unsupported (LUCE_HAS_LCD=0)");
#endif
}

void cli_task(void*) {
  uart_config_t uart_cfg{};
  uart_cfg.baud_rate = 115200;
  uart_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.parity = UART_PARITY_DISABLE;
  uart_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_cfg.source_clk = UART_SCLK_APB;
  esp_err_t err = uart_param_config(UART_NUM_0, &uart_cfg);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "CLI uart_param_config failed: %s", esp_err_to_name(err));
  }
  err = uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "CLI uart_set_pin failed: %s", esp_err_to_name(err));
  }
  err = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "CLI uart_driver_install failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(kTag, "CLI listening on UART0 at 115200. Type 'help' for commands.");
  }
  log_stage4_watermarks("cli_start");
  log_heap_integrity("cli_start");

  char line_buffer[128] = {0};
  size_t line_len = 0;
  char ch;
  while (true) {
    const int read = uart_read_bytes(UART_NUM_0, reinterpret_cast<uint8_t*>(&ch), 1, pdMS_TO_TICKS(200));
    if (read <= 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (ch == '\r' || ch == '\n') {
      if (line_len == 0) {
        continue;
      }
      line_buffer[line_len] = '\0';

      char command_buffer[128];
      std::memcpy(command_buffer, line_buffer, sizeof(command_buffer));
      cli_trim(command_buffer);
      line_len = 0;

      char* argv[8];
      char* next_token = nullptr;
      int argc = 0;
      char* token = strtok_r(command_buffer, " \t", &next_token);
      if (!token) {
        continue;
      }
      while (token && argc < 8) {
        argv[argc++] = token;
        token = strtok_r(nullptr, " \t", &next_token);
      }
      log_cli_arguments(argv[0], argc, argv);
      log_heap_integrity("cli_pre_cmd");
      log_stage4_watermarks("cli_pre_cmd");

      if (std::strcmp(argv[0], "help") == 0) {
        cli_print_help();
      } else if (std::strcmp(argv[0], "status") == 0) {
        cli_cmd_status();
      } else if (std::strcmp(argv[0], "nvs_dump") == 0) {
        cli_cmd_nvs_dump();
      } else if (std::strcmp(argv[0], "i2c_scan") == 0) {
        cli_cmd_i2c_scan();
      } else if (std::strcmp(argv[0], "mcp_read") == 0) {
        if (argc != 2) {
          ESP_LOGW(kTag, "CLI command mcp_read usage: mcp_read <gpioa|gpiob>");
        } else if (std::strcmp(argv[1], "gpioa") == 0 || std::strcmp(argv[1], "A") == 0 ||
                   std::strcmp(argv[1], "gpiob") == 0 || std::strcmp(argv[1], "B") == 0) {
          cli_cmd_mcp_read(argv[1]);
        } else {
          ESP_LOGW(kTag, "CLI command mcp_read: invalid port '%s'", argv[1]);
        }
      } else if (std::strcmp(argv[0], "relay_set") == 0) {
        if (argc != 3) {
          ESP_LOGW(kTag, "CLI command relay_set usage: relay_set <0..7> <0|1>");
        } else {
          uint32_t channel = 0;
          uint32_t value = 0;
          char tmp1[32] = {0};
          char tmp2[32] = {0};
          const bool ok1 = parse_u32_with_base(argv[1], 10, &channel, tmp1);
          const bool ok2 = parse_u32_with_base(argv[2], 10, &value, tmp2);
          if (!ok1 || !ok2 || value > 1 || channel > 7) {
            ESP_LOGW(kTag,
                     "CLI command relay_set: parse error or out-of-range (channel=%s value=%s)",
                     tmp1, tmp2);
          } else {
            cli_cmd_relay_set(static_cast<int>(channel), static_cast<int>(value));
          }
        }
      } else if (std::strcmp(argv[0], "relay_mask") == 0) {
        if (argc != 2) {
          ESP_LOGW(kTag, "CLI command relay_mask usage: relay_mask <hex>");
        } else {
          uint32_t value = 0;
          char tmp[32] = {0};
          if (!parse_u32_with_base(argv[1], 16, &value, tmp) || value > 0xFF) {
            ESP_LOGW(kTag, "CLI command relay_mask: parse error for '%s'", tmp);
          } else {
            cli_cmd_relay_mask(value);
          }
        }
      } else if (std::strcmp(argv[0], "buttons") == 0) {
        cli_cmd_buttons();
      } else if (std::strcmp(argv[0], "lcd_print") == 0) {
        if (argc < 2) {
          ESP_LOGW(kTag, "CLI command lcd_print usage: lcd_print <text>");
        } else {
          char text[128] = {0};
          std::snprintf(text, sizeof(text), "%s", argv[1]);
          for (int i = 2; i < argc; ++i) {
            const std::size_t used = std::strlen(text);
            const std::size_t next_len = std::strlen(argv[i]);
            if (used + 1 + next_len >= sizeof(text)) {
              ESP_LOGW(kTag, "CLI command lcd_print: truncated due to output length limit");
              break;
            }
            text[used] = ' ';
            text[used + 1] = '\0';
            std::strncat(text, argv[i], sizeof(text) - std::strlen(text) - 1);
          }
          cli_cmd_lcd_print(text);
        }
      } else if (std::strcmp(argv[0], "reboot") == 0) {
        ESP_LOGW(kTag, "CLI command reboot: restarting");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
      } else {
        ESP_LOGW(kTag, "CLI unknown command '%s'", argv[0]);
        cli_print_help();
      }
      std::memset(line_buffer, 0, sizeof(line_buffer));
      log_heap_integrity("cli_post_cmd");
      log_stage4_watermarks("cli_post_cmd");
      continue;
    }

    if ((ch == '\b' || ch == 0x7F) && line_len > 0) {
      --line_len;
      line_buffer[line_len] = '\0';
      continue;
    }
    if (line_len + 1 < sizeof(line_buffer)) {
      line_buffer[line_len++] = ch;
    } else {
      ESP_LOGW(kTag, "CLI input overflow, dropping current command");
      line_len = 0;
      std::memset(line_buffer, 0, sizeof(line_buffer));
    }
  }
}

void cli_startup() {
#if LUCE_STAGE4_CLI
  if (xTaskCreate(cli_task, "cli", kCliTaskStackWords, nullptr, 2, &g_cli_task) != pdPASS) {
    ESP_LOGW(kTag, "CLI task create failed");
  } else {
    log_stage4_watermarks("cli_startup");
  }
#else
  ESP_LOGW(kTag, "CLI task creation disabled by LUCE_STAGE4_CLI=%d", LUCE_STAGE4_CLI);
#endif
}

#endif  // LUCE_HAS_CLI

#if LUCE_HAS_CLI
void diagnostics_task(void*) {
  run_stage2_diagnostics();
}
#endif  // LUCE_HAS_CLI

void blink_alive_task(void*) {
  blink_alive();
}

#endif  // LUCE_HAS_I2C

void blink_alive() {
  const gpio_num_t leds[] = {GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27};

  for (gpio_num_t pin : leds) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
  }

  while (true) {
    for (size_t idx = 0; idx < 3; ++idx) {
      for (size_t j = 0; j < 3; ++j) {
        gpio_set_level(leds[j], j == idx ? 1 : 0);
      }
      vTaskDelay(pdMS_TO_TICKS(160));
    }
    for (size_t j = 0; j < 3; ++j) {
      gpio_set_level(leds[j], 0);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

extern "C" void app_main(void) {
  log_startup_banner();

  print_chip_info();
  print_heap_stats();
  print_app_info();
  print_partition_summary();

  ESP_LOGI(kTag, "Feature flags: NVS=%d I2C=%d LCD=%d CLI=%d",
           LUCE_HAS_NVS, LUCE_HAS_I2C, LUCE_HAS_LCD, LUCE_HAS_CLI);

#if LUCE_HAS_NVS
  update_boot_state_record();
#endif

#if LUCE_HAS_I2C
  xTaskCreate(blink_alive_task, "blink", kBlinkTaskStackWords, nullptr, 1, nullptr);
#if LUCE_HAS_CLI
#if LUCE_STAGE4_DIAG
  if (xTaskCreate(diagnostics_task, "diag", kDiagTaskStackWords, nullptr, 1, &g_diag_task) != pdPASS) {
    ESP_LOGW(kTag, "Stage4 diagnostic task create failed");
  }
#else
  ESP_LOGW(kTag, "Diagnostics task disabled by LUCE_STAGE4_DIAG=%d", LUCE_STAGE4_DIAG);
#endif
#if LUCE_STAGE4_CLI
  cli_startup();
#else
  ESP_LOGW(kTag, "CLI startup disabled by LUCE_STAGE4_CLI=%d", LUCE_STAGE4_CLI);
#endif
  for (;;) {
    log_stage4_watermarks("stage4_main_wait");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#else
  run_stage2_diagnostics();
  ESP_LOGW(kTag, "Stage2 diagnostics loop exited; staying in blink-only fallback");
#endif
#endif

  blink_alive();
}
