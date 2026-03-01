#include "luce/cli.h"

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <strings.h>
#include "luce_build.h"

#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "luce/boot_diagnostics.h"
#include "luce/boot_state.h"
#include "luce/led_status.h"
#include "luce/i2c_io.h"
#include "luce/net_wifi.h"
#include "luce/ntp.h"
#if LUCE_HAS_MDNS
#include "luce/mdns.h"
#endif
#if LUCE_HAS_TCP_CLI
#include "luce/cli_tcp.h"
#endif
#include "luce/task_budgets.h"
#if LUCE_HAS_MQTT
#include "luce/mqtt.h"
#endif
#if LUCE_HAS_HTTP
#include "luce/http_server.h"
#endif
#include "luce/ota.h"
constexpr const char* kTag = "luce_boot";
constexpr std::size_t kCliLineBuffer = 128;

using CliCommandHandler = int (*)(int argc, char* argv[]);

struct CliCommandInfo {
  const char* name;
  bool mutating;
  bool tcp_readonly;
  const char* usage;
  CliCommandHandler execute;
};

void cli_cmd_status();
void cli_cmd_nvs_dump();
void cli_cmd_i2c_scan();
void cli_cmd_mcp_read(const char* port);
void cli_cmd_relay_set(int channel, int on_off);
void cli_cmd_relay_mask(std::uint32_t value);
void cli_cmd_buttons();
void cli_cmd_lcd_print(const char* text);
void cli_cmd_parts();
void cli_cmd_uptime();
void cli_cmd_test();
void cli_cmd_system();
void cli_cmd_state();
void cli_cmd_free();
void cli_cmd_sensor_snapshot();
void cli_cmd_log();

int cli_handle_help(int, char*[]);
int cli_handle_status(int, char*[]);
int cli_handle_nvs_dump(int, char*[]);
int cli_handle_nvs(int, char*[]);
int cli_handle_i2c_scan(int, char*[]);
int cli_handle_mcp_read(int, char*[]);
int cli_handle_relay_set(int, char*[]);
int cli_handle_relay_mask(int, char*[]);
int cli_handle_buttons(int, char*[]);
int cli_handle_lcd_print(int, char*[]);
int cli_handle_reboot(int, char*[]);
int cli_handle_version(int, char*[]);
int cli_handle_info(int, char*[]);
int cli_handle_wakeup(int, char*[]);
int cli_handle_uptime(int, char*[]);
int cli_handle_reset(int, char*[]);
int cli_handle_parts(int, char*[]);
int cli_handle_free(int, char*[]);
int cli_handle_sensor(int, char*[]);
int cli_handle_test(int, char*[]);
int cli_handle_log(int, char*[]);
int cli_handle_logpage(int, char*[]);
int cli_handle_set(int, char*[]);
int cli_handle_system(int, char*[]);
int cli_handle_state(int, char*[]);
int cli_handle_sensors(int, char*[]);
int cli_handle_wifi_status(int, char*[]);
int cli_handle_wifi(int, char*[]);
int cli_handle_wifi_scan(int, char*[]);
int cli_handle_time_status(int, char*[]);
#if LUCE_HAS_OTA
int cli_handle_ota_status(int, char*[]);
int cli_handle_ota_check(int, char*[]);
#endif
#if LUCE_HAS_MDNS
int cli_handle_mdns_status(int, char*[]);
#endif
#if LUCE_HAS_TCP_CLI
int cli_handle_cli_net_status(int, char*[]);
#endif
#if LUCE_HAS_MQTT
int cli_handle_mqtt_status(int, char*[]);
int cli_handle_mqtt_pubtest(int, char*[]);
#endif
#if LUCE_HAS_HTTP
int cli_handle_http_status(int, char*[]);
#endif

constexpr CliCommandInfo kCliCommands[] = {
    {"version", false, true, "version", cli_handle_version},
    {"info", false, true, "info", cli_handle_info},
    {"wakeup", false, true, "wakeup", cli_handle_wakeup},
    {"uptime", false, true, "uptime", cli_handle_uptime},
    {"system", false, true, "system", cli_handle_system},
    {"state", false, true, "state", cli_handle_state},
    {"nvs", false, true, "nvs", cli_handle_nvs},
    {"free", false, true, "free", cli_handle_free},
    {"sensor", false, true, "sensor [<interval_s> <count>]", cli_handle_sensor},
    {"sensors", false, true, "sensors", cli_handle_sensors},
    {"set", true, false, "set <relay|mask|led> <ids>=<on|off>", cli_handle_set},
    {"log", true, false, "log [show | buffer [<size>] | console [level|format] [<val>] | logfile [level|format] [<val>]]", cli_handle_log},
    {"logpage", true, false, "logpage <next|prev|reset|show>", cli_handle_logpage},
    {"test", true, false, "test", cli_handle_test},
    {"reset", true, false, "reset", cli_handle_reset},
    {"parts", false, true, "parts", cli_handle_parts},
    {"help", false, true, "help", cli_handle_help},
    {"status", false, true, "status", cli_handle_status},
    {"nvs_dump", false, false, "nvs_dump", cli_handle_nvs_dump},
    {"i2c_scan", false, true, "i2c_scan", cli_handle_i2c_scan},
    {"mcp_read", false, true, "mcp_read <gpioa|gpiob>", cli_handle_mcp_read},
    {"relay_set", true, false, "relay_set <0..7> <0|1>", cli_handle_relay_set},
    {"relay_mask", true, false, "relay_mask <hex>", cli_handle_relay_mask},
    {"buttons", false, true, "buttons", cli_handle_buttons},
    {"lcd_print", false, false, "lcd_print <text>", cli_handle_lcd_print},
    {"reboot", true, false, "reboot", cli_handle_reboot},
    {"wifi", false, true, "wifi", cli_handle_wifi},
    {"wifi.status", false, true, "wifi.status", cli_handle_wifi_status},
    {"wifi.scan", false, true, "wifi.scan", cli_handle_wifi_scan},
    {"time.status", false, true, "time.status", cli_handle_time_status},
#if LUCE_HAS_MDNS
    {"mdns.status", false, true, "mdns.status", cli_handle_mdns_status},
#endif
#if LUCE_HAS_TCP_CLI
    {"cli_net.status", false, false, "cli_net.status", cli_handle_cli_net_status},
#endif
#if LUCE_HAS_MQTT
    {"mqtt.status", false, true, "mqtt.status", cli_handle_mqtt_status},
    {"mqtt.pubtest", true, false, "mqtt.pubtest", cli_handle_mqtt_pubtest},
#endif
#if LUCE_HAS_HTTP
    {"http.status", false, true, "http.status", cli_handle_http_status},
#endif
#if LUCE_HAS_OTA
    {"ota.status", false, true, "ota.status", cli_handle_ota_status},
    {"ota.check", true, false, "ota.check [url]", cli_handle_ota_check},
#endif
};

const CliCommandInfo* find_command(const char* command) {
  if (!command || !*command) {
    return nullptr;
  }
  for (const auto& entry : kCliCommands) {
    if (std::strcmp(entry.name, command) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

std::size_t tokenize_cli_line(char* line, char* argv[], std::size_t max_args) {
  std::size_t argc = 0;
  char* token = std::strtok(line, " \t");
  while (token && argc < max_args) {
    argv[argc++] = token;
    token = std::strtok(nullptr, " \t");
  }
  return argc;
}

void cli_trim(char* line) {
  char* write = line;
  for (const char* read = line; read && *read != '\0'; ++read) {
    if (*read == '\r' || *read == '\n') {
      continue;
    }
    *write++ = *read;
  }
  *write = '\0';
}

void log_cli_arguments(const char* command, int argc, char* argv[]) {
  ESP_LOGI(kTag, "CLI cmd='%s' argc=%d", command ? command : "?", argc);
  for (int i = 0; i < argc; ++i) {
    ESP_LOGI(kTag, "CLI arg[%d]='%s'", i, argv[i] ? argv[i] : "(null)");
  }
}

bool parse_u32_with_base(const char* text, int base, std::uint32_t* value, char* token_context = nullptr) {
  if (!text || !*text || !value) {
    return false;
  }
  if (token_context) {
    std::strncpy(token_context, text, 31);
    token_context[31] = '\0';
  }

  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, base);
  if (errno != 0 || end == text || *end != '\0' || parsed > 0xFFFFFFFFULL) {
    return false;
  }

  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

void append_argv_tokens(int argc, char* argv[], int start, char* out, std::size_t out_size) {
  out[0] = '\0';
  std::size_t len = 0;
  for (int i = start; i < argc; ++i) {
    const std::size_t next_len = std::strlen(argv[i]);
    if (len > 0) {
      if (len + 1 >= out_size) {
        ESP_LOGW(kTag, "CLI command requires truncation due to output limit");
        break;
      }
      out[len] = ' ';
      ++len;
      out[len] = '\0';
    }
    if (len >= out_size) {
      break;
    }
    const std::size_t available = out_size - len - 1;
    if (next_len >= available) {
      std::strncpy(&out[len], argv[i], available);
      out[out_size - 1] = '\0';
      ESP_LOGW(kTag, "CLI command text truncated due to output limit");
      break;
    }
    std::strncpy(&out[len], argv[i], available);
    len += next_len;
    out[len] = '\0';
  }
}

void cli_print_help() {
  for (const auto& entry : kCliCommands) {
    if (entry.usage != nullptr) {
      ESP_LOGI(kTag, "  - %s", entry.usage);
    } else {
      ESP_LOGI(kTag, "  - %s", entry.name);
    }
  }
}

bool cli_command_is_mutating(const char* command) {
  const CliCommandInfo* const cmd = find_command(command);
  return cmd != nullptr && cmd->mutating;
}

bool cli_command_is_readonly(const char* command) {
  const CliCommandInfo* const cmd = find_command(command);
  return cmd != nullptr && !cmd->mutating && cmd->tcp_readonly;
}

const char* reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:
      return "UNKNOWN";
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "POWER_GLITCH";
    default:
      return "OTHER";
  }
}

const char* wakeup_reason_name(esp_sleep_wakeup_cause_t reason) {
  switch (reason) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    case ESP_SLEEP_WAKEUP_ALL:
      return "ALL";
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:
      return "UART";
    case ESP_SLEEP_WAKEUP_WIFI:
      return "WIFI";
    case ESP_SLEEP_WAKEUP_COCPU:
      return "COCPU";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
      return "COCPU_TRAP";
    case ESP_SLEEP_WAKEUP_BT:
      return "BT";
    default:
      return "UNKNOWN";
  }
}

bool parse_bool_value(const char* token, bool* out_value) {
  if (!token || !out_value) {
    return false;
  }
  if (std::strcmp(token, "1") == 0 || strcasecmp(token, "on") == 0 ||
      strcasecmp(token, "true") == 0 || strcasecmp(token, "yes") == 0) {
    *out_value = true;
    return true;
  }
  if (std::strcmp(token, "0") == 0 || strcasecmp(token, "off") == 0 ||
      strcasecmp(token, "false") == 0 || strcasecmp(token, "no") == 0) {
    *out_value = false;
    return true;
  }
  return false;
}

bool parse_onoff(const char* token, bool* value) {
  return parse_bool_value(token, value);
}

int cli_handle_help(int, char*[]) {
  cli_print_help();
  return 0;
}

int cli_handle_status(int, char*[]) {
  cli_cmd_status();
  return 0;
}

int cli_handle_version(int, char*[]) {
  ESP_LOGI(kTag, "CLI command version: %s", LUCE_PROJECT_VERSION);
  return 0;
}

int cli_handle_info(int, char*[]) {
  luce_print_chip_info();
  luce_print_app_info();
  luce_print_feature_flags();
  luce_print_heap_stats();
  return 0;
}

int cli_handle_uptime(int, char*[]) {
  cli_cmd_uptime();
  return 0;
}

int cli_handle_wakeup(int, char*[]) {
  const esp_reset_reason_t reset_reason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  ESP_LOGI(kTag, "CLI command wakeup: reset=%s (0x%x) wakeup=%s (0x%x)", reset_reason_name(reset_reason),
           static_cast<unsigned>(reset_reason), wakeup_reason_name(wakeup_reason), static_cast<unsigned>(wakeup_reason));
  return 0;
}

int cli_handle_nvs(int, char*[]) {
  cli_cmd_nvs_dump();
  return 0;
}

int cli_handle_system(int, char*[]) {
  cli_cmd_system();
  return 0;
}

int cli_handle_state(int, char*[]) {
  cli_cmd_state();
  return 0;
}

int cli_handle_sensor(int argc, char* argv[]) {
  if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
    ESP_LOGI(kTag, "CLI command sensor: reads [interval_s] [count], no interval polling in this firmware");
    return 0;
  }

  if (argc == 1) {
    cli_cmd_sensor_snapshot();
    return 0;
  }
  if (argc == 2 || argc > 3) {
    ESP_LOGW(kTag, "CLI command sensor: usage sensor [interval_s] [count]");
    return 1;
  }

  std::uint32_t interval = 0;
  std::uint32_t count = 1;
  char parsed_interval[32] = {0};
  char parsed_count[32] = {0};
  if (!parse_u32_with_base(argv[1], 10, &interval, parsed_interval) || interval == 0) {
    ESP_LOGW(kTag, "CLI command sensor: invalid interval '%s'", parsed_interval);
    return 1;
  }
  if (interval % 2 != 0) {
    ESP_LOGW(kTag, "CLI command sensor: interval must be multiple of 2 seconds");
    return 1;
  }
  if (!parse_u32_with_base(argv[2], 10, &count, parsed_count)) {
    ESP_LOGW(kTag, "CLI command sensor: invalid count '%s'", parsed_count);
    return 1;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    cli_cmd_sensor_snapshot();
    if (i + 1 >= count) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(interval * 1000U));
  }
  return 0;
}

int cli_handle_sensors(int, char*[]) {
  return cli_handle_sensor(1, nullptr);
}

int cli_handle_nvs_dump(int, char*[]) {
  cli_cmd_nvs_dump();
  return 0;
}

int cli_handle_i2c_scan(int, char*[]) {
  cli_cmd_i2c_scan();
  return 0;
}

int cli_handle_mcp_read(int argc, char* argv[]) {
  if (argc != 2) {
    ESP_LOGW(kTag, "CLI command mcp_read usage: mcp_read <gpioa|gpiob>");
    return 1;
  }
  if (std::strcmp(argv[1], "gpioa") != 0 && std::strcmp(argv[1], "a") != 0 &&
      std::strcmp(argv[1], "gpiob") != 0 && std::strcmp(argv[1], "b") != 0) {
    ESP_LOGW(kTag, "CLI command mcp_read: invalid port '%s'", argv[1]);
    return 1;
  }
  cli_cmd_mcp_read(argv[1]);
  return 0;
}

int cli_handle_relay_set(int argc, char* argv[]) {
  if (argc != 3) {
    ESP_LOGW(kTag, "CLI command relay_set usage: relay_set <0..7> <0|1>");
    return 1;
  }
  std::uint32_t channel = 0;
  std::uint32_t value = 0;
  char channel_text[32] = {0};
  char value_text[32] = {0};
  const bool ok1 = parse_u32_with_base(argv[1], 10, &channel, channel_text);
  const bool ok2 = parse_u32_with_base(argv[2], 10, &value, value_text);
  if (!ok1 || !ok2 || value > 1 || channel > 7) {
    ESP_LOGW(kTag, "CLI command relay_set: parse error or out-of-range (channel=%s value=%s)", channel_text, value_text);
    return 1;
  }
  cli_cmd_relay_set(static_cast<int>(channel), static_cast<int>(value));
  return 0;
}

int cli_handle_relay_mask(int argc, char* argv[]) {
  if (argc != 2) {
    ESP_LOGW(kTag, "CLI command relay_mask usage: relay_mask <hex>");
    return 1;
  }
  std::uint32_t value = 0;
  char tmp[32] = {0};
  if (!parse_u32_with_base(argv[1], 16, &value, tmp) || value > 0xFF) {
    ESP_LOGW(kTag, "CLI command relay_mask: parse error for '%s'", tmp);
    return 1;
  }
  cli_cmd_relay_mask(value);
  return 0;
}

int cli_handle_buttons(int, char*[]) {
  cli_cmd_buttons();
  return 0;
}

int cli_handle_lcd_print(int argc, char* argv[]) {
  if (argc < 2) {
    ESP_LOGW(kTag, "CLI command lcd_print usage: lcd_print <text>");
    return 1;
  }
  char text[kCliLineBuffer] = {0};
  append_argv_tokens(argc, argv, 1, text, sizeof(text));
  if (text[0] == '\0') {
    return 1;
  }
  cli_cmd_lcd_print(text);
  return 0;
}

int cli_handle_set(int argc, char* argv[]) {
  if (argc != 3) {
    ESP_LOGW(kTag, "CLI command set: usage set <relay|mask|led> <ids>=<on|off>");
    return 1;
  }
  const char* mode = argv[1];
  const char* spec = argv[2];
  if (!mode || !spec || std::strchr(spec, '=') == nullptr) {
    ESP_LOGW(kTag, "CLI command set usage: set <relay|mask|led> <ids>=<on|off>");
    return 1;
  }

  const bool target_is_relay = (strcasecmp(mode, "relay") == 0);
  const bool target_is_mask = (strcasecmp(mode, "mask") == 0);
  const bool target_is_led = (strcasecmp(mode, "led") == 0);
  if (!target_is_relay && !target_is_mask && !target_is_led) {
    ESP_LOGW(kTag, "CLI command set: invalid target '%s'", mode);
    return 1;
  }
  if (target_is_led) {
    ESP_LOGW(kTag, "CLI command set: led control not available in current firmware");
    return 1;
  }

  const std::size_t eq_pos = static_cast<std::size_t>(std::strchr(spec, '=') - spec);
  if (eq_pos == std::string::npos) {
    ESP_LOGW(kTag, "CLI command set usage: set <relay|mask|led> <ids>=<on|off>");
    return 1;
  }
  const std::string ids(spec, eq_pos);
  const std::string value_str(spec + eq_pos + 1);
  bool value = false;
  if (!parse_onoff(value_str.c_str(), &value)) {
    ESP_LOGW(kTag, "CLI command set: invalid state '%s'", value_str.c_str());
    return 1;
  }

  if (ids.empty()) {
    ESP_LOGW(kTag, "CLI command set: empty id list");
    return 1;
  }

  std::uint8_t next_mask = g_relay_mask;
  std::uint16_t applied = 0;
  std::size_t cursor = 0;
  while (cursor < ids.size()) {
    std::size_t comma_pos = ids.find(',', cursor);
    const std::string token = ids.substr(cursor, comma_pos == std::string::npos ? std::string::npos : (comma_pos - cursor));
    if (!token.empty()) {
      std::uint32_t start = 0;
      std::uint32_t end = 0;
      const std::size_t dash = token.find('-');
      if (dash == std::string::npos) {
        const char* start_cstr = token.c_str();
        char* end_ptr = nullptr;
        start = static_cast<std::uint32_t>(std::strtoul(start_cstr, &end_ptr, 10));
        if (end_ptr == start_cstr || *end_ptr != '\0') {
          ESP_LOGW(kTag, "CLI command set: invalid id token '%s'", token.c_str());
          return 1;
        }
        end = start;
      } else {
        const std::string start_str = token.substr(0, dash);
        const std::string end_str = token.substr(dash + 1);
        if (start_str.empty() || end_str.empty()) {
          ESP_LOGW(kTag, "CLI command set: invalid range token '%s'", token.c_str());
          return 1;
        }
        char* end_ptr = nullptr;
        start = static_cast<std::uint32_t>(std::strtoul(start_str.c_str(), &end_ptr, 10));
        if (end_ptr == start_str.c_str() || *end_ptr != '\0') {
          ESP_LOGW(kTag, "CLI command set: invalid range token '%s'", token.c_str());
          return 1;
        }
        end_ptr = nullptr;
        end = static_cast<std::uint32_t>(std::strtoul(end_str.c_str(), &end_ptr, 10));
        if (end_ptr == end_str.c_str() || *end_ptr != '\0') {
          ESP_LOGW(kTag, "CLI command set: invalid range token '%s'", token.c_str());
          return 1;
        }
      }

      const std::uint32_t lo = (start <= end) ? start : end;
      const std::uint32_t hi = (start <= end) ? end : start;
      for (std::uint32_t ch1 = lo; ch1 <= hi; ++ch1) {
        if (ch1 == 0 || ch1 > 8) {
          ESP_LOGW(kTag, "CLI command set: invalid relay id %lu", static_cast<unsigned long>(ch1));
          return 1;
        }
        const std::uint8_t next_channel = static_cast<std::uint8_t>(ch1 - 1);
        next_mask = relay_mask_for_channel_state(static_cast<int>(next_channel), value, next_mask);
        if (applied < 16) {
          ++applied;
        }
      }
    }
    if (comma_pos == std::string::npos) {
      break;
    }
    cursor = comma_pos + 1;
  }

  if (applied == 0) {
    ESP_LOGW(kTag, "CLI command set: no channels selected");
    return 1;
  }

  const esp_err_t err = set_relay_mask_safe(next_mask);
  if (err == ESP_OK) {
    g_relay_mask = next_mask;
    ESP_LOGI(kTag, "CLI command set: %s=%s mask=0x%02X", target_is_mask ? "mask" : "relay",
             value ? "on" : "off", next_mask);
    return 0;
  }
  ESP_LOGW(kTag, "CLI command set: relay write failed: %s", esp_err_to_name(err));
  return 1;
}

int cli_handle_test(int, char*[]) {
  cli_cmd_test();
  return 0;
}

int cli_handle_log(int argc, char* argv[]) {
  if (argc < 2) {
    cli_cmd_log();
    return 0;
  }

  const char* sub = argv[1];
  if (std::strcmp(sub, "show") == 0) {
    ESP_LOGI(kTag, "CLI command log show: not persisted in this firmware");
    return 0;
  }
  if (std::strcmp(sub, "buffer") == 0) {
    if (argc == 2) {
      ESP_LOGI(kTag, "CLI command log buffer: logging buffer size is fixed in current build");
      return 0;
    }
    if (argc == 3) {
      ESP_LOGI(kTag, "CLI command log buffer: set requested to %s (not persisted)", argv[2]);
      return 0;
    }
    ESP_LOGW(kTag, "CLI command log buffer usage: log buffer [<size>]");
    return 1;
  }
  if (std::strcmp(sub, "console") == 0 || std::strcmp(sub, "logfile") == 0) {
    if (argc < 3) {
      ESP_LOGW(kTag, "CLI command log %s usage: <level|format> [<value>]", sub);
      return 1;
    }
    if (argc == 3) {
      ESP_LOGI(kTag, "CLI command log %s: not persisted in this firmware", sub);
      return 0;
    }
    ESP_LOGI(kTag, "CLI command log %s %s=%s", sub, argv[2], argv[3]);
    return 0;
  }
  ESP_LOGW(kTag, "CLI command log: unknown subcommand '%s'", sub);
  return 1;
}

int cli_handle_logpage(int argc, char* argv[]) {
  if (argc != 2) {
    ESP_LOGW(kTag, "CLI command logpage usage: logpage <next|prev|reset|show>");
    return 1;
  }
  if (std::strcmp(argv[1], "show") == 0) {
    io_lcd_show_logs_page();
    return 0;
  }
  if (std::strcmp(argv[1], "next") == 0) {
    io_lcd_show_logs_page();
    io_lcd_log_page_next();
    return 0;
  }
  if (std::strcmp(argv[1], "prev") == 0) {
    io_lcd_show_logs_page();
    io_lcd_log_page_prev();
    return 0;
  }
  if (std::strcmp(argv[1], "reset") == 0) {
    io_lcd_show_logs_page();
    io_lcd_log_page_reset();
    return 0;
  }
  ESP_LOGW(kTag, "CLI command logpage usage: logpage <next|prev|reset|show>");
  return 1;
}

int cli_handle_reset(int argc, char* argv[]) {
  if (argc < 2 || strcasecmp(argv[1], "yes") != 0) {
    ESP_LOGW(kTag, "CLI command reset: factory reset! use 'reset yes' to confirm");
    return 1;
  }
  ESP_LOGW(kTag, "CLI command reset: erasing NVS");
  const esp_err_t erase_err = nvs_flash_erase();
  if (erase_err != ESP_OK) {
    ESP_LOGW(kTag, "CLI command reset: nvs_flash_erase failed: %s", esp_err_to_name(erase_err));
    return 1;
  }
  cli_handle_reboot(0, nullptr);
  return 0;
}

int cli_handle_parts(int, char*[]) {
  cli_cmd_parts();
  return 0;
}

int cli_handle_free(int, char*[]) {
  cli_cmd_free();
  return 0;
}

int cli_handle_reboot(int, char*[]) {
  ESP_LOGW(kTag, "CLI command reboot: restarting");
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
  return 0;
}

int cli_handle_wifi_status(int, char*[]) {
  wifi_status_for_cli();
  return 0;
}

int cli_handle_wifi(int, char*[]) {
  wifi_status_for_cli();
  return 0;
}

int cli_handle_wifi_scan(int, char*[]) {
  wifi_scan_for_cli();
  return 0;
}

int cli_handle_time_status(int, char*[]) {
  ntp_status_for_cli();
  return 0;
}

#if LUCE_HAS_OTA
int cli_handle_ota_status(int, char*[]) {
  ota_status_for_cli();
  return 0;
}

int cli_handle_ota_check(int argc, char* argv[]) {
  if (argc == 1) {
    ota_request_check();
    return 0;
  }
  if (argc == 2) {
    ota_request_check_with_url(argv[1]);
    return 0;
  }
  ESP_LOGW(kTag, "CLI command ota.check usage: ota.check [url]");
  return 1;
}
#endif

#if LUCE_HAS_MDNS
int cli_handle_mdns_status(int, char*[]) {
  mdns_status_for_cli();
  return 0;
}
#endif

#if LUCE_HAS_TCP_CLI
int cli_handle_cli_net_status(int, char*[]) {
  cli_net_status_for_cli();
  return 0;
}
#endif

#if LUCE_HAS_MQTT
int cli_handle_mqtt_status(int, char*[]) {
  mqtt_status_for_cli();
  return 0;
}

int cli_handle_mqtt_pubtest(int, char*[]) {
  mqtt_pubtest_for_cli();
  return 0;
}
#endif

#if LUCE_HAS_HTTP
int cli_handle_http_status(int, char*[]) {
  http_status_for_cli();
  return 0;
}
#endif

void cli_cmd_status() {
  luce_log_status_health();
}

void cli_cmd_uptime() {
  const std::uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
  const std::uint64_t days = uptime_s / (24ULL * 3600ULL);
  const std::uint64_t hours = (uptime_s / 3600ULL) % 24ULL;
  const std::uint64_t mins = (uptime_s / 60ULL) % 60ULL;
  const std::uint64_t secs = uptime_s % 60ULL;
  const std::time_t boot_time = std::time(NULL) - static_cast<std::time_t>(uptime_s);
  std::tm* tm_utc = std::gmtime(&boot_time);
  char date_line[40] = "n/a";
  if (tm_utc != nullptr) {
    std::snprintf(date_line, sizeof(date_line), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm_utc->tm_year + 1900, tm_utc->tm_mon + 1, tm_utc->tm_mday,
                  tm_utc->tm_hour, tm_utc->tm_min, tm_utc->tm_sec);
  }
  ESP_LOGI(kTag, "CLI command uptime: %llud %lluh %llum %llus", static_cast<unsigned long long>(days),
           static_cast<unsigned long long>(hours), static_cast<unsigned long long>(mins),
           static_cast<unsigned long long>(secs));
  ESP_LOGI(kTag, "CLI command uptime: boot_time=%s", date_line);
}

void cli_cmd_system() {
  ESP_LOGI(kTag, "System command: firmware info");
  cli_handle_version(0, nullptr);
  ESP_LOGI(kTag, "System command: diagnostics");
  luce_print_chip_info();
  luce_print_app_info();
  luce_print_heap_stats();
  ESP_LOGI(kTag, "System command: nvs dump");
  cli_cmd_nvs_dump();
  ESP_LOGI(kTag, "System command: partitions");
  cli_cmd_parts();
}

void cli_cmd_state() {
  ESP_LOGI(kTag, "State: wifi");
  wifi_status_for_cli();
  ESP_LOGI(kTag, "State: time");
  ntp_status_for_cli();
  ESP_LOGI(kTag, "State: sensor");
  cli_cmd_sensor_snapshot();
}

void cli_cmd_parts() {
  ESP_LOGI(kTag, "CLI command parts: scanning partition table");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  if (!it) {
    ESP_LOGW(kTag, "CLI command parts: no partitions found");
    return;
  }
  for (esp_partition_iterator_t cur = it; cur;) {
    const esp_partition_t* partition = esp_partition_get(cur);
    if (partition) {
      ESP_LOGI(kTag, "  type=0x%02X subtype=0x%02X label=%s addr=0x%06lX size=0x%06lX", partition->type,
               partition->subtype, partition->label, static_cast<unsigned long>(partition->address),
               static_cast<unsigned long>(partition->size));
    }
    cur = esp_partition_next(cur);
  }
  esp_partition_iterator_release(it);
}

void cli_cmd_free() {
  ESP_LOGI(kTag, "CLI command free: heap_free=%u min_free=%u", heap_caps_get_free_size(MALLOC_CAP_8BIT),
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

void cli_cmd_sensor_snapshot() {
  float temperature = 0.0f;
  float humidity = 0.0f;
  int light = 0;
  int voltage = 0;
  bool dht_ok = false;
  I2cSensorSnapshot snapshot {};
  if (read_sensor_snapshot(snapshot)) {
    temperature = snapshot.temperature_c;
    humidity = snapshot.humidity_percent;
    light = snapshot.light_raw;
    voltage = snapshot.voltage_raw;
    dht_ok = snapshot.dht_ok;
  }
  ESP_LOGI(kTag, "CLI command sensor: temp=%.1fC hum=%.1f%% light=%d voltage=%d dht=%s", temperature, humidity, light, voltage,
           dht_ok ? "ok" : "invalid");
}

void cli_cmd_test() {
  if (!g_mcp_available) {
    ESP_LOGW(kTag, "CLI command test: MCP unavailable");
    return;
  }
  ESP_LOGI(kTag, "CLI command test: cycling each relay");
  const std::uint8_t start_mask = g_relay_mask;
  for (int i = 0; i < 8; ++i) {
    const std::uint8_t on_mask = relay_mask_for_channel_state(i, true, g_relay_mask);
    (void)set_relay_mask_safe(on_mask);
    g_relay_mask = on_mask;
    vTaskDelay(pdMS_TO_TICKS(250));

    const std::uint8_t off_mask = relay_mask_for_channel_state(i, false, g_relay_mask);
    (void)set_relay_mask_safe(off_mask);
    g_relay_mask = off_mask;
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  (void)set_relay_mask_safe(start_mask);
  g_relay_mask = start_mask;
  ESP_LOGI(kTag, "CLI command test: done");
}

void cli_cmd_log() {
  ESP_LOGI(kTag, "CLI command log: legacy CLI logging controls are no longer persisted");
  ESP_LOGI(kTag, "  options: show | buffer [size] | console [level|format] [value] | logfile [level|format] [value]");
}

void cli_cmd_nvs_dump() {
  ESP_LOGI(kTag, "CLI command nvs_dump: executing");
  dump_nvs_entries();
  ESP_LOGI(kTag, "CLI command nvs_dump: done");
}

void cli_cmd_i2c_scan() {
  I2cScanResult scan {};
  const InitPathResult scan_result = run_i2c_scan_flow(scan, "CLI command i2c_scan", false);
  ESP_LOGI(kTag, "CLI command i2c_scan: found=%d mcp=%d lcd=%d ok=%d", scan.found_count,
           scan.mcp ? 1 : 0, scan.lcd ? 1 : 0, scan_result.ok ? 1 : 0);
  if (!scan_result.ok) {
    ESP_LOGW(kTag, "CLI command i2c_scan: init failed");
  }
}

void cli_cmd_mcp_read(const char* port) {
  if (!g_mcp_available) {
    ESP_LOGW(kTag, "CLI command mcp_read: MCP unavailable");
    return;
  }
  const std::uint8_t reg = (std::strcmp(port, "gpioa") == 0 || std::strcmp(port, "a") == 0)
                               ? kMcpRegGpioA
                               : kMcpRegGpioB;
  std::uint8_t value = 0x00;
  const esp_err_t err = mcp_read_reg(reg, &value);
  ESP_LOGI(kTag, "CLI command mcp_read %s rc=%s value=0x%02X", port, esp_err_to_name(err), value);
}

void cli_cmd_relay_set(int channel, int on_off) {
  const std::uint8_t new_mask = relay_mask_for_channel_state(channel, on_off != 0, g_relay_mask);
  const esp_err_t err = set_relay_mask_safe(new_mask);
  if (err == ESP_OK) {
    g_relay_mask = new_mask;
  }
  ESP_LOGI(kTag, "CLI command relay_set: ch=%d value=%d new_mask=0x%02X rc=%s", channel, on_off,
           new_mask, esp_err_to_name(err));
}

void cli_cmd_relay_mask(std::uint32_t value) {
  const std::uint8_t mask = static_cast<std::uint8_t>(value & 0xFF);
  const esp_err_t err = set_relay_mask_safe(mask);
  if (err == ESP_OK) {
    g_relay_mask = mask;
  }
  ESP_LOGI(kTag, "CLI command relay_mask: mask=0x%02X rc=%s", mask, esp_err_to_name(err));
}

void cli_cmd_buttons() {
  if (!g_mcp_available) {
    ESP_LOGW(kTag, "CLI command buttons: MCP unavailable");
    return;
  }
  std::uint8_t value = 0x00;
  const esp_err_t err = mcp_read_reg(kMcpRegGpioB, &value);
  ESP_LOGI(kTag, "CLI command buttons: rc=%s gpiob=0x%02X", esp_err_to_name(err), value);
}

void cli_cmd_lcd_print(const char* text) {
  if (!text || !*text) {
    ESP_LOGW(kTag, "CLI command lcd_print: missing text argument");
    return;
  }
  const bool ok = i2c_lcd_write_text(text);
  ESP_LOGI(kTag, "CLI command lcd_print: rc=%s text='%s'", ok ? "OK" : "ERR", text);
}

int cli_execute_command(int argc, char* argv[]) {
  if (argc <= 0 || !argv || !argv[0]) {
    led_status_notify_user_error();
    return 1;
  }

  const CliCommandInfo* const command = find_command(argv[0]);
  if (!command || command->execute == nullptr) {
    ESP_LOGW(kTag, "CLI unknown command '%s'", argv[0]);
    cli_print_help();
    led_status_notify_user_error();
    return 1;
  }
  led_status_notify_user_input();
  const int rc = command->execute(argc, argv);
  if (rc != 0) {
    led_status_notify_user_error();
  }
  return rc;
}

int cli_execute_command_readonly(int argc, char* argv[], bool* denied_mutation) {
  if (denied_mutation) {
    *denied_mutation = false;
  }
  if (argc <= 0 || !argv || !argv[0]) {
    led_status_notify_user_error();
    return 1;
  }
  if (cli_command_is_mutating(argv[0])) {
    led_status_notify_user_error();
    if (denied_mutation) {
      *denied_mutation = true;
    }
    ESP_LOGW(kTag, "CLI readonly command denied: '%s'", argv[0]);
    return 2;
  }
  return cli_execute_command(argc, argv);
}

void cli_task(void*) {
  uart_config_t uart_cfg {};
  uart_cfg.baud_rate = 115200;
  uart_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.parity = UART_PARITY_DISABLE;
  uart_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_cfg.source_clk = UART_SCLK_APB;
  if (uart_param_config(UART_NUM_0, &uart_cfg) != ESP_OK) {
    ESP_LOGW(kTag, "CLI uart_param_config failed");
  }
  if (uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                  UART_PIN_NO_CHANGE) != ESP_OK) {
    ESP_LOGW(kTag, "CLI uart_set_pin failed");
  }
  const esp_err_t install_result = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
  if (install_result != ESP_OK && install_result != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "CLI uart_driver_install failed");
  } else {
    ESP_LOGI(kTag, "CLI listening on UART0 at 115200. Type 'help' for commands.");
  }

  char line_buffer[kCliLineBuffer] = {0};
  std::size_t line_len = 0;
  char ch = 0;

  while (true) {
    const int read = uart_read_bytes(UART_NUM_0, reinterpret_cast<uint8_t*>(&ch), 1, pdMS_TO_TICKS(200));
    if (read <= 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (ch == '\r' || ch == '\n') {
      if (line_len == 0) {
        continue;
      }
      line_buffer[line_len] = '\0';

      char command_buffer[kCliLineBuffer] = {0};
      std::memcpy(command_buffer, line_buffer, sizeof(command_buffer));
      cli_trim(command_buffer);
      line_len = 0;

      char* argv[8] = {nullptr};
      const std::size_t argc = tokenize_cli_line(command_buffer, argv, sizeof(argv) / sizeof(argv[0]));
      if (argc == 0) {
        continue;
      }
      log_cli_arguments(argv[0], static_cast<int>(argc), argv);
      cli_execute_command(static_cast<int>(argc), argv);
      std::memset(line_buffer, 0, sizeof(line_buffer));
      continue;
    }

    if ((ch == '\b' || ch == 0x7F) && line_len > 0) {
      --line_len;
      line_buffer[line_len] = '\0';
      continue;
    }

    if (line_len + 1 < sizeof(line_buffer)) {
      line_buffer[line_len++] = ch;
    } else {
      ESP_LOGW(kTag, "CLI input overflow, dropping current command");
      line_len = 0;
      std::memset(line_buffer, 0, sizeof(line_buffer));
    }
  }
}

void cli_startup() {
  if (!luce::start_task(cli_task, luce::task_budget::kTaskCli)) {
    ESP_LOGW(kTag, "CLI task create failed");
  }
}
