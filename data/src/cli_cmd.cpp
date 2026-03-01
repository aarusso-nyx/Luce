#include <cstdio>
#include <sstream>
#include <string>
#include <cstring>
#include <inttypes.h>

#include <esp_console.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <esp_system.h>

#include "config.h"
#include "util.h"
#include "settings.h"
#include "logger.h"
#include "leds.h"
#include "relays.h"
#include "probes.h"

///////////////////////////////////////////////////////////////////////
// Queries
///////////////////////////////////////////////////////////////////////
// Print version information
static int cmd_version(int, char**) {
  printf("Firmware Version: %s\tBuilt in " __DATE__ ", " __TIME__" \n", FW_VERSION);
  return 0;
}

static int cmd_info(int, char**) {
  // CPU info
  auto c = probes::getCpuInfo();
  std::string extra;
  if (c.pmEnabled) {
    extra = utilFormatSafe(" (cur=%dMHz)", c.curFreqMHz);
  }
  printf(" CPU: %d MHz\n  PM: %s%s\n",
         c.freqMHz,
         c.pmEnabled ? "" : "disabled",
         extra.c_str());

  // Chip info
  auto i = probes::getChipInfo();
  printf("Chip: Rev=%d, Cores=%d, Features=0x%08lX\n",
           i.revision, i.cores, i.features);
  printf(" MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
          i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5]);
  printf(" SDK: %s\n", i.sdkVersion.c_str());

    // Flash info
  auto f = probes::getFlashInfo();
  printf(" Mem: Size=%" PRIu32 " kB, Speed=%" PRIu32 " MHz\n",
         f.sizeKB, f.speedMHz);

  // Heap info
  auto h = probes::getHeapInfo();
  printf("Heap: Free=%" PRIu32 "kB, MinEver=%" PRIu32 "kB, Largest=%" PRIu32 "kB\n",
          h.freeKB, h.minFreeKB, h.largestFreeKB);

  // PSRAM info       
  auto p = probes::getPsramInfo();
  if (p.supported)
    printf("PSRAM: Total=%zu Free=%zu\n", p.total, p.free);
  else  
    puts("PSRAM not present\n");
  
  return 0;
}

static int cmd_wakeup(int, char**) {
  auto w = probes::getWakeupInfo();
  printf("Wakeup Reason: %d (%s)\n", w.cause, w.causeStr.c_str());

  auto r = probes::getResetInfo();
  printf("Reset  Reason: %d (%s)\n", r.reason, r.reasonStr.c_str());
  if (r.brownout) {
    puts("  *** Brownout occurred! ***\n");
  }
  return 0;
}

static int cmd_nvs(int, char**) {
  puts("Namespace/Key        <Type> = Value\n");
  puts("------------------------------------------------\n");
  // Get NVS entries and print them
  for (auto &kv: probes::getNvsEntries()) {
    if ( strncmp(kv.ns.c_str(), "dhcp_state", 10) == 0 ) {
      continue;
    }

    printf("%s/%-15s <%3s> = %s \n",
            kv.ns.c_str(), kv.key.c_str(),
            kv.type.c_str(), kv.value.c_str());
  }
  return 0;
}

static int cmd_parts(int, char**) {
  puts(" Label      Type Subtype  Address     Size(kb)\n");
  puts("------------------------------------------------\n");
  for (auto &p: probes::getPartitionTable()) {
  printf(" %-10s T:%02X   S:%02X   A:%06lX %08lXkB\n",
         p.label.c_str(), p.type, p.subtype, p.address, p.sizeKB);
  }
  return 0;
}

static int cmd_uptime(int, char**) {
  auto u = probes::getUptimeInfo();
  printf("Uptime: %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32 "\n",
         u.days, u.hours, u.minutes, u.secs);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &u.timestamp);
  printf("Date  : %s\n", buf);
  return 0;
}

static int cmd_wifi(int, char**) {
  auto w = probes::getWifiInfo();
  if (w.mode==probes::WifiInfo::OFF) {
    printf("WiFi disabled\n\n");
  } else if (w.mode==probes::WifiInfo::STA) {
    printf("WiFi STA: %s IP:%s RSSI:%d\n\n",
            w.ssid.c_str(),w.ip.c_str(),w.rssi);
  } else {
    printf("WiFi AP: %s Clients:%d IP:%s\n\n",
            w.ssid.c_str(),w.clients,w.ip.c_str());
  }
  return 0;
}

static int cmd_sensor(int argc, char** argv) {
  // sensor [<interval_s> <count>]  
  if (argc == 1) {
    auto s = probes::getSensorInfo();  
    printf("Temperature: %.1fC\n   Humidity: %.1f%%\n   Lighting: %u/%-4u (%s)\n",
           s.temperature, s.humidity, (unsigned)s.light, (unsigned)s.threshold, (s.light > s.threshold) ? "day" : "night");
    return 0;
  }  
  // Parse polling interval (seconds)
  int interval = atoi(argv[1]);
  if (interval <= 0) {
    printf("Invalid interval '%s'\n\n", argv[1]);  
    return 0;
  }  
  if (interval % 2 != 0) {
    printf("Interval must be a multiple of 2 seconds\n\n");  
    return 0;
  }  
  // Parse count (0 = continuous)
  int count = 0;
  if (argc >= 3) count = atoi(argv[2]);
  if (count < 0) count = 0;
  if (count == 0) {
    printf("Polling every %ds; CTRL-C to exit\n", interval);  
  } else {
    printf("Polling every %ds; %d samples\n", interval, count);  
  }  
  int done = 0;
  do {
    auto s = probes::getSensorInfo();  
    printf("L:%u \tV:%u \tT:%.1f \tH:%.1f\r",
           (unsigned)s.light, (unsigned)s.voltage, s.temperature, s.humidity);
    fflush(stdout);       
    if (count > 0 && ++done >= count) break;
    vTaskDelay(pdMS_TO_TICKS(interval * 1000));
  } while (true);  
  return 0;
}  

// Actions

static int cmd_test(int, char**) {
  relaysTest();
  printf("Relays test sequence %s\n", (true) ? "Ok" : "fail");
  return 0;
}

static int cmd_reboot(int, char**) {
  printf("Rebooting...\n\n");
  fflush(stdout);
  vTaskDelay(100);
  esp_restart();
  return 0;
}

static int cmd_reset(int argc, char** argv) {
  if (argc<2 || strcasecmp(argv[1],"yes")!=0) {
    printf("factory reset! 'reset yes' to confirm\n\n");
  } else {
    printf("Erasing NVS...\n");
    nvs_flash_erase();
    return cmd_reboot(argc, argv); // Reboot after reset
  }
  return 0;
}

// State

static int cmd_set(int argc, char** argv) {
  if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    printf("Usage: set <relay|mask|led> <ids>=<on|off> (relay IDs:1-%d, LED IDs:A-%c)\n", config::RELAY_COUNT, char('A' + LedsClass::NUM_LEDS - 1));
    return 0;
  }
  if (argc == 1) {
    auto ri = probes::getRelayInfo();
    auto li = probes::getLedInfo();
    printf("Relays:\n");
    for (int i = 0; i < config::RELAY_COUNT; ++i) {
      printf(" %d: %s (%s)\n", i+1,
             (ri.stateMask & (1 << i)) ? "ON " : "OFF",
             (ri.nightMask & (1 << i)) ? "On Night" : "All Day");
    }
    printf("\nLEDs:\n");
    for (int i = 0; i < (int)LedsClass::NUM_LEDS; ++i) {
      printf(" %c: %s\n", char('A'+i),
             (li.mask & (1 << i)) ? "ON" : "OFF");
    }
    return 0;
  }
  if (argc < 3) {
    printf("Usage: set <relay|mask|led> <ids>=<on|off>\n");
    return 0;
  }
  std::string type = argv[1]; utilTrim(type);
  std::transform(type.begin(), type.end(), type.begin(), ::tolower);
  std::string spec = argv[2]; utilTrim(spec);
  auto eq = spec.find('=');
  if (eq == std::string::npos) {
    printf("Bad syntax '%s'; expected ids=state\n", spec.c_str());
    return 0;
  }
  std::string ids = spec.substr(0, eq); utilTrim(ids);
  std::string vs = spec.substr(eq+1); utilTrim(vs);
  std::transform(vs.begin(), vs.end(), vs.begin(), ::tolower);
  bool st = (vs == "1" || vs == "on" || vs == "true");
  bool isRelay = (type == "relay");
  bool isMask  = (type == "mask");
  bool isLed   = (type == "led");
  if (!isRelay && !isMask && !isLed) {
    printf("Unknown type '%s'; must be relay, mask, or led\n", argv[1]);
    return 0;
  }
  std::istringstream iss(ids);
  std::string tok;
  while (std::getline(iss, tok, ',')) {
    utilTrim(tok);
    if (tok.empty()) continue;
    auto dash = tok.find('-');
    if (dash != std::string::npos) {
      std::string a = tok.substr(0, dash); utilTrim(a);
      std::string b = tok.substr(dash+1); utilTrim(b);
      if (isLed && a.size()==1 && b.size()==1 && isalpha(a[0]) && isalpha(b[0])) {
        for (char c = toupper(a[0]); c <= toupper(b[0]); ++c) {
          int idx = c - 'A';
          if (idx >= 0 && idx < (int)LedsClass::NUM_LEDS) Leds.set(idx, st);
        }
      } else if ((isRelay || isMask) && isdigit(a[0]) && isdigit(b[0])) {
        int ia = std::stoi(a), ib = std::stoi(b);
        for (int i = ia; i <= ib; ++i) {
          if (i >= 1 && i <= (int)config::RELAY_COUNT) {
    if (isMask) relaysSetNightIdx(i-1, st);
    else        relaysSetStateIdx(i-1, st);
          }
        }
      } else {
        printf("Invalid range '%s' for type '%s'\n", tok.c_str(), type.c_str());
      }
    } else {
      if (isLed && tok.size()==1 && isalpha(tok[0])) {
        int idx = toupper(tok[0]) - 'A';
        if (idx >= 0 && idx < (int)LedsClass::NUM_LEDS) Leds.set(idx, st);
        else printf("Invalid LED ID '%s'\n", tok.c_str());
      } else if ((isRelay || isMask) && isdigit(tok[0])) {
        int v = std::stoi(tok);
        if (v >= 1 && v <= (int)config::RELAY_COUNT) {
          if (isMask) relaysSetNightIdx(v-1, st);
          else        relaysSetStateIdx(v-1, st);
        } else {
          printf("Invalid relay ID '%s'\n", tok.c_str());
        }
      } else {
        printf("Invalid ID '%s' for type '%s'\n", tok.c_str(), type.c_str());
      }
    }
  }
  printf("Updated %s(s)\n", argv[1]);
  return 0;
}

static int cmd_log(int argc, char** argv) {
  // log [ show | buffer [<size>] | console [level|format] [<val>] | logfile [level|format] [<val>] ]
  auto ls = probes::getLogSettings();
  if (argc == 1) {
    printf("Console: level=%d, format='%s'\n", ls.consoleLevel, ls.consoleFmt.c_str());
    printf("LogFile: level=%d, format='%s'\n", ls.fileLevel,  ls.fileFmt.c_str());
    printf("Buffer : %d lines\n",      ls.maxLines);
    return 0;
  }
  std::string cmd(argv[1]); utilTrim(cmd); std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
  if (cmd == "show") {
    auto buf = probes::getLogBuffer();
    printf("Showing %zu log lines:\n", buf.size());
    for (auto &l: buf) printf("%s\n", l.c_str());
    return 0;
  }
  if (cmd == "buffer") {
    if (argc == 2) {
      printf("Buffer size: %d\n", ls.maxLines);
    } else if (argc == 3) {
      int sz = atoi(argv[2]);
      Settings.setLogMaxLines(sz);
      printf("Buffer size set to %d\n", sz);
    } else {
      printf("Usage: log buffer [<size>]\n");
    }
    return 0;
  }
  bool fileMode = (cmd == "logfile");
  if (cmd == "console" || fileMode) {
    if (argc == 2) {
      if (fileMode) printf("LogFile: level=%d, format='%s'\n", ls.fileLevel, ls.fileFmt.c_str());
      else          printf("Console: level=%d, format='%s'\n", ls.consoleLevel, ls.consoleFmt.c_str());
      return 0;
    }
    std::string sub(argv[2]); utilTrim(sub); std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);
    if (sub != "level" && sub != "format") {
      printf("Unknown subcommand '%s'; expected 'level' or 'format'\n", sub.c_str());
      return 0;
    }
    SettingsClass::LogTarget target = fileMode ? SettingsClass::LogTarget::File : SettingsClass::LogTarget::Console;
    SettingsClass::LogParam  param  = (sub == "format") ? SettingsClass::LogParam::Format : SettingsClass::LogParam::Level;
    if (argc == 3) {
      std::string val = Settings.getLog(target, param);
      printf("%s %s: %s\n", fileMode ? "LogFile" : "Console", sub.c_str(), val.c_str());
    } else if (argc == 4) {
      Settings.setLog(target, param, argv[3]);
    } else {
      printf("Usage: log %s %s [<value>]\n", fileMode ? "logfile" : "console", sub.c_str());
    }
    return 0;
  }
  printf("Usage: log [ show | buffer [<size>] | console [level|format] [<val>] | logfile [level|format] [<val>] ]\n");
  return 0;
}

// System Queries

static int cmd_system(int, char**) {
  puts("System Information:\n\n");
  cmd_info(0, nullptr);
  puts("\r\nFirmware Info:\n\n");
  cmd_version(0, nullptr);
  puts("\r\nSystem State:\n\n");
  cmd_wakeup(0, nullptr);
  puts("\r\nNVS Entries:\n\n");
  cmd_nvs(0, nullptr);
  puts("\r\nPartition Table:\n\n");
  cmd_parts(0, nullptr);
  return 0;
}

static int cmd_state(int, char**) {
  puts("Networking:\n\n");
  cmd_wifi(0, nullptr);
  puts("\r\nSystem Time:\n\n");
  cmd_uptime(0, nullptr);
  puts("\r\nSensors Readings:\n\n");
  cmd_sensor(1, nullptr);
  return 0;
}

static int cmdFree(int argc, char** argv) {
  // TaskStatus_t tasks[16];
  // UBaseType_t count = uxTaskGetSystemState(tasks, 16, NULL);
  // printf("\r\n%-16s %-10s %-10s\r\n", "Task", "HighWater", "Bytes");
  // printf("================ ========== ==========\r\n");

  // for (int i = 0; i < count; ++i) {
  //   printf("%-16s %10u %10u\r\n",
  //          tasks[i].pcTaskName,
  //          tasks[i].usStackHighWaterMark,
  //          tasks[i].usStackHighWaterMark * sizeof(StackType_t));
  // }

  // printf("\r\nHeap: current=%u bytes, minimum ever=%u bytes\r\n",
  //        esp_get_free_heap_size(),
  //        esp_get_minimum_free_heap_size());

  return 0;
}

void registerCommands() {
  // Zero-initialize to ensure argtable and hint are NULL
  esp_console_cmd_t cmd{};

  // Register all commands
  cmd.command = "system";
  cmd.help    = "Show full system diagnostics";
  cmd.func    = cmd_system;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "state";
  cmd.help    = "Show system state";
  cmd.func    = cmd_state;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "info";
  cmd.help    = "Show hardware info: CPU, Flash, PSRAM";
  cmd.func    = cmd_info;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "nvs";
  cmd.help    = "List NVS namespaces/keys";
  cmd.func    = cmd_nvs;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "parts";
  cmd.help    = "Show partition table";
  cmd.func    = cmd_parts;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "sensor";
  cmd.help    = "Show or poll sensor readings: sensor [<interval_s> <count>]";
  cmd.func    = cmd_sensor;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  
  cmd.command = "uptime";
  cmd.help    = "Show uptime and date";
  cmd.func    = cmd_uptime;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  
  cmd.command = "version";
  cmd.help    = "Show firmware version";
  cmd.func    = cmd_version;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  
  cmd.command = "wakeup";
  cmd.help    = "Show wakeup reason";
  cmd.func    = cmd_wakeup;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  
  cmd.command = "wifi";
  cmd.help    = "Show Wi-Fi info";
  cmd.func    = cmd_wifi;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "free";
  cmd.help    = "Show free memory and task stack usage";
  cmd.func    = cmdFree;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "reboot";
  cmd.help    = "Reboot device";
  cmd.func    = cmd_reboot;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  
  cmd.command = "reset";
  cmd.help    = "Factory reset (erase NVS); reset yes";
  cmd.func    = cmd_reset;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  
  cmd.command = "test";
  cmd.help    = "Run relay test sequence";
  cmd.func    = cmd_test;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

  cmd.command = "log";
  cmd.help    = "Log commands: show, buffer, console, logfile";
  cmd.hint    = "[show] | [buffer [<size>]] | [console [level|format] [<val>]] | [logfile [level|format] [<val>]]";
  cmd.func    = cmd_log;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
  
  cmd.command = "set";
  cmd.help    = "Set relays/mask/LEDs: set <relay|mask|led> <ids>=<on|off> (relay IDs:1-8, LED IDs:A-C)";
  cmd.hint    = "<type> <ids>=<on|off>";
  cmd.func    = cmd_set;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
