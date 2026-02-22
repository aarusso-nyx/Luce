// probes.cpp: Implementation of data layer functions for diagnostics and state
#include "probes.h"

#include <algorithm>
#include <vector>
#include <string>
#include <time.h>

#include <esp_system.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_netif.h>

#include <nvs.h>
#include <nvs_flash.h>
#include <esp_ota_ops.h>

#include "leds.h"
#include "logger.h"
#include "relays.h"
#include "settings.h"

using namespace config;
using namespace probes;

ResetInfo probes::getResetInfo() {
  ResetInfo info;
  info.reason = esp_reset_reason();
  switch (info.reason) {
    case ESP_RST_UNKNOWN:    info.reasonStr = "UNKNOWN";    break;
    case ESP_RST_POWERON:    info.reasonStr = "POWERON";    break;
    case ESP_RST_EXT:        info.reasonStr = "EXT";        break;
    case ESP_RST_SW:         info.reasonStr = "SW";         break;
    case ESP_RST_PANIC:      info.reasonStr = "PANIC";      break;
    case ESP_RST_INT_WDT:    info.reasonStr = "INT_WDT";    break;
    case ESP_RST_TASK_WDT:   info.reasonStr = "TASK_WDT";   break;
    case ESP_RST_WDT:        info.reasonStr = "WDT";        break;
    case ESP_RST_DEEPSLEEP:  info.reasonStr = "DEEPSLEEP";  break;
    case ESP_RST_BROWNOUT:   info.reasonStr = "BROWNOUT";   break;
    case ESP_RST_SDIO:       info.reasonStr = "SDIO";       break;
    default:                 info.reasonStr = "UNKNOWN";    break;
  }
  info.brownout = (info.reason == ESP_RST_BROWNOUT);
  return info;
}

WakeupInfo probes::getWakeupInfo() {
  WakeupInfo info;
  info.cause = esp_sleep_get_wakeup_cause();
  switch (info.cause) {
    case ESP_SLEEP_WAKEUP_EXT0:     info.causeStr = "EXT0";    break;
    case ESP_SLEEP_WAKEUP_EXT1:     info.causeStr = "EXT1";    break;
    case ESP_SLEEP_WAKEUP_TIMER:    info.causeStr = "TIMER";   break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: info.causeStr = "TOUCHPAD";break;
    case ESP_SLEEP_WAKEUP_ULP:      info.causeStr = "ULP";     break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:info.causeStr = "UNDEFINED";break;
    default:                        info.causeStr = "OTHER";   break;
  }
  return info;
}

ChipInfo probes::getChipInfo() {
  ChipInfo info;
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  info.revision = chip.revision;
  info.cores     = chip.cores;
  info.features  = chip.features;
  // Read MAC
  ESP_ERROR_CHECK(esp_read_mac(info.mac, ESP_MAC_WIFI_STA));
  // Use native ESP-IDF API for SDK version
  info.sdkVersion = esp_get_idf_version();
  return info;
}

FlashInfo probes::getFlashInfo() {
  FlashInfo info;
  // Retrieve flash size via ESP-IDF
  uint32_t chipSize = 0;
  esp_flash_get_size(NULL, &chipSize);
  info.sizeKB   = chipSize >> 10;
  // Flash speed not available via ESP-IDF API; set to 0
  info.speedMHz = 0;
  return info;
}

HeapInfo probes::getHeapInfo() {
  HeapInfo info;
  size_t freeHeap    = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  size_t minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
  size_t largestBlk  = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  info.freeKB      = static_cast<uint32_t>(freeHeap >> 10);
  info.minFreeKB   = static_cast<uint32_t>(minFreeHeap >> 10);
  info.largestFreeKB = static_cast<uint32_t>(largestBlk >> 10);
  return info;
}

std::vector<PartitionEntry> probes::getPartitionTable() {
  std::vector<PartitionEntry> parts;
  esp_partition_iterator_t it = esp_partition_find(
    ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it) {
    const esp_partition_t* p = esp_partition_get(it);
    PartitionEntry e;
      e.label   = p->label;
    e.type    = p->type;
    e.subtype = p->subtype;
    e.address = p->address;
    e.sizeKB  = p->size >> 10;
    parts.push_back(e);
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  return parts;
}

std::vector<NvsEntry> probes::getNvsEntries() {
  std::vector<NvsEntry> entries;
  // NVS is initialized by Settings.begin(); just iterate existing entries
  nvs_iterator_t it = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY);
  // Temporary namespace holder
  std::string lastNs;
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    // Namespace string
    std::string ns(info.namespace_name);
    // Skip internal namespaces
    if (ns == "phy" || ns.rfind("nvs.net80211", 0) == 0) {
      it = nvs_entry_next(it);
      continue;
    }
    NvsEntry e;
    e.ns  = ns;
    e.key = info.key;
    // read value based on type
    nvs_handle handle;
    if (nvs_open(info.namespace_name, NVS_READONLY, &handle) == ESP_OK) {
      switch (info.type) {
        case NVS_TYPE_U8: {
          uint8_t v;
          if (nvs_get_u8(handle, info.key, &v) == ESP_OK) {
            e.type = "U8";
            e.value = std::to_string(v);
          }
          break;
        }
        case NVS_TYPE_I32: {
          int32_t v;
          if (nvs_get_i32(handle, info.key, &v) == ESP_OK) {
            e.type = "I32";
            e.value = std::to_string(v);
          }
          break;
        }
        case NVS_TYPE_STR: {
          size_t len = 0;
          if (nvs_get_str(handle, info.key, nullptr, &len) == ESP_OK && len > 0) {
            // RAII buffer for string
            std::vector<char> buf(len);
            if (nvs_get_str(handle, info.key, buf.data(), &len) == ESP_OK) {
              e.value = std::string(buf.data());
            }
            e.type = "STR";
          }
          break;
        }
        case NVS_TYPE_BLOB: {
          size_t blen = 0;
          if (nvs_get_blob(handle, info.key, nullptr, &blen) == ESP_OK && blen > 0) {
            // RAII buffer for blob data
            std::vector<uint8_t> blob(blen);
            if (nvs_get_blob(handle, info.key, blob.data(), &blen) == ESP_OK) {
              std::string s;
              s.reserve(blen * 3);
              for (size_t i = 0; i < blen; ++i) {
                char tmp[4];
                snprintf(tmp, sizeof(tmp), "%02X", blob[i]);
                s += tmp;
                if (i + 1 < blen) s += ' ';
              }
              e.value = s;
            }
            e.type = "BLOB";
          }
          break;
        }
        default: break;
      }
      nvs_close(handle);
    }
    entries.push_back(e);
    it = nvs_entry_next(it);
  }

// sort entries by namespace and key
  std::sort(entries.begin(), entries.end(), [](const NvsEntry &a, const NvsEntry &b) {
    if (a.ns != b.ns) return a.ns < b.ns;
    return a.key < b.key;
  });

  return entries;
}

PsramInfo probes::getPsramInfo() {
  PsramInfo info;
#if CONFIG_SPIRAM_SUPPORT
  info.supported = true;
  // Total and free PSRAM via heap capabilities
  info.total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  info.free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
  info.supported = false;
  info.total = info.free = 0;
#endif
  return info;
}

CpuInfo probes::getCpuInfo() {
  CpuInfo info;
  // CPU frequency in MHz via RTC clock
  info.freqMHz   = rtc_clk_cpu_freq_get() / 1000000;
#if CONFIG_PM_ENABLE
  info.pmEnabled = true;
  info.curFreqMHz = rtc_clk_cpu_freq_get() / 1000000;
#else
  info.pmEnabled = false;
  info.curFreqMHz = info.freqMHz;
#endif
  return info;
}

WifiInfo probes::getWifiInfo() {
  WifiInfo info;
  // Determine Wi-Fi mode
  wifi_mode_t m = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&m) != ESP_OK) {
    info.mode = WifiInfo::OFF;
    return info;
  }
  if (m == WIFI_MODE_NULL) {
    info.mode = WifiInfo::OFF;
  } else if (m == WIFI_MODE_STA) {
    info.mode = WifiInfo::STA;
    // Station IP and RSSI
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
      esp_netif_ip_info_t ipinfo;
      if (esp_netif_get_ip_info(sta_netif, &ipinfo) == ESP_OK &&
          ipinfo.ip.u_addr.ip4.addr != 0) {
        // SSID from station config
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
          info.ssid = std::string(reinterpret_cast<char*>(cfg.sta.ssid));
        }
        char ipstr[16];
        snprintf(ipstr, sizeof(ipstr), "%d.%d.%d.%d", IP2STR(&ipinfo.ip.u_addr.ip4));
        info.ip = std::string(ipstr);
        // RSSI
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
          info.rssi = ap_info.rssi;
        }
      }
    }
  } else if (m == WIFI_MODE_AP) {
    info.mode = WifiInfo::AP;
    // SoftAP SSID and IP
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
      info.ssid = std::string(reinterpret_cast<char*>(cfg.ap.ssid));
    }
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
      esp_netif_ip_info_t ipinfo;
      if (esp_netif_get_ip_info(ap_netif, &ipinfo) == ESP_OK) {
        char ipstr[16];
        snprintf(ipstr, sizeof(ipstr), "%d.%d.%d.%d", IP2STR(&ipinfo.ip.u_addr.ip4));
        info.ip = std::string(ipstr);
      }
    }
    // Clients count
    uint16_t num = 0;
    esp_wifi_ap_get_sta_num(&num);
    info.clients = num;
  } else { // WIFI_MODE_APSTA
    info.mode = WifiInfo::APSTA;
    // For simplicity, report station info
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
      esp_netif_ip_info_t ipinfo;
      if (esp_netif_get_ip_info(sta_netif, &ipinfo) == ESP_OK &&
          ipinfo.ip.u_addr.ip4.addr != 0) {
        wifi_config_t cfg;
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
          info.ssid = std::string(reinterpret_cast<char*>(cfg.sta.ssid));
        }
        char ipstr[16];
        snprintf(ipstr, sizeof(ipstr), "%d.%d.%d.%d", IP2STR(&ipinfo.ip.u_addr.ip4));
        info.ip = std::string(ipstr);
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
          info.rssi = ap_info.rssi;
        }
      }
    }
    uint16_t num = 0;
    esp_wifi_ap_get_sta_num(&num);
    info.clients = num;
  }
  return info;
}

UptimeInfo probes::getUptimeInfo() {
  UptimeInfo info;
  uint64_t ms = esp_timer_get_time() / 1000ULL;
  info.ms       = ms;
  info.seconds  = ms / 1000;
  info.msRem    = ms % 1000;
  uint32_t s    = info.seconds;
  info.days     = s / 86400;
  s %= 86400;
  info.hours    = s / 3600;
  s %= 3600;
  info.minutes  = s / 60;
  info.secs     = s % 60;
  time_t now = time(nullptr);
  localtime_r(&now, &info.timestamp);
  return info;
}

SensorInfo probes::getSensorInfo() {
  SensorInfo info;
  info.light       = Settings.getSensor();
  info.voltage     = Settings.getVoltageRaw();
  info.temperature = Settings.getTemperature();
  info.humidity    = Settings.getHumidity();
  info.threshold   = Settings.getLight();
  return info;
}

RelayInfo probes::getRelayInfo() {
  RelayInfo info;
  info.stateMask = Settings.getState();
  info.nightMask = Settings.getNight();
  return info;
}

LedInfo probes::getLedInfo() {
  LedInfo info;
  info.mask    = Leds.getMask();
  info.numLeds = LedsClass::NUM_LEDS;
  return info;
}

LogSettings probes::getLogSettings() {
  LogSettings info;
  info.consoleLevel = static_cast<int>(Logger::getConsoleLevel());
  info.fileLevel    = static_cast<int>(Logger::getFileLevel());
  info.consoleFmt   = Logger::getConsoleFormat();
  info.fileFmt      = Logger::getFileFormat();
  info.maxLines     = static_cast<int>(Settings.getLogMaxLines());
  return info;
}

std::vector<std::string> probes::getLogBuffer() {
  return Logger::getBuffer();
}

uint64_t probes::getUptimeMs() {
  return esp_timer_get_time() / 1000ULL;
}

SketchInfo probes::getSketchInfo() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* update  = esp_ota_get_next_update_partition(nullptr);
  SketchInfo info;
  info.usedKB = (running->size >> 10);
  info.freeKB = update ? (update->size >> 10) : 0;
  return info;
}