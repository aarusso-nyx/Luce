// cli.cpp: CLI implementation (Serial & Telnet) using a shared input FSM
#include <errno.h>
#include <cstring>
#include <esp_err.h>
#include <esp_console.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"
#include "logger.h"
#include "cli.h"

// Use native ESP-IDF console over UART
#include <driver/uart.h>
#include <esp_vfs_dev.h>
#include <lwip/sockets.h>

// Forward to register commands defined elsewhere
extern void registerCommands();

// Telnet state (remains for Telnet CLI)
static int sock = -1;
static int client_fd = -1;
// Telnet session state
static enum CLIState { CLI_NONE=0, CLI_TELNET=1 } cliState = CLI_NONE;

// Close telnet client
static void closeClient() {
  if (errno != EAGAIN && errno != EWOULDBLOCK) {
    LOGERR("CLI","Telnet","send failed: %s", strerror(errno));
  }

  if (client_fd >= 0) {
    close(client_fd);
    client_fd = -1;
    LOGSYS("CLI","Telnet","Session ended");
    cliState = CLI_NONE; // Reset state
  }
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Print helper: route output to Serial and Telnet
// Remove custom write routing: use standard stdout via VFS UART

// Use standard printf/fputs via stdout/stderr after registering UART VFS

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////

// Legacy character-by-character CLI processing removed in favor of esp_console

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Initialize console and start listening; return false to abort task
bool cliInit() {
  // Setup esp_console
  esp_console_config_t cfg = {
    .max_cmdline_length = config::CLI_MAX_LINE,
    .max_cmdline_args   = 8,
    .hint_color         = 33,
    .hint_bold          = true
  };

  ESP_ERROR_CHECK(esp_console_init(&cfg));
  registerCommands();
  ESP_ERROR_CHECK(esp_console_register_help_command());
  // Register UART driver as VFS for console I/O
  esp_vfs_dev_uart_use_driver(UART_NUM_0);
  // Disable buffering on stdin/stdout
  setvbuf(stdin, NULL, _IONBF, 0);
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  // Serial banner
  LOGINFO("CLI","Init","Starting CLI...");
  LOGINFO("CLI","Init","Type ESC to enter Serial CLI mode");
  return true;
}

// Poll Serial for CLI input (ESC to enter/exit); return false to abort task
// Simple console loop: read full lines from stdin and dispatch
bool cliLoop() {
  static char line[config::CLI_MAX_LINE];
  printf(config::CLI_PROMPT);
  if (fgets(line, sizeof(line), stdin) != NULL) {
    int ret = 0;
    esp_console_run(line, &ret);
    printf("\n");
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
// Setup Telnet server socket; return false to abort task
// Telnet server remains unchanged
bool cliServer() {
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    LOGERR("CLI","Telnet","socket");
    return false;
  }
  
  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(0); // Bind to all interfaces
  addr.sin_port = htons(config::TELNET_PORT);

  if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(sock, 1) < 0) {
    LOGERR("CLI","Telnet","bind/listen");
    return false;
  }
  
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL,0) | O_NONBLOCK);
  LOGSYS("CLI","Telnet","Server started on port %d", config::TELNET_PORT);
  return true;
}

// Poll Telnet for CLI input (ESC to exit); return false to abort task
bool cliTelnet() {
  // if (sock < 0) {
  //   cliServer(); // Ensure server is initialized

  //   if (sock < 0) {
  //     LOGERR("CLI","Telnet","Server not initialized");
  //     vTaskDelete(NULL); // Terminate task if running
  //     return;
  //   }
  // }

  if ( cliState == CLI_NONE ) {    
  // if ( client_fd < 0 && sock >= 0 ) {
    // Start Telnet server if not already running
    // LOGINFO("CLI","Telnet","Server Ready");
    cliState = CLI_TELNET; // Switch to Telnet mode
    
    struct sockaddr_in ca; 
    socklen_t len=sizeof(ca);
    int fd = accept(sock, (struct sockaddr*)&ca, &len);
    if (fd < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        LOGWARN("CLI","Telnet","accept errno=%d", errno);
      return true;
    }
    
    LOGSYS("CLI","Telnet","Session started");
    const char* w = "\r\n=== Telnet CLI ===\r\n";
    client_fd = fd;
    
    send(client_fd, w, strlen(w), 0);
    send(client_fd, config::CLI_PROMPT, strlen(config::CLI_PROMPT), 0);
    fcntl(client_fd, F_SETFL, fcntl(client_fd,F_GETFL,0) | O_NONBLOCK);

  } 
  
  if ( cliState == CLI_TELNET && client_fd >= 0 ) {
    int c;
    if (recv(client_fd, &c, 1, 0) < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        closeClient();
      }
    } else {
      printf("%c (%d)\t", c, (int)c); // Echo to stdout
      if (c == 4) { // Ctrl-D: exit session
        closeClient();
      }

      send(client_fd, &c, 1, 0); // Echo back to Telnet client
      if (c == '\n' || c == '\r') {
        send(client_fd, config::CLI_PROMPT, strlen(config::CLI_PROMPT), 0);
      }
  
      // process(c);
    }
  }
  // Other states: ignored
  return true;
}