// probes.h: Data layer structures and functions for device diagnostics and state
#pragma once
#include <string>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_sleep.h>

#include <vector>
#include <time.h>

namespace probes {
// Reset information
struct ResetInfo {
  esp_reset_reason_t reason;
  std::string reasonStr;
  bool brownout;
};
ResetInfo getResetInfo();

// Wakeup reason information
struct WakeupInfo {
  esp_sleep_wakeup_cause_t cause;
  std::string causeStr;
};
WakeupInfo getWakeupInfo();

// Chip information
struct ChipInfo {
  int revision;
  int cores;
  uint32_t features;
  uint8_t mac[6];
  std::string sdkVersion;
};
ChipInfo getChipInfo();

// Flash memory information
struct FlashInfo {
  uint32_t sizeKB;
  uint32_t speedMHz;
};
FlashInfo getFlashInfo();

// Heap memory information
struct HeapInfo {
  uint32_t freeKB;
  uint32_t minFreeKB;
  uint32_t largestFreeKB;
};
HeapInfo getHeapInfo();

// Partition table entry
struct PartitionEntry {
  std::string label;
  uint8_t type;
  uint8_t subtype;
  uint32_t address;
  uint32_t sizeKB;
};
std::vector<PartitionEntry> getPartitionTable();

// NVS (Non-volatile storage) entry
struct NvsEntry {
  std::string ns;
  std::string key;
  std::string type;
  std::string value;
};
std::vector<NvsEntry> getNvsEntries();

// PSRAM information
struct PsramInfo {
  bool supported;
  size_t total;
  size_t free;
};
PsramInfo getPsramInfo();

// CPU and power management information
struct CpuInfo {
  int freqMHz;
  bool pmEnabled;
  int curFreqMHz;
};
CpuInfo getCpuInfo();

// Wi-Fi information
struct WifiInfo {
  enum Mode { OFF, STA, AP, APSTA } mode;
  std::string ssid;
  std::string ip;
  int rssi;
  int clients;
};
WifiInfo getWifiInfo();

// Uptime information
struct UptimeInfo {
  uint64_t ms;
  uint32_t seconds;
  uint32_t msRem;
  uint32_t days;
  uint32_t hours;
  uint32_t minutes;
  uint32_t secs;
  struct tm timestamp;
};
UptimeInfo getUptimeInfo();

// Sensor readings
struct SensorInfo {
  uint16_t light;
  uint16_t voltage;
  float temperature;
  float humidity;
  uint16_t threshold;
};
SensorInfo getSensorInfo();

// Relay state information
struct RelayInfo {
  uint8_t stateMask;
  uint8_t nightMask;
};
RelayInfo getRelayInfo();

// LED state information
struct LedInfo {
  uint8_t mask;
  uint8_t numLeds;
};
LedInfo getLedInfo();

// Logging settings and buffer
struct LogSettings {
  int consoleLevel;
  int fileLevel;
  std::string consoleFmt;
  std::string fileFmt;
  // Ring-buffer capacity
  int maxLines;
};
LogSettings getLogSettings();
std::vector<std::string> getLogBuffer();

// Uptime from system timer
uint64_t getUptimeMs();
// Sketch (application) size and free OTA space (in KB)
struct SketchInfo {
  uint32_t usedKB;
  uint32_t freeKB;
};
SketchInfo getSketchInfo();
} // namespace probes