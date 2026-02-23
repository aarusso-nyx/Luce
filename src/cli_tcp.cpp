// Stage8 TCP CLI transport.
#include "luce/cli_tcp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if LUCE_HAS_TCP_CLI

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs.h"

#include "luce/cli.h"
#include "luce/net_wifi.h"
#include "luce_build.h"

namespace {

constexpr const char* kTag = "[CLI_NET]";
constexpr const char* kCliNetNs = "cli_net";
constexpr std::size_t kLineMax = 255;
constexpr std::size_t kTokenMax = 8;
constexpr std::size_t kTaskStackWords = 3072;
constexpr std::uint32_t kDefaultPort = 2323;
constexpr std::uint32_t kDefaultIdleTimeoutS = 120;

struct CliNetConfig {
  bool enabled = false;
  uint16_t port = kDefaultPort;
  char token[33] = "luce-cli";
  uint32_t idle_timeout_s = kDefaultIdleTimeoutS;
};

struct CliNetSession {
  bool running = false;
  bool authed = false;
  int fail_count = 0;
  uint32_t port = kDefaultPort;
  char peer[48] = "n/a";
};

CliNetConfig g_cfg {};
CliNetSession g_session {};
TaskHandle_t g_task = nullptr;
int g_listener = -1;

bool is_auth_line(int argc, char* argv[]) {
  return argc >= 2 && (std::strcmp(argv[0], "AUTH") == 0 || std::strcmp(argv[0], "auth") == 0);
}

void load_cli_net_config() {
  std::memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.enabled = false;
  g_cfg.port = kDefaultPort;
  std::snprintf(g_cfg.token, sizeof(g_cfg.token), "luce-cli");
  g_cfg.idle_timeout_s = kDefaultIdleTimeoutS;

  nvs_handle_t nvs_handle = 0;
  if (nvs_open(kCliNetNs, NVS_READONLY, &nvs_handle) != ESP_OK) {
    ESP_LOGW(kTag, "[CLI_NET] namespace '%s' not found; defaults active", kCliNetNs);
    return;
  }

  std::uint8_t enabled = 0;
  std::uint32_t timeout = 0;
  size_t needed = 0;
  if (nvs_get_u8(nvs_handle, "enabled", &enabled) == ESP_OK) {
    g_cfg.enabled = (enabled != 0);
  }

  if (nvs_get_u16(nvs_handle, "port", &g_cfg.port) != ESP_OK || g_cfg.port == 0) {
    g_cfg.port = kDefaultPort;
  }
  if (nvs_get_u32(nvs_handle, "idle_timeout_s", &timeout) == ESP_OK) {
    if (timeout == 0 || timeout > 1800) {
      timeout = kDefaultIdleTimeoutS;
    }
    g_cfg.idle_timeout_s = timeout;
  }
  if (nvs_get_str(nvs_handle, "token", nullptr, &needed) == ESP_OK && needed > 0) {
    if (needed >= sizeof(g_cfg.token)) {
      needed = sizeof(g_cfg.token) - 1;
    }
    nvs_get_str(nvs_handle, "token", g_cfg.token, &needed);
  }
  nvs_close(nvs_handle);
}

void send_line(int sock, const char* text) {
  if (!text) {
    return;
  }
  char out[kLineMax + 16] = {0};
  const std::size_t n = std::strlen(text);
  if (n + 4 > sizeof(out)) {
    return;
  }
  std::snprintf(out, sizeof(out), "%s\r\n", text);
  send(sock, out, std::strlen(out), 0);
}

void send_auth_prompt(int sock) {
  send_line(sock, "AUTH <token>");
}

void send_cmd_denied(int sock, const char* command) {
  if (!command) {
    send_line(sock, "DENIED");
    return;
  }
  char out[64] = {0};
  std::snprintf(out, sizeof(out), "DENIED cmd=%s", command);
  send_line(sock, out);
}

void send_ok(int sock, const char* text) {
  send_line(sock, text ? text : "OK");
}

bool line_contains_printable(char ch) {
  return ch >= 32 && ch <= 126;
}

std::size_t tokenize_line(char* line, char* argv[]) {
  std::size_t argc = 0;
  char* token = strtok(line, " \t");
  while (token && argc < kTokenMax) {
    argv[argc++] = token;
    token = strtok(nullptr, " \t");
  }
  return argc;
}

bool command_allowed_readonly(const char* cmd) {
  if (!cmd) {
    return false;
  }
  if (std::strcmp(cmd, "help") == 0) {
    return true;
  }
  if (std::strcmp(cmd, "status") == 0) {
    return true;
  }
  if (std::strcmp(cmd, "wifi.status") == 0) {
    return true;
  }
  if (std::strcmp(cmd, "time.status") == 0) {
    return true;
  }
#if LUCE_HAS_MDNS
  if (std::strcmp(cmd, "mdns.status") == 0) {
    return true;
  }
#endif
  if (std::strcmp(cmd, "i2c_scan") == 0) {
    return true;
  }
  if (std::strcmp(cmd, "mcp_read") == 0) {
    return true;
  }
  if (std::strcmp(cmd, "buttons") == 0) {
    return true;
  }
  if (std::strcmp(cmd, "cli_net.status") == 0) {
    return true;
  }
  return false;
}

void handle_command(int sock, char* line_buffer) {
  char* argv[kTokenMax] = {nullptr};
  const std::size_t argc = tokenize_line(line_buffer, argv);
  if (argc == 0) {
    send_line(sock, "ERR");
    return;
  }

  if (!g_session.authed) {
    if (!is_auth_line(static_cast<int>(argc), argv)) {
      send_line(sock, "AUTH required");
      return;
    }
    if (argv[1] == nullptr || std::strcmp(argv[1], g_cfg.token) != 0) {
      ++g_session.fail_count;
      ESP_LOGW(kTag, "[CLI_NET] auth fail ip=%s", g_session.peer[0] != '\0' ? g_session.peer : "n/a");
      send_line(sock, "auth fail");
      if (g_session.fail_count >= 3) {
        send_line(sock, "session aborted");
        ESP_LOGW(kTag, "[CLI_NET] session aborted ip=%s", g_session.peer[0] != '\0' ? g_session.peer : "n/a");
        g_session.running = false;
      }
      return;
    }
    g_session.authed = true;
    g_session.fail_count = 0;
    ESP_LOGI(kTag, "[CLI_NET] auth ok ip=%s", g_session.peer[0] != '\0' ? g_session.peer : "n/a");
    send_line(sock, "AUTH ok");
    return;
  }

  if (!command_allowed_readonly(argv[0])) {
    send_cmd_denied(sock, argv[0]);
    ESP_LOGW(kTag, "[CLI_NET] cmd ip=%s cmd=\"%s\" rc=2 denied", g_session.peer[0] != '\0' ? g_session.peer : "n/a",
             argv[0]);
    return;
  }

  int rc = cli_execute_command(static_cast<int>(argc), argv);
  if (rc == 0) {
    send_ok(sock, "OK");
  } else if (rc == 1) {
    send_line(sock, "ERR");
  } else {
    send_line(sock, "DENIED");
  }
}

void close_server(int sock) {
  if (sock >= 0) {
    shutdown(sock, SHUT_RDWR);
    close(sock);
  }
}

void stop_listener() {
  if (g_listener >= 0) {
    close(g_listener);
    g_listener = -1;
  }
}

void cli_net_task(void*) {
  load_cli_net_config();
  ESP_LOGI(kTag, "[CLI_NET] enabled=%d port=%u", g_cfg.enabled ? 1 : 0, g_cfg.port);
  if (!g_cfg.enabled) {
    g_session.running = false;
    return;
  }

  g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (g_listener < 0) {
    ESP_LOGW(kTag, "[CLI_NET] socket() failed");
    return;
  }

  const int reuse = 1;
  setsockopt(g_listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(g_cfg.port);

  if (bind(g_listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    ESP_LOGW(kTag, "[CLI_NET] bind failed");
    stop_listener();
    return;
  }
  if (listen(g_listener, 1) != 0) {
    ESP_LOGW(kTag, "[CLI_NET] listen failed");
    stop_listener();
    return;
  }
  g_session.port = g_cfg.port;
  ESP_LOGI(kTag, "[CLI_NET] listener started on port=%u", g_cfg.port);

  while (true) {
    if (!g_cfg.enabled) {
      break;
    }
    g_session.running = true;
    g_session.authed = false;
    g_session.fail_count = 0;

    fd_set rfds {};
    FD_ZERO(&rfds);
    FD_SET(g_listener, &rfds);
    struct timeval timeout {};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    const int ready = select(g_listener + 1, &rfds, nullptr, nullptr, &timeout);
    if (ready <= 0 || !FD_ISSET(g_listener, &rfds)) {
      continue;
    }

    sockaddr_in peer {};
    socklen_t peer_len = sizeof(peer);
    const int session = accept(g_listener, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (session < 0) {
      continue;
    }
    std::snprintf(g_session.peer, sizeof(g_session.peer), "%s", inet_ntoa(peer.sin_addr));
    ESP_LOGI(kTag, "[CLI_NET] session accepted ip=%s", g_session.peer);
    send_auth_prompt(session);

    char line[kLineMax + 1] = {0};
    std::size_t line_len = 0;
    TickType_t last_activity = xTaskGetTickCount();
    while (true) {
      fd_set rd {};
      FD_ZERO(&rd);
      FD_SET(session, &rd);
      struct timeval to {};
      to.tv_sec = 1;
      to.tv_usec = 0;
      const int r = select(session + 1, &rd, nullptr, nullptr, &to);
      const TickType_t now = xTaskGetTickCount();

      if (r > 0 && FD_ISSET(session, &rd)) {
        char ch[2] = {0};
        const int read = recv(session, ch, 1, 0);
        if (read <= 0) {
          ESP_LOGW(kTag, "[CLI_NET] session closed ip=%s", g_session.peer);
          break;
        }
        last_activity = now;

        if (ch[0] == '\r' || ch[0] == '\n') {
          if (line_len == 0) {
            continue;
          }
          line[line_len] = '\0';
          handle_command(session, line);
          std::memset(line, 0, sizeof(line));
          line_len = 0;
          if (!g_session.running || !g_session.authed || g_session.fail_count >= 3) {
            break;
          }
          continue;
        }

        if (!line_contains_printable(ch[0])) {
          continue;
        }

        if (line_len + 1 < sizeof(line)) {
          line[line_len++] = ch[0];
        } else {
          send_line(session, "line too long");
          line_len = 0;
          std::memset(line, 0, sizeof(line));
        }
        continue;
      }

      const TickType_t age_ms = (now - last_activity) * 1000 / configTICK_RATE_HZ;
      if (age_ms >= g_cfg.idle_timeout_s * 1000ULL) {
        send_line(session, "session timeout");
        ESP_LOGW(kTag, "[CLI_NET] session timeout ip=%s", g_session.peer);
        break;
      }
    }
    close_server(session);
    g_session.authed = false;
    g_session.running = false;
    g_session.fail_count = 0;
    g_session.peer[0] = '\0';
  }

  stop_listener();
}

}  // namespace

void cli_net_startup() {
  if (g_task == nullptr) {
    xTaskCreate(cli_net_task, "cli_net", kTaskStackWords, nullptr, 2, &g_task);
  }
}

void cli_net_status_for_cli() {
  ESP_LOGI(kTag, "cli_net.status enabled=%d running=%d authed=%d session=%d port=%u peer=%s",
           g_cfg.enabled ? 1 : 0, g_listener >= 0 ? 1 : 0, g_session.authed ? 1 : 0, g_session.running ? 1 : 0,
           g_session.port, g_session.peer[0] != '\0' ? g_session.peer : "n/a");
}

#else

void cli_net_startup() {}
void cli_net_status_for_cli() {}

#endif
