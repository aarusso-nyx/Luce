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
#include "luce/i2c_io.h"
#include "luce/net_wifi.h"
#include "luce/ntp.h"
#if LUCE_HAS_MDNS
#include "luce/mdns.h"
#endif
#if LUCE_HAS_TCP_CLI
#include "luce/cli_tcp.h"
#endif
#if LUCE_HAS_MQTT
#include "luce/mqtt.h"
#endif
#if LUCE_HAS_HTTP
#include "luce/http_server.h"
#endif
constexpr const char* kTag = "luce_boot";
constexpr std::size_t kCliTaskStackWords = 6144;
constexpr std::size_t kCliLineBuffer = 128;

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

void cli_print_help() {
  ESP_LOGI(kTag, "CLI commands: help, status, nvs_dump, i2c_scan, mcp_read, relay_set, relay_mask, buttons, lcd_print, reboot");
  ESP_LOGI(kTag, "  - wifi.status");
  ESP_LOGI(kTag, "  - wifi.scan");
  ESP_LOGI(kTag, "  - time.status");
#if LUCE_HAS_MDNS
  ESP_LOGI(kTag, "  - mdns.status");
#endif
#if LUCE_HAS_TCP_CLI
  ESP_LOGI(kTag, "  - cli_net.status");
#endif
#if LUCE_HAS_MQTT
  ESP_LOGI(kTag, "  - mqtt.status");
  ESP_LOGI(kTag, "  - mqtt.pubtest");
#endif
#if LUCE_HAS_HTTP
  ESP_LOGI(kTag, "  - http.status");
#endif
  ESP_LOGI(kTag, "  - relay_set <0..7> <0|1>");
  ESP_LOGI(kTag, "  - relay_mask <hex>");
  ESP_LOGI(kTag, "  - mcp_read <gpioa|gpiob>");
  ESP_LOGI(kTag, "  - buttons");
  ESP_LOGI(kTag, "  - i2c_scan");
  ESP_LOGI(kTag, "  - lcd_print <text>");
  ESP_LOGI(kTag, "  - reboot");
}

bool cli_command_is_mutating(const char* command) {
  if (!command || !*command) {
    return false;
  }
  return std::strcmp(command, "relay_set") == 0 || std::strcmp(command, "relay_mask") == 0 ||
         std::strcmp(command, "reboot") == 0 || std::strcmp(command, "mqtt.pubtest") == 0;
}

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
    return 1;
  }

  int rc = 0;
  if (std::strcmp(argv[0], "help") == 0) {
    cli_print_help();
  } else if (std::strcmp(argv[0], "status") == 0) {
    cli_cmd_status();
  } else if (std::strcmp(argv[0], "nvs_dump") == 0) {
    cli_cmd_nvs_dump();
  } else if (std::strcmp(argv[0], "i2c_scan") == 0) {
    cli_cmd_i2c_scan();
  } else if (std::strcmp(argv[0], "mcp_read") == 0) {
    if (argc != 2) {
      ESP_LOGW(kTag, "CLI command mcp_read usage: mcp_read <gpioa|gpiob>");
      rc = 1;
    } else if (std::strcmp(argv[1], "gpioa") == 0 || std::strcmp(argv[1], "a") == 0 ||
               std::strcmp(argv[1], "gpiob") == 0 || std::strcmp(argv[1], "b") == 0) {
      cli_cmd_mcp_read(argv[1]);
    } else {
      ESP_LOGW(kTag, "CLI command mcp_read: invalid port '%s'", argv[1]);
      rc = 1;
    }
  } else if (std::strcmp(argv[0], "relay_set") == 0) {
    if (argc != 3) {
      ESP_LOGW(kTag, "CLI command relay_set usage: relay_set <0..7> <0|1>");
      rc = 1;
    } else {
      std::uint32_t channel = 0;
      std::uint32_t value = 0;
      char channel_text[32] = {0};
      char value_text[32] = {0};
      const bool ok1 = parse_u32_with_base(argv[1], 10, &channel, channel_text);
      const bool ok2 = parse_u32_with_base(argv[2], 10, &value, value_text);
      if (!ok1 || !ok2 || value > 1 || channel > 7) {
        ESP_LOGW(kTag,
                 "CLI command relay_set: parse error or out-of-range (channel=%s value=%s)", channel_text,
                 value_text);
        rc = 1;
      } else {
        cli_cmd_relay_set(static_cast<int>(channel), static_cast<int>(value));
      }
    }
  } else if (std::strcmp(argv[0], "relay_mask") == 0) {
    if (argc != 2) {
      ESP_LOGW(kTag, "CLI command relay_mask usage: relay_mask <hex>");
      rc = 1;
    } else {
      std::uint32_t value = 0;
      char tmp[32] = {0};
      if (!parse_u32_with_base(argv[1], 16, &value, tmp) || value > 0xFF) {
        ESP_LOGW(kTag, "CLI command relay_mask: parse error for '%s'", tmp);
        rc = 1;
      } else {
        cli_cmd_relay_mask(value);
      }
    }
  } else if (std::strcmp(argv[0], "buttons") == 0) {
    cli_cmd_buttons();
  } else if (std::strcmp(argv[0], "lcd_print") == 0) {
    if (argc < 2) {
      ESP_LOGW(kTag, "CLI command lcd_print usage: lcd_print <text>");
      rc = 1;
    } else {
      char text[kCliLineBuffer] = {0};
      snprintf(text, sizeof(text), "%s", argv[1]);
      for (int i = 2; i < argc; ++i) {
        const std::size_t used = std::strlen(text);
        const std::size_t next_len = std::strlen(argv[i]);
        if (used + 1 + next_len >= sizeof(text)) {
          ESP_LOGW(kTag, "CLI command lcd_print: truncated due to output length limit");
          break;
        }
        text[used] = ' ';
        text[used + 1] = '\0';
        std::strncat(text, argv[i], sizeof(text) - std::strlen(text) - 1);
      }
      cli_cmd_lcd_print(text);
    }
  } else if (std::strcmp(argv[0], "reboot") == 0) {
    ESP_LOGW(kTag, "CLI command reboot: restarting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
  } else if (std::strcmp(argv[0], "wifi.status") == 0) {
    wifi_status_for_cli();
  } else if (std::strcmp(argv[0], "wifi.scan") == 0) {
    wifi_scan_for_cli();
  } else if (std::strcmp(argv[0], "time.status") == 0) {
    ntp_status_for_cli();
#if LUCE_HAS_MDNS
  } else if (std::strcmp(argv[0], "mdns.status") == 0) {
    mdns_status_for_cli();
#endif
#if LUCE_HAS_TCP_CLI
  } else if (std::strcmp(argv[0], "cli_net.status") == 0) {
    cli_net_status_for_cli();
#endif
#if LUCE_HAS_MQTT
  } else if (std::strcmp(argv[0], "mqtt.status") == 0) {
    mqtt_status_for_cli();
  } else if (std::strcmp(argv[0], "mqtt.pubtest") == 0) {
    mqtt_pubtest_for_cli();
#endif
#if LUCE_HAS_HTTP
  } else if (std::strcmp(argv[0], "http.status") == 0) {
    http_status_for_cli();
#endif
  } else {
    ESP_LOGW(kTag, "CLI unknown command '%s'", argv[0]);
    cli_print_help();
    rc = 1;
  }
  return rc;
}

int cli_execute_command_readonly(int argc, char* argv[], bool* denied_mutation) {
  if (denied_mutation) {
    *denied_mutation = false;
  }
  if (argc <= 0 || !argv || !argv[0]) {
    return 1;
  }
  if (cli_command_is_mutating(argv[0])) {
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
  if (xTaskCreate(cli_task, "cli", kCliTaskStackWords, nullptr, 2, nullptr) != pdPASS) {
    ESP_LOGW(kTag, "CLI task create failed");
  }
}
