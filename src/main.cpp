#include <cinttypes>
#include <cstdio>

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
#include "luce/cli_tcp.h"
#include "luce/net_wifi.h"
#include "luce/ntp.h"
#include "luce/mdns.h"
#include "luce/mqtt.h"
#include "luce/http_server.h"
#include "luce/i2c_io.h"
#include "luce/ota.h"
#include "luce/led_status.h"

namespace {

constexpr const char* kTag = "luce_boot";
}  // namespace

extern "C" void app_main(void) {
  luce_log_startup_banner();
  luce_print_chip_info();
  luce_print_heap_stats();
  luce_print_app_info();
  luce_print_partition_summary();
  luce_print_feature_flags();

  update_boot_state_record();

  led_status_startup();
  io_startup();

  cli_startup();

#if LUCE_HAS_WIFI
  wifi_startup();
#endif

#if LUCE_HAS_NTP
  ntp_startup();
#endif

#if LUCE_HAS_MDNS
  mdns_startup();
#endif

#if LUCE_HAS_TCP_CLI
  cli_net_startup();
#endif

#if LUCE_HAS_MQTT
  mqtt_startup();
#endif

#if LUCE_HAS_HTTP
  http_startup();
#endif

#if LUCE_HAS_OTA
  ota_startup();
#endif

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
