// config.h: single place for compile-time hardware configuration
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdint>

namespace config {
  // MQTT reconnect policy
  constexpr int MQTT_MAX_RECONNECT_ATTEMPTS = 5;
  constexpr uint32_t MQTT_RECONNECT_BASE_MS = 5000;  // initial backoff in ms
  constexpr uint32_t MQTT_RECONNECT_MAX_MS = 60000; // max backoff in ms
  // I2C bus configuration
  constexpr uint8_t I2C_SDA = 23;
  constexpr uint8_t I2C_SCL = 22;
  constexpr uint32_t I2C_FREQ = 100000; // in Hz

  // MCP23017 (relay expander)
  constexpr uint8_t MCP_I2C_ADDR = 0x20;
  constexpr uint8_t MCP_INTA_PIN = 19;
  constexpr uint8_t RELAY_COUNT = 8;

  // LCD display
  constexpr uint8_t LCD_I2C_ADDR = 0x27;
  constexpr uint8_t LCD_COLS = 20;
  constexpr uint8_t LCD_ROWS = 4;
  constexpr uint8_t LCD_PAGE_COUNT = 5;

  // Status LEDs
  constexpr uint8_t LED_PIN_0 = 25;
  constexpr uint8_t LED_PIN_1 = 26;
  constexpr uint8_t LED_PIN_2 = 27;

  // Sensor pins and types
  constexpr uint8_t DHT_PIN = 13;
  #define DHT_TYPE DHT22
  constexpr uint8_t LDR_PIN = 34;
  constexpr uint8_t VOLT_PIN = 35;

  // Default intervals
  constexpr uint32_t SENSOR_INTERVAL_DEFAULT = 5000; // ms
  constexpr uint32_t DISPLAY_SWAP_MS = 15000;       // ms between auto page flips
  constexpr uint32_t DISPLAY_UPDATE_MS = 1000;      // ms between page redraws

  // CLI and buffer sizes
  constexpr uint16_t TELNET_PORT = 23;
  // HTTP server port
  constexpr uint16_t HTTP_PORT = 80;
  // DNS port for captive portal
  constexpr uint16_t DNS_PORT = 53;
  // Serial CLI watcher polling interval
  constexpr uint32_t SERIAL_WATCHER_DELAY_MS = 10;
  // Night mode timer period (seconds)
  constexpr uint32_t NIGHT_TIMER_SEC = 60;


  constexpr size_t STACK_SIZE = 3072; // default FreeRTOS task stack (words)
  // EventBus queue length
  constexpr size_t EVENT_QUEUE_LENGTH = 10;

  // CLI prompt
  constexpr const char* CLI_PROMPT = "> "; // Prompt string for CLI
  constexpr size_t CLI_MAX_LINE = 128; // characters

} // namespace config

// Helper macro: delay in ms with watchdog reset
#define delay(ms) do { vTaskDelay(pdMS_TO_TICKS(ms)); WDT_RESET(); } while (0)


// Firmware version
#define FW_VERSION "1.0.0"

// System-wide event bus types and global queue
// Event types for the system
enum class EventType : uint8_t {
  EVT_RELAY,         // Relay state changed
  EVT_SENSOR,        // New sensor readings available
  EVT_FACTORY_RESET  // Hardware factory reset triggered
};

// Payload for relay events
struct RelayEventData {
  uint8_t idx;
  bool state;
};

// Payload for sensor events
struct SensorEventData {
  uint16_t ldr;
  uint16_t voltage;
  float temperature;
  float humidity;
};

// Container for any event
struct BusEvent {
  EventType type;
  union {
    RelayEventData relay;
    SensorEventData sensor;
  } data;
};
