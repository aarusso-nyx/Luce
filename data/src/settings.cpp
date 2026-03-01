#include <vector>

#include <dht.h>
#include <driver/adc.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <freertos/queue.h>

#include "bitfield.h"
#include "config.h"
#include "eventBus.h"
#include "http.h"
#include "logger.h"
#include "mqtt.h"
#include "settings.h"
#include "tasks.hpp"  // includes integrated WDT functions
#include "bitfield.h"

// No persistent DHT object needed; use esp-idf-lib dht functions directly

SettingsClass Settings;
using RelayMask = BitField<uint8_t, config::RELAY_COUNT>;




void SettingsClass::begin() {
  // Check if persistent STATE is uninitialized via NVS
  uint8_t state_val = 0xFF;
  bool pristine = true;
  nvs_handle_t h;
  if (nvs_open("relays", NVS_READONLY, &h) == ESP_OK) {
    uint8_t val;
    if (nvs_get_u8(h, "state", &val) == ESP_OK) {
      state_val = val;
      pristine = (val == static_cast<uint8_t>(0xFF));
      LOGSYS("Settings","Begin", pristine ? "State uninitialized" : "State: 0x%02X", val);
    } else {
      LOGSYS("Settings","Begin","State uninitialized");
    }
    nvs_close(h);
  }
  if (pristine) {
    LOGINFO("Settings","Begin","First-time setup: saving default config and state");
    saveConfig();
    saveState();
  } else {
    LOGINFO("Settings","Begin","Existing NVS data found");
  }

  // Load configuration and dynamic state
  loadConfig();
  // Apply logging formats
  Logger::setConsoleFormat(logConsoleFmt);
  Logger::setFileFormat(logFileFmt);
  // Apply log levels
  Logger::setConsoleLevel(static_cast<Logger::Level>(logConsoleLevel));
  Logger::setFileLevel(static_cast<Logger::Level>(logFileLevel));
  // Apply ring-buffer capacity
  Logger::setMaxLines(logMaxLines);
  printConfig();
  loadState();
  printState();
}

// Unified setter implementations using change helpers
bool SettingsClass::setName(const char* v)    { return changeString(name, v, "name"); }
bool SettingsClass::setSSID(const char* v)    { return changeString(ssid[0], v, "ssid1"); }
bool SettingsClass::setPass(const char* v)    { return changeString(pass[0], v, "pass1"); }
bool SettingsClass::setSSID2(const char* v)   { return changeString(ssid[1], v, "ssid2"); }
bool SettingsClass::setPass2(const char* v)   { return changeString(pass[1], v, "pass2"); }
bool SettingsClass::setMqtt(const char* v)    { return changeString(mqtt, v, "mqtt"); }
// Detailed console logs: include file, line, and function
bool SettingsClass::setInterval(uint32_t v)   { return changeConfig(sensorInterval, v, "interval"); }
bool SettingsClass::setLight(uint16_t v)      { return changeState(light, v, "light"); }
// OTA password setter
bool SettingsClass::setOtaPassword(const char* v) { return changeString(otaPassword, v, "otaPassword"); }
// Ring-buffer capacity setter/getter
uint16_t SettingsClass::getLogMaxLines() const {
  return logMaxLines;
}
bool SettingsClass::setLogMaxLines(uint16_t n) {
  if (n > LOGMAXLINES_MAX) n = LOGMAXLINES_MAX;
  if (logMaxLines == n) return false;
  logMaxLines = n;
  // Persist new logMaxLines
  nvs_handle_t h;
  if (nvs_open("config", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u16(h, "logMaxLines", logMaxLines);
    nvs_commit(h);
    nvs_close(h);
  }
  Logger::setMaxLines(logMaxLines);
  LOGINFO("Settings", "Set", "logMaxLines = %u", (unsigned)logMaxLines);
  return true;
}
// Unified log getter/setter
bool SettingsClass::setLog(LogTarget target, LogParam param, const char* value) {
  if (param == LogParam::Level) {
    std::string v(value);
    char c0 = std::tolower(v[0]);
    int lvl = std::isdigit(c0) ? std::stoi(v)
             : (c0 == 'e')         ? Logger::ERROR
             : (c0 == 'w')         ? Logger::WARNING
             : (c0 == 'i')         ? Logger::INFO
             : (c0 == 's')         ? Logger::SYSTEM
             : (c0 == 'd')         ? Logger::DEBUG
             : -1;
    if (lvl < 0) return false;
    uint8_t& field = (target == LogTarget::Console) ? logConsoleLevel : logFileLevel;
    const char* key = (target == LogTarget::Console) ? "logConsoleLevel" : "logFileLevel";
    if ((int)field == lvl) return false;
    field = static_cast<uint8_t>(lvl);
    nvs_handle_t h;
    if (nvs_open("config", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_u8(h, key, field);
      nvs_commit(h);
      nvs_close(h);
    }
    if (target == LogTarget::Console) Logger::setConsoleLevel((Logger::Level)lvl);
    else                               Logger::setFileLevel((Logger::Level)lvl);
    LOGINFO("Settings", "Set", "%s = %d", key, lvl);
    return true;
  } else {
    // Update format string
    if (target == LogTarget::Console) {
      if (changeString(logConsoleFmt, value, "logConsoleFmt")) {
        Logger::setConsoleFormat(logConsoleFmt);
        return true;
      }
    } else {
      if (changeString(logFileFmt, value, "logFileFmt")) {
        Logger::setFileFormat(logFileFmt);
        return true;
      }
    }
    return false;
  }
}

std::string SettingsClass::getLog(LogTarget target, LogParam param) const {
  if (param == LogParam::Level) {
    int lvl = (target == LogTarget::Console) ? logConsoleLevel : logFileLevel;
    return std::to_string(lvl);
  } else {
    const char* fmt = (target == LogTarget::Console) ? logConsoleFmt : logFileFmt;
    return std::string(fmt);
  }
}

// Save configuration (WiFi, MQTT, logging, night mode) to NVS ("config" namespace)
void SettingsClass::saveConfig() {
  // Persist configuration to NVS
  nvs_handle_t h;
  if (nvs_open("config", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, "name", name);
    // Primary WiFi credentials
    nvs_set_str(h, "ssid", ssid[0]);
    nvs_set_str(h, "pass", pass[0]);
    // Secondary WiFi credentials
    nvs_set_str(h, "ssid2", ssid[1]);
    nvs_set_str(h, "pass2", pass[1]);
    // MQTT
    nvs_set_str(h, "mqtt", mqtt);
    // OTA update password
    nvs_set_str(h, "otaPass", otaPassword);
    // Logging format strings
    nvs_set_str(h, "logConsoleFmt", logConsoleFmt);
    nvs_set_str(h, "logFileFmt",    logFileFmt);
    // Logging levels
    nvs_set_u8(h, "logConsoleLevel", logConsoleLevel);
    nvs_set_u8(h, "logFileLevel",    logFileLevel);
    // Ring-buffer capacity and sensor interval
    nvs_set_u16(h, "logMaxLines",    logMaxLines);
    nvs_set_u32(h, "interval",       sensorInterval);
    nvs_commit(h);
    nvs_close(h);
  }

  LOGINFO("Settings","SaveConfig","Configuration saved to NVS (restart to apply)");
}

// Load configuration from NVS ("config" namespace)
void SettingsClass::loadConfig() {
  // Load configuration from NVS
  nvs_handle_t h;
  if (nvs_open("config", NVS_READONLY, &h) == ESP_OK) {
    size_t sz;
    // Device name
    sz = NAME_LEN;
    if (nvs_get_str(h, "name", name, &sz) != ESP_OK) name[0] = '\0';
    // Primary WiFi
    sz = SSID_LEN;
    if (nvs_get_str(h, "ssid", ssid[0], &sz) != ESP_OK) ssid[0][0] = '\0';
    sz = PASS_LEN;
    if (nvs_get_str(h, "pass", pass[0], &sz) != ESP_OK) pass[0][0] = '\0';
    // Secondary WiFi
    sz = SSID_LEN;
    if (nvs_get_str(h, "ssid2", ssid[1], &sz) != ESP_OK) ssid[1][0] = '\0';
    sz = PASS_LEN;
    if (nvs_get_str(h, "pass2", pass[1], &sz) != ESP_OK) pass[1][0] = '\0';
    // MQTT URI
    sz = MQTT_LEN;
    if (nvs_get_str(h, "mqtt", mqtt, &sz) != ESP_OK) mqtt[0] = '\0';
    // OTA password
    sz = PASS_LEN;
    if (nvs_get_str(h, "otaPass", otaPassword, &sz) != ESP_OK) otaPassword[0] = '\0';
    // Sensor interval
    uint32_t iv;
    if (nvs_get_u32(h, "interval", &iv) == ESP_OK) sensorInterval = iv;
    // Logging formats
    sz = LOGFMT_LEN;
    if (nvs_get_str(h, "logConsoleFmt", logConsoleFmt, &sz) != ESP_OK)
      strncpy(logConsoleFmt, Logger::getConsoleFormat().c_str(), LOGFMT_LEN - 1);
    sz = LOGFMT_LEN;
    if (nvs_get_str(h, "logFileFmt", logFileFmt, &sz) != ESP_OK)
      strncpy(logFileFmt, Logger::getFileFormat().c_str(), LOGFMT_LEN - 1);
    // Logging levels
    uint8_t lvl;
    if (nvs_get_u8(h, "logConsoleLevel", &lvl) == ESP_OK) logConsoleLevel = lvl;
    if (nvs_get_u8(h, "logFileLevel", &lvl)    == ESP_OK) logFileLevel    = lvl;
    // Ring-buffer capacity
    uint16_t ml;
    if (nvs_get_u16(h, "logMaxLines", &ml)    == ESP_OK) logMaxLines     = ml;
    nvs_close(h);
  }
  // Persist any default values for missing config keys back to NVS
  saveConfig();
}


// Save dynamic state (relay & daylight mask) to NVS ("state" namespace)
void SettingsClass::saveState() {
  // Save dynamic state to NVS
  // Save dynamic state to NVS
  if (nvs_handle_t h = nvsOpen("relays", false)) {
    nvs_set_u8(h, "state", static_cast<uint8_t>(state));
    nvs_set_u8(h, "night", static_cast<uint8_t>(night));
    nvs_set_u16(h, "light", light);
    nvs_commit(h);
    nvs_close(h);
  }

  // Publish relay change events
  // Publish relay change events for bits that changed
  RelayMask prev = saved;
  RelayMask curr = state;
  uint8_t diff = static_cast<uint8_t>(curr) ^ static_cast<uint8_t>(prev);
  for (uint8_t i = 0; i < config::RELAY_COUNT; ++i) {
    if ((diff >> i) & 1) {
      BusEvent ev;
      ev.type = EventType::EVT_RELAY;
      ev.data.relay.idx = i;
      ev.data.relay.state = curr.get(i);
      EventBus::publish(ev);
    }
  }
}  
// Sensor sampling loop
void SettingsClass::loop() {
  // Initialize ADC and watchdog once
  static bool initialized = false;
  if (!initialized) {
    // Configure ADC width and attenuation for LDR and voltage pins
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11); // LDR_PIN = GPIO34
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11); // VOLT_PIN = GPIO35
    WDT_ADD();
    initialized = true;
  }
  for (;;) {
    WDT_RESET();
    // Read sensors: light and voltage via ADC, DHT via esp-idf-lib
    uint16_t ldr = adc1_get_raw(ADC1_CHANNEL_6);
    uint16_t volt = adc1_get_raw(ADC1_CHANNEL_7);
    float hum = 0.0f, tmp = 0.0f;
    if (dht_read_float_data((dht_sensor_type_t)DHT_TYPE,
                            (gpio_num_t)config::DHT_PIN,
                            &hum, &tmp) != ESP_OK) {
      LOGWARN("Settings","DHT","Failed to read DHT sensor");
    }
    setSensor(ldr);
    setVoltageRaw(volt);
    setHumidity(hum);
    setTemperature(tmp);
    // Publish sensor event
    BusEvent ev;
    ev.type = EventType::EVT_SENSOR;
    ev.data.sensor.ldr = ldr;
    ev.data.sensor.voltage = volt;
    ev.data.sensor.temperature = tmp;
    ev.data.sensor.humidity = hum;
    EventBus::publish(ev);
    vTaskDelay(pdMS_TO_TICKS(sensorInterval));
  }
}

// Load saved state
void SettingsClass::loadState() {
  // Load saved state from NVS
  if (nvs_handle_t h = nvsOpen("relays", true)) {
    uint8_t s;
    if (nvs_get_u8(h, "state", &s) == ESP_OK) state = s;
    if (nvs_get_u8(h, "night", &s) == ESP_OK) night = s;
    uint16_t lt;
    if (nvs_get_u16(h, "light", &lt) == ESP_OK) light = lt;
    nvs_close(h);
  }
}

// Print configuration
void SettingsClass::printConfig() {
  LOGINFO("Settings","PrintConfig","Name: %s", name);
  LOGINFO("Settings","PrintConfig","WiFi1: %s (%s)", ssid[0], pass[0]);
  LOGINFO("Settings","PrintConfig","WiFi2: %s (%s)", ssid[1], pass[1]);
  LOGINFO("Settings","PrintConfig","MQTT: %s", hasMqtt() ? mqtt : "disabled");
  LOGINFO("Settings","PrintConfig","Sensor interval: %u ms", (unsigned)sensorInterval);
  // Log output formats
  LOGINFO("Settings","PrintConfig","ConsoleFmt: %s", logConsoleFmt);
  LOGINFO("Settings","PrintConfig","FileFmt:    %s", logFileFmt);
}

// Print dynamic state
void SettingsClass::printState() {
  LOGINFO("Settings","PrintState","State:0x%02X Night:0x%02X", state, night);
  LOGINFO("Settings","PrintState","Light threshold: %u", (unsigned)light);
}

// -- Removed individual setters in favor of unified change helpers --

// Relay mask mutator (full register)
bool SettingsClass::setState(uint8_t mask) {
  RelayMask bf(mask);
  if (state == bf) return false;
  // Save previous state for event diff
  saved = state;
  // Persist new state mask
  return changeState(state, bf, "state");
}

// Individual relay mutator
bool SettingsClass::setState(uint8_t idx, bool on) {
  if (idx >= config::RELAY_COUNT) return false;
  RelayMask bf = state;
  bf.set(idx, on);
  return setState(bf.get());
}

// Night mask mutator (full register)
bool SettingsClass::setNight(uint8_t mask) {
  RelayMask bf(mask);
  return changeState(night, bf, "night");
}

// Set individual night-mode flag
bool SettingsClass::setNight(uint8_t idx, bool on) {
  if (idx >= config::RELAY_COUNT) return false;
  RelayMask bf = night;
  bf.set(idx, on);
  return setNight(bf.get());
}

