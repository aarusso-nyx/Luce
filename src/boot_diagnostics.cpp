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
#include "esp_private/esp_clk.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

void luce_log_startup_banner() {
  char reason_line[48];
  luce_init_path_reset_reason_line(reason_line, sizeof(reason_line), esp_reset_reason());

  ESP_LOGI(kTag, "LUCE STAGE%d", LUCE_STAGE);
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
