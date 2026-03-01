#include "luce/cli.h"

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "luce_build.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_log.h"
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

int cli_handle_help(int, char*[]);
int cli_handle_status(int, char*[]);
int cli_handle_nvs_dump(int, char*[]);
int cli_handle_i2c_scan(int, char*[]);
int cli_handle_mcp_read(int, char*[]);
int cli_handle_relay_set(int, char*[]);
int cli_handle_relay_mask(int, char*[]);
int cli_handle_buttons(int, char*[]);
int cli_handle_lcd_print(int, char*[]);
int cli_handle_reboot(int, char*[]);
int cli_handle_wifi_status(int, char*[]);
int cli_handle_wifi_scan(int, char*[]);
int cli_handle_time_status(int, char*[]);
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

int cli_handle_help(int, char*[]) {
  cli_print_help();
  return 0;
}

int cli_handle_status(int, char*[]) {
  cli_cmd_status();
  return 0;
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

int cli_handle_wifi_scan(int, char*[]) {
  wifi_scan_for_cli();
  return 0;
}

int cli_handle_time_status(int, char*[]) {
  ntp_status_for_cli();
  return 0;
}

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
