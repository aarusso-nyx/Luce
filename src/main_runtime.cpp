#include <cinttypes>
#include <cstdio>

#include "luce/runtime.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "luce/boot_diagnostics.h"
#include "luce/boot_state.h"
#include "luce/cli.h"
#include "luce/net_wifi.h"
#include "luce/ntp.h"

#if LUCE_HAS_I2C
#include "luce/stage2_io.h"
#endif

namespace {

constexpr const char* kTag = "luce_boot";

constexpr std::size_t kBlinkTaskStackWords = 2048;

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

void blink_alive_task(void*) {
  blink_alive();
}

#if LUCE_HAS_I2C
void stage2_task(void*) {
  run_stage2_diagnostics();
}
#endif

}  // namespace

void luce_runtime_main(void) {
  luce_log_startup_banner();
  luce_print_chip_info();
  luce_print_heap_stats();
  luce_print_app_info();
  luce_print_partition_summary();
  luce_print_feature_flags();

#if LUCE_HAS_NVS
  update_boot_state_record();
#endif

#if LUCE_HAS_I2C
  if (xTaskCreate(blink_alive_task, "blink", kBlinkTaskStackWords, nullptr, 1, nullptr) != pdPASS) {
    ESP_LOGW(kTag, "Failed to create LED blink task");
  }
  if (xTaskCreate(stage2_task, "stage2", 8192, nullptr, 1, nullptr) != pdPASS) {
    ESP_LOGW(kTag, "Failed to create Stage2 diagnostics task");
  }
  cli_startup();
  wifi_startup();
  ntp_startup();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#else
  blink_alive();
#endif
}
