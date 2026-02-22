#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "luce_build.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_private/esp_clk.h"
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

namespace {

constexpr const char* kTag = "luce_boot";
void blink_alive();

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
  } else {
    for (esp_partition_iterator_t it = part_it; it; it = esp_partition_next(it)) {
      const esp_partition_t* partition = esp_partition_get(it);
      if (!partition) {
        continue;
      }
      ESP_LOGI(kTag, "  type=%d subtype=%d label=%s offset=0x%08" PRIx32 " size=0x%08" PRIx32,
               partition->type, partition->subtype, partition->label,
               partition->address, partition->size);
    }
    esp_partition_iterator_release(part_it);
  }
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

void scan_i2c_bus() {
  bool found_expected = false;
  int found_count = 0;

  for (uint8_t addr = kI2CSampleAddressStart; addr <= kI2CSampleAddressEnd; ++addr) {
    esp_err_t probe = i2c_probe_device(addr);
    if (probe == ESP_OK) {
      found_expected = found_expected || (addr == kMcpAddress);
      ESP_LOGI(kTag, "I2C found 0x%02X", addr);
      ++found_count;
    }
  }

  if (found_count == 0) {
    ESP_LOGW(kTag, "I2C scan: no devices detected on bus");
  } else {
    ESP_LOGI(kTag, "I2C scan summary: %d device(s) detected", found_count);
  }
  if (found_expected) {
    ESP_LOGI(kTag, "I2C scan expects MCP23017 at 0x20: found");
  } else {
    ESP_LOGW(kTag, "I2C scan expects MCP23017 at 0x20: not found");
  }
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
  ESP_LOGI(kTag, "MCP23017 configured: relays OFF, buttons pullups enabled, IOCON=0x%02X", kIoconValue);
  return true;
}

esp_err_t set_relay_mask(Mcp23017State& state, uint8_t mask) {
  const esp_err_t err = mcp_write_reg(kGpioa, mask);
  if (err == ESP_OK) {
    state.relay_mask = mask;
    ESP_LOGI(kTag, "Relay mask set: 0x%02X", mask);
  }
  return err;
}

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
  if (!init_i2c()) {
    ESP_LOGW(kTag, "Stage2 abort: I2C bus init failed");
    return;
  }

  scan_i2c_bus();

  Mcp23017State mcp_state;
  if (!init_mcp23017(mcp_state)) {
    ESP_LOGW(kTag, "Stage2 diagnostics degraded: MCP23017 missing or unresponsive");
    return;
  }

  configure_int_pin();

  if (set_relay_mask(mcp_state, kRelayOffValue) != ESP_OK) {
    ESP_LOGW(kTag, "Stage2 diagnostics degraded: cannot write initial relay state");
    return;
  }

  uint8_t debounce_counts[8] = {0};
  uint8_t debounced_buttons = kRelayOffValue;  // placeholder replaced by first read
  uint8_t last_reported_buttons = 0x00;
  uint8_t current_buttons = 0x00;
  uint8_t channel = 0;
  bool have_button_snapshot = false;
  TickType_t last_relay_tick = 0;
  TickType_t last_button_tick = 0;
  TickType_t last_int_tick = 0;
  int last_int_level = gpio_get_level(kMcpIntPin);

  ESP_LOGI(kTag, "Entering Stage2 runtime diagnostics loop (1 ch at a time relay sweep + button sample)");
  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if ((now - last_relay_tick) >= kRelayStepDelay) {
      const uint8_t relay_mask = relay_mask_for_channel(channel);
      if (set_relay_mask(mcp_state, relay_mask) == ESP_OK) {
        ESP_LOGI(kTag, "Relay channel=%d sweep mask=0x%02X", static_cast<int>(channel), relay_mask);
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

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

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
  ESP_LOGI(kTag, "LUCE STAGE%d", LUCE_STAGE);
  ESP_LOGI(kTag, "Build timestamp: %s %s", __DATE__, __TIME__);
  ESP_LOGI(kTag, "Project version: %s", LUCE_PROJECT_VERSION);
  ESP_LOGI(kTag, "Git SHA: %s", LUCE_GIT_SHA);
  ESP_LOGI(kTag, "Reset reason: %s (%d)", reset_reason_to_string(esp_reset_reason()),
           static_cast<int>(esp_reset_reason()));

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
  xTaskCreate(blink_alive_task, "blink", 2048, nullptr, 1, nullptr);
  run_stage2_diagnostics();
  ESP_LOGW(kTag, "Stage2 diagnostics loop exited; staying in blink-only fallback");
#endif

  blink_alive();
}
