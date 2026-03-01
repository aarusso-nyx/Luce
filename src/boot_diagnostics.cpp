#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "luce/boot_diagnostics.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if LUCE_HAS_I2C
#include "luce/i2c_io.h"
#endif

constexpr const char* kTag = "luce_boot";

const char* luce_reset_reason_to_string(esp_reset_reason_t reason) {
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

std::size_t luce_init_path_reset_reason_line(char* out, std::size_t out_size,
                                            esp_reset_reason_t reason) {
  return std::snprintf(out, out_size, "%s (%d)", luce_reset_reason_to_string(reason),
                       static_cast<int>(reason));
}

void luce_log_heap_integrity(const char* context) {
#if LUCE_HAS_CLI
  if (!context) {
    context = "unknown";
  }
  const bool ok = heap_caps_check_integrity_all(true);
  ESP_LOGI(kTag, "Heap integrity (%s): %s", context, ok ? "OK" : "CORRUPTED");
#else
  (void)context;
#endif
}

void luce_log_startup_banner() {
  char reason_line[48];
  luce_init_path_reset_reason_line(reason_line, sizeof(reason_line), esp_reset_reason());

  ESP_LOGI(kTag, "LUCE STRATEGY=%s", LUCE_STRATEGY_NAME);
  ESP_LOGI(kTag, "Build timestamp: %s %s", __DATE__, __TIME__);
  ESP_LOGI(kTag, "Project version: %s", LUCE_PROJECT_VERSION);
  ESP_LOGI(kTag, "Git SHA: %s", LUCE_GIT_SHA);
  ESP_LOGI(kTag, "Reset reason: %s", reason_line);
}

void luce_print_chip_info() {
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

void luce_print_app_info() {
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

void luce_print_partition_summary() {
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
  }
}

void luce_print_heap_stats() {
  ESP_LOGI(kTag, "Heap free: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "Heap min-free: %u bytes",
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "Task watermark (current): %u words", uxTaskGetStackHighWaterMark(nullptr));
}

void luce_print_feature_flags() {
  ESP_LOGI(kTag,
           "Feature flags: NVS=%d I2C=%d LCD=%d CLI=%d WIFI=%d NTP=%d mDNS=%d MQTT=%d HTTP=%d",
           LUCE_HAS_NVS, LUCE_HAS_I2C, LUCE_HAS_LCD, LUCE_HAS_CLI, LUCE_HAS_WIFI,
           LUCE_HAS_NTP, LUCE_HAS_MDNS, LUCE_HAS_MQTT, LUCE_HAS_HTTP);
}

void luce_log_status_health() {
  char reason_line[48];
  std::uint8_t relay_mask = 0;
  std::uint8_t button_mask = 0;

  luce_init_path_reset_reason_line(reason_line, sizeof(reason_line), esp_reset_reason());
  ESP_LOGI(kTag, "status: strategy=%s reset=%s uptime=%llus", LUCE_STRATEGY_NAME, reason_line,
           static_cast<long long>(esp_timer_get_time() / 1000000ULL));
  ESP_LOGI(kTag, "status: heap_free=%u min_free=%u",
           heap_caps_get_free_size(MALLOC_CAP_8BIT),
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag,
           "status: feature i2c=%d lcd=%d cli=%d wifi=%d ntp=%d mdns=%d mqtt=%d http=%d",
           LUCE_HAS_I2C, LUCE_HAS_LCD, LUCE_HAS_CLI, LUCE_HAS_WIFI, LUCE_HAS_NTP, LUCE_HAS_MDNS,
           LUCE_HAS_MQTT, LUCE_HAS_HTTP);

#if LUCE_HAS_I2C
  relay_mask = g_relay_mask;
  button_mask = g_button_mask;
  ESP_LOGI(kTag, "status: i2c_init=%d mcp=%d REL:0x%02X BTN:0x%02X", g_i2c_initialized ? 1 : 0,
           g_mcp_available ? 1 : 0, relay_mask, button_mask);
#else
  ESP_LOGI(kTag, "status: i2c_init=0 mcp=0 REL:0x%02X BTN:0x%02X", relay_mask, button_mask);
#endif
}

void luce_log_runtime_status(std::uint64_t uptime_s, bool i2c_ok, bool mcp_ok,
                            std::uint8_t relay_mask, std::uint8_t button_mask) {
  char mask_line[32] = {0};
  std::snprintf(mask_line, sizeof(mask_line), "REL:0x%02X BTN:0x%02X", relay_mask, button_mask);
  ESP_LOGI(kTag, "LUCE S3 %llu | I2C:%s MCP:%s %s",
           static_cast<unsigned long long>(uptime_s), i2c_ok ? "ok" : "no",
           mcp_ok ? "ok" : "no", mask_line);
}

void luce_log_stage_watermarks(const char* context) {
  const char* used_context = context ? context : "unknown";
  const UBaseType_t now = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGI(kTag, "Stack watermark (%s): current=%u", used_context, now);
}
