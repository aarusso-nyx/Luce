#include <esp_vfs_littlefs.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <lwip/apps/sntp.h>
#include <mdns.h>
#include <freertos/event_groups.h>
#include <nvs_flash.h>
#include <driver/uart.h>
#include <driver/i2c.h>
#include <driver/gpio.h>
#include <esp_err.h>

#include "cli.h"
#include "util.h"
#include "eventBus.h"
#include "http.h"
#include "lcd.h"
#include "leds.h"
#include "logger.h"
#include "mqtt.h"
#include "ota.h"
#include "relays.h"
#include "settings.h"
#include "network.h"

#include "tasks.hpp"

// Task wrappers: return bool to indicate continuation
// LCD task wrappers using free-function API
static bool lcdInitTask()   { return lcdInit(); }
// LCD main loop: return false to terminate on error
static bool lcdLoopTask()   { return lcdLoop(); }
// Relay hardware initialization and polling uses relaysInit/relaysLoop directly


// Network init relocated to network.cpp (include network.h)
#include "network.h"

extern "C" void app_main() {
  // Initialize NVS
  ESP_ERROR_CHECK(nvs_flash_init());

  // Initialize UART0 for CLI and logging
  uart_config_t uart_cfg = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity    = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
  };
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_cfg));
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
  // Initialize I2C master for external devices (LCD, MCP23X17)
  {
    i2c_config_t i2c_conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = config::I2C_SDA,
      .scl_io_num = config::I2C_SCL,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master = { .clk_speed = config::I2C_FREQ }
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));
  }
  // Allow UART driver to initialize
  vTaskDelay(pdMS_TO_TICKS(1000));
 
  // Mount LittleFS file system via VFS
  {
    esp_vfs_littlefs_conf_t lfs_conf = {
      .base_path = "/littlefs",
      .partition_label = NULL,
      .format_if_mount_failed = true,
      .dont_mount = false,
      .max_files = 5,
      .cache_size = 64
    };
    esp_err_t err = esp_vfs_littlefs_register(&lfs_conf);
    if (err != ESP_OK) {
      LOGERR("FS","Mount","Failed to mount LittleFS (%s)", esp_err_to_name(err));
      utilSafePanic();
    }
  }

  Logger::begin();
  // Serial logging removed; logs are output via ESP-IDF logger over UART0

  Settings.begin();

  EventBus::init();

  // Broadcast and publish sensor data using unified struct
  EventBus::subscribe(EventType::EVT_SENSOR, [](const BusEvent& ev){
    broadcastSensorData(ev.data.sensor);
    publishSensorData(  ev.data.sensor);
  });

  EventBus::subscribe(EventType::EVT_RELAY, [](const BusEvent& ev){
    const RelayEventData &r = ev.data.relay;
    char sub[32];
    snprintf(sub, sizeof(sub), "relays/state/%u", (unsigned)r.idx);
    mqttPublish(sub, r.state ? 1 : 0);
    broadcastRelayEvent(r.idx, r.state);
  });
  
  Leds.begin();

  // Start CLI Task
  tasks::Task<cliInit, cliLoop, 10>::create("CLI",3);

  // Start LCD task (initialization + periodic refresh)
  tasks::Task<lcdInitTask, lcdLoopTask, config::DISPLAY_UPDATE_MS>::create("LCD", 4);
  
  // Initialize relay hardware and start polling
  tasks::Task<relaysInit, relaysLoop, 10>::create("Relay",7);
  
  // Initialize network settings
  if (networkInit(Settings.getName())) {
    // MQTT task (disabled if mqtt config is "0" or empty)
    if (Settings.hasMqtt()) {
      tasks::Task<mqttInit, mqttLoop, 10>::create("MQTT",6);
    }
    
    // HTTP server task
    // tasks::Task<httpInit, httpLoop, 10>::create("HTTP",5);
    
    // Start Telnet server task
    tasks::Task<cliServer, cliTelnet, 10>::create("Telnet", 2);

    // OTA update service task
    tasks::Task<otaInit, otaLoop, 10>::create("OTA",1);  
  }
}

