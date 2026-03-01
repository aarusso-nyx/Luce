
// settings.h: handles persistent storage, WiFi config portal, and mDNS
#pragma once

#include "config.h"
#include <cmath>
#include <functional>
#include <string>
#include "bitfield.h"
#include "logger.h"

// Relay bitmask type
using RelayMask = BitField<uint8_t, config::RELAY_COUNT>;


// EventBus replaces legacy relay callbacks
class SettingsClass {
public:
  void begin();
  void loop();
  // Unified logging control: level or format, for console or file
  enum class LogTarget { Console, File };
  enum class LogParam  { Level,  Format };
  // Set or query log settings: for level, supply value as number or initial letter (e.g. "error","w"); for format, supply format string
  bool setLog(LogTarget target, LogParam param, const char* value);
  // Get log setting; returns level as text or format string
  std::string getLog(LogTarget target, LogParam param) const;
  // Ring-buffer capacity settings
  uint16_t getLogMaxLines() const;
  bool setLogMaxLines(uint16_t n);
  
  const char* getName() const { return name; }
  bool setName(const char* name);
  
  const char* getSSID() const { return ssid[0]; }
  bool setSSID(const char* ssid0);
  const char* getPass() const { return pass[0]; }
  bool setPass(const char* pass0);
  const char* getSSID2() const { return ssid[1]; }
  bool setSSID2(const char* ssid1);
  const char* getPass2() const { return pass[1]; }
  bool setPass2(const char* pass1);
  
  const char* getMqtt() const { return mqtt; }
  bool setMqtt(const char* uri);
  bool hasMqtt() const { return mqtt[0] != '0' && mqtt[0] != '\0'; }
  // OTA update password access
  const char* getOtaPassword() const { return otaPassword; }
  bool setOtaPassword(const char* pass);

  uint16_t getLight() const { return light; }
  bool setLight(uint16_t threshold);

  uint8_t getNight() const { return night; }
  bool setNight(uint8_t mask);
  
  bool getNight(uint8_t idx) const { return (night >> idx) & 1; }
  bool setNight(uint8_t idx, bool on);
  
  uint8_t getState() const { return state; }
  bool setState(uint8_t mask);
  
  bool getState(uint8_t idx) const { return (state >> idx) & 1; }
  bool setState(uint8_t idx, bool on);
  

  uint32_t getInterval() const { return sensorInterval; }
  bool setInterval(uint32_t v);
  
  void setSensor(uint16_t v)    { sensor = v; }
  uint16_t getSensor() const    { return sensor; }
  
  void setVoltageRaw(uint16_t v){ voltageRaw = v; }
  uint16_t getVoltageRaw() const{ return voltageRaw; }
  
  void setTemperature(float v)   { temperature = v; }
  float getTemperature() const   { return temperature; }
  
  void setHumidity(float v)      { humidity = v; }
  float getHumidity() const      { return humidity; }
  
  static constexpr size_t NAME_LEN = 32;
  static constexpr size_t SSID_LEN = 32;
  static constexpr size_t PASS_LEN = 64;
  static constexpr size_t MQTT_LEN = 128;
  
private:
  void loadConfig();
  void loadState();
  void saveConfig();
  void saveState();
  void printConfig();
  void printState();

  template<size_t N>
  bool changeString(char (&field)[N], const char* newVal, const char* fieldName);
  bool changeBool(bool& field, bool newVal, const char* fieldName);
  template<typename T>
  bool changeConfig(T& field, T newVal, const char* fieldName);
  template<typename T>
  bool changeState(T& field, T newVal, const char* fieldName);

  char ssid[2][SSID_LEN] = { "NYXK", "NYXH" };
  char pass[2][PASS_LEN] = { "It's$14.99!", "Its$14.99!" };
  char name[NAME_LEN] = "Nyx Luce";
  char mqtt[MQTT_LEN] = "mDNS";
  // OTA update password
  char otaPassword[PASS_LEN] = "It's$14.99!";

  RelayMask state{0};
  // Previously persisted state, for change detection
  RelayMask saved{0};
  RelayMask night{static_cast<uint8_t>(~0)};

  // Sensor and relay configuration
  uint16_t light = 3000;                     // default light threshold
  uint16_t sensor = 0;                       // last sensor reading
  uint16_t voltageRaw = 0;                   // last voltage reading
  float temperature = 0.0f;                  // last temperature reading
  float humidity = 0.0f;                     // last humidity reading
  uint32_t sensorInterval = config::SENSOR_INTERVAL_DEFAULT;
  // Logging format strings
  static constexpr size_t LOGFMT_LEN = 128;
  char logConsoleFmt[LOGFMT_LEN] = {0};
  char logFileFmt[LOGFMT_LEN]    = {0};
  uint8_t logConsoleLevel = static_cast<uint8_t>(Logger::INFO);
  uint8_t logFileLevel    = static_cast<uint8_t>(Logger::INFO);
  // Ring-buffer capacity for in-memory logs (0-1023)
  static constexpr size_t LOGMAXLINES_MIN = 0;
  static constexpr size_t LOGMAXLINES_MAX = 1023;
  uint16_t logMaxLines = 1000;
};


extern SettingsClass Settings;
#include <cstring>

template<size_t N>
bool SettingsClass::changeString(char (&field)[N], const char* newVal, const char* fieldName) {
  if (newVal == nullptr || strcmp(field, newVal) == 0) return false;
  strlcpy(field, newVal, N);
  LOGINFO("Settings", "Set", "%s = %s", fieldName, field);
  saveConfig();
  return true;
}

inline bool SettingsClass::changeBool(bool& field, bool newVal, const char* fieldName) {
  if (field == newVal) return false;
  field = newVal;
  LOGINFO("Settings", "Set", "%s = %u", fieldName, newVal);
  saveConfig();
  return true;
}

template<typename T>
bool SettingsClass::changeConfig(T& field, T newVal, const char* fieldName) {
  if (field == newVal) return false;
  field = newVal;
  LOGINFO("Settings", "Set", "%s = %u", fieldName, (unsigned)newVal);
  saveConfig();
  return true;
}

template<typename T>
bool SettingsClass::changeState(T& field, T newVal, const char* fieldName) {
  if (field == newVal) return false;
  field = newVal;
  LOGINFO("Settings", "Set", "%s = %u", fieldName, (unsigned)newVal);
  saveState();
  return true;
}

// end of header