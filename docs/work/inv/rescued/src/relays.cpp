#include <i2cdev.h>
#include <mcp23017.h>
#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include "bitfield.h"
#include "config.h"
using namespace config;
#include "lcd.h"
#include "logger.h"
// Module header
#include "relays.h"
#include "settings.h"
#include "util.h"
#include "tasks.hpp"  // includes integrated WDT functions

// Night-mode update timer handle
static TimerHandle_t nightTimer = nullptr;


// MCP23017 I2C expander handle
static mcp23017_handle_t mcp = NULL;

static volatile bool intFlag = false;
// Previous input bits snapshot
static uint8_t prevInputs = 0;
// Previous day-mode state
static bool prevDay = false;

// IRQ handler for relay inputs
static void IRAM_ATTR onRelayIRQ() {
  intFlag = true;
}

// No class: free-function API

// Initialize relay hardware and inputs; return false to abort task
bool relaysInit() {
  LOGINFO("Relays","Init","Initializing relays");
  
  // Initialize I2C expander (MCP23017)
  i2cdev_init();
  esp_err_t err = mcp23017_init_desc(&mcp,
                                     I2C_NUM_0,
                                     (gpio_num_t)config::I2C_SDA,
                                     (gpio_num_t)config::I2C_SCL,
                                     MCP_I2C_ADDR);
  if (err != ESP_OK) {
    LOGERR("Relays","Init","MCP23017 init failed: %s", esp_err_to_name(err));
    return false;
  }
  // Hardware initialized; proceed

  // Configure interrupt pin for relay inputs (negative edge)
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_NEGEDGE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pin_bit_mask = (1ULL << MCP_INTA_PIN);
  gpio_config(&io_conf);
  // Install ISR service and handler
  gpio_install_isr_service(0);
  gpio_isr_handler_add(MCP_INTA_PIN, onRelayIRQ, NULL);
  
  // Bulk configure expander directions and outputs
  // Inputs on upper RELAY_COUNT bits, outputs on lower bits
  uint16_t dirMask = ((1u << RELAY_COUNT) - 1u) << RELAY_COUNT;
  mcp23017_port_dir(mcp, dirMask);
  // Set initial outputs from saved state (active-low)
  uint8_t state = Settings.getState();
  uint16_t outMask = (~state) & ((1u << RELAY_COUNT) - 1u);
  mcp23017_port_write(mcp, outMask);
  // Prime previous input states
  uint16_t portVal = 0;
  mcp23017_port_read(mcp, &portVal);
  prevInputs = static_cast<uint8_t>((portVal >> RELAY_COUNT) & ((1u << RELAY_COUNT) - 1u));
  // Initialize previous day-mode state
  prevDay = (Settings.getSensor() > Settings.getLight());

  nightTimer = xTimerCreate("NightUpd",
                            pdMS_TO_TICKS(config::NIGHT_TIMER_SEC * 1000UL),
                            pdFALSE,
                            nullptr,
    [](TimerHandle_t xTimer) {
      relaysUpdate();
    });
  if (nightTimer == nullptr) {
    LOGWARN("Relays","Init","Failed to create night update timer");
  }
  return true;
}


// Poll inputs and update relays accordingly; return true to continue
bool relaysLoop() {
  const uint8_t N = RELAY_COUNT;  // Input pins start after relay pins

  if (intFlag) {
    intFlag = false;
    // Read all port states
    uint16_t portVal = 0;
    mcp23017_port_read(mcp, &portVal);
    // Extract input bits (pins RELAY_COUNT..2*RELAY_COUNT-1)
    uint8_t inputs = static_cast<uint8_t>((portVal >> RELAY_COUNT) & ((1 << RELAY_COUNT) - 1));
    uint8_t diff = inputs ^ prevInputs;
    prevInputs = inputs;
    // Command key is highest input bit (bit RELAY_COUNT-1)
    bool cmd = !(inputs & (1 << (RELAY_COUNT - 1)));
    if (cmd) {
      bool handled = false;
      // Numeric keys 0..RELAY_COUNT-2
      for (uint8_t n = 0; n < RELAY_COUNT - 1; ++n) {
        if (!(inputs & (1 << n))) {
          lcdHandleKey(static_cast<LcdKey>(LCD_KEY_1 + n));
          handled = true;
          break;
        }
      }
      if (!handled) {
        lcdHandleKey(LCD_KEY_DOWN);
      }
    } else {
      // Toggle relay for any pressed input key
      for (uint8_t n = 0; n < RELAY_COUNT; ++n) {
        if (diff & (1 << n)) {
          if (!(inputs & (1 << n))) {
            relaysToggle(n);
          }
        }
      }
    }
  }
  return true;
}


// Test sequence for all relays
// Blink each relay once
void relaysTest() {
  if (!relaysEnabled) {
    LOGDEBUG("Relays","Test","Relays disabled, skipping");
    return;
  }
  // Snapshot current relay states
  RelayMask mask(Settings.getState());
  const uint8_t N = RELAY_COUNT;
  const uint8_t all_off = (uint8_t)((1u << N) - 1u);
  // Ensure all outputs off (HIGH)
  mcp23017_port_write(mcp, (uint16_t)all_off);
  // Blink each relay sequentially
  for (uint8_t i = 0; i < N; ++i) {
    uint8_t mask_on = (uint8_t)(all_off & ~(1u << i));
    // Turn one on (active-low) and others off
    mcp23017_port_write(mcp, (uint16_t)mask_on);
    vTaskDelay(pdMS_TO_TICKS(500));
    // All off
    mcp23017_port_write(mcp, (uint16_t)all_off);
    WDT_RESET();
  }
  // Restore original states (bitwise invert mask)
  uint8_t orig_port = (uint8_t)(all_off & ~mask.get());
  mcp23017_port_write(mcp, (uint16_t)orig_port);
}




/* removed legacy RelaysClass method */
// Set full night-mode mask
bool relaysSetNight(uint8_t mask) {
  if (!relaysEnabled) return false;
  if (!Settings.setNight(mask)) return false;
  LOGINFO("Relays","Night","Mask:0x%02X",mask);
  if (nightTimer) xTimerReset(nightTimer, 0);
  return true;
}

// Set full relay state mask
bool relaysSetState(uint8_t mask) {
  if (!relaysEnabled) return false;
  if (!Settings.setState(mask)) return false;
  LOGINFO("Relays","SetAll","Mask:0x%02X",mask);
  uint16_t outMask = (~mask) & ((1u << config::RELAY_COUNT) - 1u);
  mcp23017_port_write(mcp, outMask);
  return true;
}

// Set individual night-mode bit
bool relaysSetNightIdx(uint8_t idx, bool on) {
  if (!relaysEnabled || idx >= config::RELAY_COUNT) return false;
  uint8_t m = Settings.getNight();
  uint8_t nm = on ? (m | (1u << idx)) : (m & ~(1u << idx));
  return relaysSetNight(nm);
}

// Set individual relay state bit
bool relaysSetStateIdx(uint8_t idx, bool on) {
  if (!relaysEnabled || idx >= config::RELAY_COUNT) return false;
  uint8_t s = Settings.getState();
  uint8_t ns = on ? (s | (1u << idx)) : (s & ~(1u << idx));
  return relaysSetState(ns);
}


// Toggle a relay
void relaysToggle(uint8_t idx) {
  if (!relaysEnabled || idx >= config::RELAY_COUNT) return;
  uint8_t s = Settings.getState() ^ (1u << idx);
  relaysSetState(s);
}
// Emergency shutdown: force all relays off
void relaysForceAllOff() {
  if (!relaysEnabled) {
    LOGDEBUG("Relays","ForceOff","disabled");
    return;
  }
  Settings.setState(0);
  uint16_t offMask = (1u << config::RELAY_COUNT) - 1u;
  mcp23017_port_write(mcp, offMask);
  LOGINFO("Relays","ForceOff","All off");
}


// Update relays based on sensor threshold and night mask
void relaysUpdate() {
  if (!relaysEnabled) return;
  bool newDay = Settings.getSensor() > Settings.getLight();
  if (newDay == isDay) return;
  isDay = newDay;
  LOGINFO("Relays","Mode", isDay ? "day" : "night");
  uint16_t allOff = (1u << config::RELAY_COUNT) - 1u;
  uint8_t state = Settings.getState();
  uint8_t night = Settings.getNight();
  uint16_t outMask = allOff & ~(state & (isDay ? ~night : allOff));
  mcp23017_port_write(mcp, outMask);
}