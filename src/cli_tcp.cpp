// Stage8 TCP CLI transport.
#include "luce/cli_tcp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>

#if LUCE_HAS_TCP_CLI

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/errno.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs.h"

#include "luce/cli.h"
#include "luce/net_wifi.h"
#include "luce_build.h"
#include "luce/task_budgets.h"
#include "luce/nvs_helpers.h"
#include "luce/led_status.h"

namespace {

constexpr const char* kTag = "[CLI_NET]";
constexpr const char* kCliNetNs = "cli_net";
constexpr std::size_t kLineMax = 255;
constexpr std::size_t kTokenMax = 8;
constexpr std::uint32_t kDefaultPort = 2323;
constexpr std::uint32_t kDefaultIdleTimeoutS = 120;
constexpr std::uint32_t kNetStackWaitMs = 250;
constexpr std::uint8_t kNetStackMaxWaitAttempts = 80;

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

const char* session_peer_or_na() {
  return g_session.peer[0] != '\0' ? g_session.peer : "n/a";
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
  std::uint16_t port = kDefaultPort;
  std::uint32_t timeout = kDefaultIdleTimeoutS;
  if (luce::nvs::read_u8(nvs_handle, "enabled", enabled, 0)) {
    g_cfg.enabled = (enabled != 0);
  }

  if (luce::nvs::read_u16(nvs_handle, "port", port, kDefaultPort) && port != 0) {
    g_cfg.port = port;
  }

  if (luce::nvs::read_u32(nvs_handle, "idle_timeout_s", timeout, kDefaultIdleTimeoutS)) {
    if (timeout > 0 && timeout <= 1800) {
      g_cfg.idle_timeout_s = timeout;
    }
  }

  (void)luce::nvs::read_string(nvs_handle, "token", g_cfg.token, sizeof(g_cfg.token), "luce-cli");
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

bool command_allowed_readonly(const char* cmd) {
  return cli_command_is_readonly(cmd);
}

void handle_command(int sock, char* line_buffer) {
  char* argv[kTokenMax] = {nullptr};
  const std::size_t argc = tokenize_cli_line(line_buffer, argv, kTokenMax);
  if (argc == 0) {
    send_line(sock, "ERR");
    return;
  }

  if (!g_session.authed) {
    if (!is_auth_line(static_cast<int>(argc), argv)) {
      led_status_notify_user_error();
      send_line(sock, "AUTH required");
      return;
    }
    if (argv[1] == nullptr || std::strcmp(argv[1], g_cfg.token) != 0) {
      led_status_notify_user_error();
      ++g_session.fail_count;
      ESP_LOGW(kTag, "[CLI_NET] auth fail ip=%s", session_peer_or_na());
      send_line(sock, "auth fail");
      if (g_session.fail_count >= 3) {
        send_line(sock, "session aborted");
        ESP_LOGW(kTag, "[CLI_NET] session aborted ip=%s", session_peer_or_na());
        g_session.running = false;
      }
      return;
    }
    g_session.authed = true;
    g_session.fail_count = 0;
    ESP_LOGI(kTag, "[CLI_NET] auth ok ip=%s", session_peer_or_na());
    send_line(sock, "AUTH ok");
    return;
  }

  if (!command_allowed_readonly(argv[0])) {
    led_status_notify_user_error();
    send_cmd_denied(sock, argv[0]);
    ESP_LOGW(kTag, "[CLI_NET] cmd ip=%s cmd=\"%s\" rc=2 denied", session_peer_or_na(), argv[0]);
    return;
  }

  int rc = cli_execute_command(static_cast<int>(argc), argv);
  if (rc == 0) {
    led_status_notify_user_input();
  } else {
    led_status_notify_user_error();
  }
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

bool ensure_tcp_stack_ready() {
  for (std::uint8_t attempt = 0; attempt < kNetStackMaxWaitAttempts; ++attempt) {
    const esp_err_t err = esp_netif_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
      return true;
    }
    ESP_LOGW(kTag, "[CLI_NET] esp_netif_init retry=%u err=%s", static_cast<unsigned>(attempt + 1), esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(kNetStackWaitMs));
  }
  ESP_LOGE(kTag, "[CLI_NET] tcp stack not ready after %u attempts", static_cast<unsigned>(kNetStackMaxWaitAttempts));
  return false;
}

int open_listener_socket(std::uint16_t port) {
  int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listener < 0) {
    const int socket_err = errno;
    ESP_LOGW(kTag, "[CLI_NET] socket() failed port=%u errno=%d", static_cast<unsigned>(port), socket_err);
    return -1;
  }

  const int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);

  if (bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    const int bind_err = errno;
    close(listener);
    ESP_LOGW(kTag, "[CLI_NET] bind failed port=%u errno=%d", static_cast<unsigned>(port), bind_err);
    return -1;
  }
  if (listen(listener, 1) != 0) {
    const int listen_err = errno;
    close(listener);
    ESP_LOGW(kTag, "[CLI_NET] listen failed port=%u errno=%d", static_cast<unsigned>(port), listen_err);
    return -1;
  }
  return listener;
}

void cli_net_task(void*) {
  load_cli_net_config();
  ESP_LOGI(kTag, "[CLI_NET] enabled=%d port=%u", g_cfg.enabled ? 1 : 0, g_cfg.port);
  if (!g_cfg.enabled) {
    g_session.running = false;
    // Keep task alive; returning from a task triggers a hard abort.
    while (true) {
      g_session.running = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  if (!ensure_tcp_stack_ready()) {
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  while (true) {
    g_listener = open_listener_socket(g_cfg.port);
    if (g_listener >= 0) {
      break;
    }
    ESP_LOGW(kTag, "[CLI_NET] retrying listener setup");
    vTaskDelay(pdMS_TO_TICKS(kNetStackWaitMs));
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
          ESP_LOGW(kTag, "[CLI_NET] session closed ip=%s", session_peer_or_na());
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
        ESP_LOGW(kTag, "[CLI_NET] session timeout ip=%s", session_peer_or_na());
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
    (void)luce::start_task_once(g_task, cli_net_task, luce::task_budget::kTaskCliNet);
  }
}

bool cli_net_is_enabled() {
  return g_cfg.enabled;
}

bool cli_net_is_listening() {
  return g_listener >= 0;
}

void cli_net_status_for_cli() {
  ESP_LOGI(kTag, "cli_net.status enabled=%d running=%d authed=%d session=%d port=%u peer=%s",
           g_cfg.enabled ? 1 : 0, g_listener >= 0 ? 1 : 0, g_session.authed ? 1 : 0, g_session.running ? 1 : 0,
           g_session.port, session_peer_or_na());
}

#else

bool cli_net_is_enabled() {
  return false;
}

bool cli_net_is_listening() {
  return false;
}

void cli_net_startup() {}
void cli_net_status_for_cli() {}

#endif
