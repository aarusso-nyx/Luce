#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <ctime>

#include "luce_build.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#if LUCE_HAS_NVS
#include "nvs.h"
#include "nvs_flash.h"
#endif

#if LUCE_HAS_I2C
#include "driver/i2c.h"
#endif

#if LUCE_HAS_WIFI
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#endif

#if LUCE_HAS_NTP
#include "esp_sntp.h"
#endif

#if LUCE_HAS_MDNS
#include "lwip/apps/mdns.h"
#endif

#if LUCE_HAS_TCP_CLI
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#endif
#if LUCE_HAS_MQTT
#include "mqtt_client.h"
#endif
#if LUCE_HAS_HTTP
#include "esp_https_server.h"
#endif

#if LUCE_HAS_CLI
#include "driver/uart.h"
#endif

namespace {

constexpr const char* kTag = "luce_boot";
void blink_alive();

#ifndef LUCE_STAGE4_DIAG
#define LUCE_STAGE4_DIAG 1
#endif

#ifndef LUCE_STAGE4_LCD
#define LUCE_STAGE4_LCD 1
#endif

#ifndef LUCE_STAGE4_CLI
#define LUCE_STAGE4_CLI 1
#endif

#ifndef LUCE_DEBUG_STAGE4_DIAG
#define LUCE_DEBUG_STAGE4_DIAG 1
#endif

#ifndef LUCE_WIFI_AUTOSTART
#define LUCE_WIFI_AUTOSTART 0
#endif

constexpr std::size_t kDiagTaskStackWords = 8192;
constexpr std::size_t kCliTaskStackWords = 6144;
constexpr std::size_t kBlinkTaskStackWords = 2048;
constexpr std::size_t kMqttTaskStackWords = 6144;
#if LUCE_HAS_HTTP
constexpr std::size_t kHttpServerStackWords = 8192;
#endif
#if LUCE_HAS_MQTT
constexpr uint32_t kMqttMinRetryBackoffMs = 1000;
#endif

TaskHandle_t g_diag_task = nullptr;
TaskHandle_t g_cli_task = nullptr;
#if LUCE_HAS_TCP_CLI
TaskHandle_t g_cli_net_task = nullptr;
#endif
#if LUCE_HAS_MQTT
TaskHandle_t g_mqtt_task = nullptr;
#endif

extern bool g_i2c_initialized;
extern bool g_mcp_available;
extern uint8_t g_relay_mask;
extern uint8_t g_button_mask;
#if LUCE_HAS_LCD
extern bool g_lcd_present;
#endif

enum class InitPathStatus : uint8_t {
  kUnknown = 0,
  kSuccess,
  kFailure,
};

struct InitPathResult {
  bool ok;
  esp_err_t error;
  InitPathStatus status;
};

enum class McpMaskFormat : uint8_t {
  kRuntime = 0,
  kStatusCommand,
};

#if LUCE_HAS_WIFI
enum class WifiState : uint8_t {
  kDisabled = 0,
  kInit,
  kConnecting,
  kGotIp,
  kBackoff,
  kStopped,
};

enum class WifiEventType : uint8_t {
  kStart,
  kScanDone,
  kConnected,
  kGotIp,
  kDisconnected,
  kStop,
};

struct WifiEvent {
  WifiEventType type;
  int32_t reason;
};

struct WifiConfig {
  bool enabled = false;
  char ssid[33] = {0};
  char pass[65] = {0};
  char hostname[33] = {0};
  uint32_t max_retries = 6;
  uint32_t backoff_min_ms = 250;
  uint32_t backoff_max_ms = 5000;
};

struct WifiStatusSnapshot {
  WifiState state = WifiState::kDisabled;
  bool enabled = false;
  uint32_t attempt = 0;
  uint32_t max_retries = 0;
  uint32_t backoff_ms = 0;
  uint32_t next_backoff_ms = 0;
  char ip[16] = "0.0.0.0";
  char gw[16] = "0.0.0.0";
  char mask[16] = "0.0.0.0";
  int rssi = 0;
};

constexpr char kWifiConfigNamespace[] = "wifi";
constexpr std::size_t kWifiScanResultLimit = 4;
constexpr uint32_t kWifiQueueLength = 16;
constexpr TickType_t kWifiNoopDelayMs = 100;

void log_wifi_status();
void log_wifi_status_snapshot(const WifiStatusSnapshot& snapshot);
void wifi_startup();
void wifi_task(void* arg);
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void wifi_scan_for_cli();
void wifi_status_for_cli();
bool wifi_has_ip();
#endif

#if LUCE_HAS_MDNS
enum class MdnsState : uint8_t {
  kDisabled = 0,
  kDisabledByConfig,
  kInit,
  kStarted,
  kFailed,
};

struct MdnsConfig {
  bool enabled = false;
  char hostname[33] = {0};
  char instance[33] = "Luce Stage";
};

void mdns_log_status();
void mdns_startup();
void mdns_handle_wifi_got_ip();
void mdns_handle_wifi_lost_ip();
bool mdns_load_config();
#endif

#if LUCE_HAS_MQTT
enum class MqttState : uint8_t {
  kDisabled = 0,
  kInitialized,
  kConnecting,
  kConnected,
  kBackoff,
  kFailed,
};

struct MqttConfig {
  bool enabled = false;
  char uri[128] = "mqtt://localhost:1883";
  char client_id[33] = {0};
  char base_topic[64] = "luce/stage9";
  char username[33] = {0};
  char password[65] = {0};
  bool tls_enabled = false;
  char ca_pem_source[16] = "embedded";
  char ca_pem[1536] = {0};
  uint32_t qos = 0;
  uint32_t keepalive_s = 120;
};

struct MqttRuntime {
  esp_mqtt_client_handle_t client = nullptr;
  MqttState state = MqttState::kDisabled;
  bool connected = false;
  TickType_t next_retry_tick = 0;
  TickType_t last_connect_tick = 0;
  TickType_t last_disconnect_tick = 0;
  uint32_t retry_backoff_ms = kMqttMinRetryBackoffMs;
  uint32_t reconnects = 0;
  uint32_t publish_count = 0;
  uint32_t last_publish_ms = 0;
  time_t last_pub_unix = 0;
  int last_pub_rc = 0;
  uint32_t last_pub_payload_size = 0;
  uint32_t last_publish_latency_ms = 0;
  bool last_connected = false;
  bool last_disconnected = false;
  char last_reason[64] = "uninitialized";
  char last_state[16] = "DISABLED";
  char last_topic[96] = "(none)";
  uint32_t connect_count = 0;
  uint32_t publish_backoff_failures = 0;
  char effective_uri[128] = {0};
  char last_will_topic[128] = {0};
};

void mqtt_set_defaults(MqttConfig& config);
void mqtt_startup();
void mqtt_task(void* arg);
bool mqtt_load_config(MqttConfig& config);
bool mqtt_load_dev_ca_pem(char* out, std::size_t out_size);
void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id,
                       void* event_data);
void mqtt_handle_wifi_got_ip();
void mqtt_handle_wifi_lost_ip();
void mqtt_log_status_line();
int mqtt_publish_payload(const char* topic, const char* payload, int payload_len, uint8_t qos = 0);
int mqtt_publish_status(bool include_time);
int mqtt_publish_test_payload();
#endif

#if LUCE_HAS_NTP
enum class NtpState : uint8_t;
struct NtpRuntimeState;
extern NtpRuntimeState g_ntp_runtime;
const char* ntp_state_name(NtpState state);
std::size_t ntp_format_utc(char* out, std::size_t out_size, time_t epoch_seconds);
time_t ntp_last_sync_unix();
NtpState ntp_state_snapshot();
#endif

#if LUCE_HAS_HTTP
enum class HttpState : uint8_t {
  kDisabled = 0,
  kInit,
  kReady,
  kStarted,
  kFailed,
};

struct HttpConfig {
  bool enabled = false;
  uint16_t port = 443;
  bool tls_dev_mode = false;
  char token[64] = {0};
};

struct HttpRuntime {
  HttpState state = HttpState::kDisabled;
  bool started = false;
  uint32_t request_count = 0;
  uint32_t unauthorized_count = 0;
  uint32_t error_count = 0;
};

bool http_load_config(HttpConfig& config);
void http_startup();
void http_start_if_ready();
void http_handle_wifi_got_ip();
void http_handle_wifi_lost_ip();
void http_shutdown();
void cli_cmd_http_status();
#endif

#if LUCE_HAS_NTP
enum class NtpState : uint8_t {
  kDisabled = 0,
  kUnsynced,
  kSyncing,
  kSynced,
  kFailed,
};

struct NtpConfig {
  bool enabled = false;
  char server1[33] = "pool.ntp.org";
  char server2[33] = "time.google.com";
  char server3[33] = "";
  uint32_t sync_timeout_s = 30;
  uint32_t sync_interval_s = 3600;
};

void ntp_log_status_line();
const char* ntp_state_name(NtpState state);
void ntp_set_state(NtpState next_state, const char* reason);
void ntp_startup();
void ntp_task(void* arg);
bool ntp_has_time();
#endif

const char* reset_reason_to_string(esp_reset_reason_t reason);
std::size_t init_path_reset_reason_line(char* out, std::size_t out_size,
                                       esp_reset_reason_t reason);

void log_heap_integrity(const char* context) {
#if LUCE_DEBUG_STAGE4_DIAG
  if (!context) {
    context = "unknown";
  }
  const bool ok = heap_caps_check_integrity_all(true);
  ESP_LOGI(kTag, "Heap integrity (%s): %s", context, ok ? "OK" : "CORRUPTED");
#else
  (void)context;
#endif
}

std::size_t format_mcp_mask_line(char* out, std::size_t out_size, uint8_t relay_mask,
                                uint8_t button_mask, McpMaskFormat format = McpMaskFormat::kRuntime) {
  switch (format) {
    case McpMaskFormat::kStatusCommand:
      return std::snprintf(out, out_size, "relay_mask=0x%02X button_mask=0x%02X", relay_mask,
                           button_mask);
    case McpMaskFormat::kRuntime:
    default:
      return std::snprintf(out, out_size, "REL:0x%02X BTN:0x%02X", relay_mask, button_mask);
  }
}

const char* init_status_name(InitPathStatus status) {
  switch (status) {
    case InitPathStatus::kSuccess:
      return "success";
    case InitPathStatus::kFailure:
      return "failure";
    case InitPathStatus::kUnknown:
    default:
      return "unknown";
  }
}

InitPathResult init_result_success() {
  return InitPathResult{true, ESP_OK, InitPathStatus::kSuccess};
}

InitPathResult init_result_failure(esp_err_t error) {
  return InitPathResult{false, error, InitPathStatus::kFailure};
}

void log_startup_banner() {
  char reason_line[48];
  init_path_reset_reason_line(reason_line, sizeof(reason_line), esp_reset_reason());

  ESP_LOGI(kTag, "LUCE STAGE%d", LUCE_STAGE);
  ESP_LOGI(kTag, "Build timestamp: %s %s", __DATE__, __TIME__);
  ESP_LOGI(kTag, "Project version: %s", LUCE_PROJECT_VERSION);
  ESP_LOGI(kTag, "Git SHA: %s", LUCE_GIT_SHA);
  ESP_LOGI(kTag, "Reset reason: %s", reason_line);
}

void log_status_health_lines() {
  char reason_line[48];
  char mask_line[48];
  init_path_reset_reason_line(reason_line, sizeof(reason_line), esp_reset_reason());
  format_mcp_mask_line(mask_line, sizeof(mask_line), g_relay_mask, g_button_mask,
                       McpMaskFormat::kStatusCommand);

  ESP_LOGI(kTag, "status: stage=%d reset=%s uptime=%llus", LUCE_STAGE, reason_line,
           static_cast<long long>(esp_timer_get_time() / 1000000ULL));
  ESP_LOGI(kTag, "status: heap_free=%u min_free=%u", heap_caps_get_free_size(MALLOC_CAP_8BIT),
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "status: task_watermark_words=%u", uxTaskGetStackHighWaterMark(nullptr));
  ESP_LOGI(kTag, "status: feature i2c=%d lcd=%d cli=%d wifi=%d ntp=%d mdns=%d mqtt=%d http=%d",
           LUCE_HAS_I2C, LUCE_HAS_LCD, LUCE_HAS_CLI, LUCE_HAS_WIFI, LUCE_HAS_NTP, LUCE_HAS_MDNS,
           LUCE_HAS_MQTT, LUCE_HAS_HTTP);
#if LUCE_HAS_I2C
  ESP_LOGI(kTag, "status: i2c_init=%d mcp=%d %s", g_i2c_initialized, g_mcp_available, mask_line);
#endif
}

std::size_t init_path_reset_reason_line(char* out, std::size_t out_size, esp_reset_reason_t reason) {
  return std::snprintf(out, out_size, "%s (%d)", reset_reason_to_string(reason),
                       static_cast<int>(reason));
}

void log_runtime_status_line(uint64_t uptime_s, bool i2c_ok, bool mcp_ok, uint8_t relay_mask,
                            uint8_t button_mask) {
  char mask_line[32];
  format_mcp_mask_line(mask_line, sizeof(mask_line), relay_mask, button_mask);
  ESP_LOGI(kTag, "LUCE S3 %llu | I2C:%s MCP:%s %s",
           static_cast<unsigned long long>(uptime_s), i2c_ok ? "ok" : "no",
           mcp_ok ? "ok" : "no", mask_line);
}

void log_stage4_watermarks(const char* context) {
#if LUCE_DEBUG_STAGE4_DIAG
  if (!context) {
    context = "unknown";
  }
  UBaseType_t cli = 0;
  UBaseType_t diag = 0;
#if LUCE_HAS_CLI
  if (g_cli_task) {
    cli = uxTaskGetStackHighWaterMark(g_cli_task);
  }
#endif
#if LUCE_HAS_I2C
  if (g_diag_task) {
    diag = uxTaskGetStackHighWaterMark(g_diag_task);
  }
#endif
  const UBaseType_t now = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGI(kTag, "Stack watermark (%s): cli=%u diag=%u current=%u", context, cli, diag, now);
#else
  (void)context;
#endif
}

const char* reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SOFTWARE";
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
      return "UNKNOWN";
  }
}

void print_chip_info() {
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  ESP_LOGI(kTag, "Chip: model=%d revision=%d cores=%d", chip_info.model,
           chip_info.revision, chip_info.cores);
  ESP_LOGI(kTag, "Features: %s%s%s%s%s", chip_info.features & CHIP_FEATURE_WIFI_BGN ? "WIFI " : "",
           chip_info.features & CHIP_FEATURE_BT ? "BT " : "",
           chip_info.features & CHIP_FEATURE_BLE ? "BLE " : "",
           chip_info.features & CHIP_FEATURE_EMB_FLASH ? "EMB_FLASH " : "",
           chip_info.features & CHIP_FEATURE_EMB_PSRAM ? "EMB_PSRAM " : "");
  ESP_LOGI(kTag, "CPU frequency: %u MHz", esp_clk_cpu_freq() / 1000000ULL);
}

void print_app_info() {
  const esp_app_desc_t* app_desc = esp_app_get_description();
  if (!app_desc) {
    ESP_LOGW(kTag, "esp_app_get_description() returned null");
    return;
  }
  ESP_LOGI(kTag, "App version: %s", app_desc->version);
  ESP_LOGI(kTag, "Project name: %s", app_desc->project_name);
  ESP_LOGI(kTag, "Secure version: %d", app_desc->secure_version);
  ESP_LOGI(kTag, "App compile time: %s %s", app_desc->time, app_desc->date);
  ESP_LOGI(kTag, "ESP-IDF version: %s", app_desc->idf_ver);
  ESP_LOGI(kTag, "ELF SHA256: %s", esp_app_get_elf_sha256_str());
}

void print_partition_summary() {
  ESP_LOGI(kTag, "Partition map:");
  esp_partition_iterator_t part_it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  if (!part_it) {
    ESP_LOGW(kTag, "  no partition table entries found");
    return;
  }

  // esp_partition_next() may free iterator state when it reaches end of list.
  // Keep a single iterator variable and allow NULL-safe release at the end.
  for (esp_partition_iterator_t it = part_it; it;) {
    const esp_partition_t* partition = esp_partition_get(it);
    if (partition) {
      ESP_LOGI(kTag, "  type=%d subtype=%d label=%s offset=0x%08" PRIx32 " size=0x%08" PRIx32,
               partition->type, partition->subtype, partition->label, partition->address, partition->size);
    }
    it = esp_partition_next(it);
  }
}

void print_heap_stats() {
  ESP_LOGI(kTag, "Heap free: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "Heap min-free: %u bytes",
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  ESP_LOGI(kTag, "Task watermark (current): %u words", uxTaskGetStackHighWaterMark(nullptr));
}

#if LUCE_HAS_WIFI
const char* wifi_state_to_string(WifiState state) {
  switch (state) {
    case WifiState::kDisabled:
      return "DISABLED";
    case WifiState::kInit:
      return "INIT";
    case WifiState::kConnecting:
      return "CONNECTING";
    case WifiState::kGotIp:
      return "GOT_IP";
    case WifiState::kBackoff:
      return "BACKOFF";
    case WifiState::kStopped:
      return "STOPPED";
    default:
      return "UNKNOWN";
  }
}

constexpr uint32_t kDefaultWifiMaxRetries = 6;
constexpr uint32_t kDefaultWifiBackoffMinMs = 250;
constexpr uint32_t kDefaultWifiBackoffMaxMs = 5000;
constexpr uint32_t kDefaultWifiScanCount = 4;
constexpr TickType_t kWifiStatusIntervalMs = 5000;
constexpr TickType_t kWifiIdlePollMs = 250;
constexpr int32_t kRssiInvalid = -1000;

void log_wifi_status();
void log_wifi_status_snapshot(const WifiStatusSnapshot& snapshot);
void wifi_log_nvs_line(const char* key, bool present, const char* value, bool mask_value = false);
void wifi_queue_send(WifiEventType type, int32_t reason);
bool wifi_send_connect();
void wifi_schedule_backoff();
bool wifi_load_config(WifiConfig& config);
bool wifi_init_wifi_stack();
void wifi_status_for_cli();
void wifi_scan_for_cli();

static QueueHandle_t g_wifi_event_queue = nullptr;
static TaskHandle_t g_wifi_manager_task = nullptr;
static esp_event_handler_instance_t g_wifi_event_any_instance = nullptr;
static esp_event_handler_instance_t g_wifi_event_ip_instance = nullptr;
static esp_netif_t* g_wifi_netif = nullptr;
static bool g_wifi_stack_ready = false;
static bool g_wifi_has_ip = false;
static int g_wifi_last_rssi = kRssiInvalid;
static int32_t g_wifi_last_error_reason = 0;
static uint32_t g_wifi_attempt = 0;
static uint32_t g_wifi_backoff_ms = 0;
static uint32_t g_wifi_next_backoff_ms = 0;
static TickType_t g_wifi_next_connect_tick = 0;
static uint32_t g_wifi_max_retries = kDefaultWifiMaxRetries;
static uint32_t g_wifi_backoff_min_ms = kDefaultWifiBackoffMinMs;
static uint32_t g_wifi_backoff_max_ms = kDefaultWifiBackoffMaxMs;
static char g_wifi_ip[16] = "0.0.0.0";
static char g_wifi_gw[16] = "0.0.0.0";
static char g_wifi_mask[16] = "0.0.0.0";
static bool g_wifi_enabled = false;
static bool g_wifi_initialized = false;
static bool g_wifi_configured = false;
static WifiState g_wifi_state = WifiState::kDisabled;
static WifiConfig g_wifi_config {};

void format_ip_addr(const esp_ip4_addr_t& src, char* dst, std::size_t dst_len) {
  if (!dst || dst_len == 0) {
    return;
  }
  const char* text = ip4addr_ntoa(reinterpret_cast<const ip4_addr_t*>(&src));
  if (!text) {
    std::strncpy(dst, "0.0.0.0", dst_len - 1);
    dst[dst_len - 1] = '\0';
    return;
  }
  std::snprintf(dst, dst_len, "%s", text);
}

void log_wifi_lifecycle_transition(WifiState next_state, const char* reason) {
  const char* state_label = wifi_state_to_string(next_state);
  if (reason) {
    ESP_LOGI(kTag, "[WIFI][LIFECYCLE] state=%s attempt=%lu reason=%s", state_label,
             static_cast<unsigned long>(g_wifi_attempt), reason);
  } else {
    ESP_LOGI(kTag, "[WIFI][LIFECYCLE] state=%s attempt=%lu", state_label,
             static_cast<unsigned long>(g_wifi_attempt));
  }
}

void set_wifi_state(WifiState next_state, const char* reason) {
  if (g_wifi_state == next_state) {
    return;
  }
  g_wifi_state = next_state;
  log_wifi_lifecycle_transition(next_state, reason);
}

void wifi_log_nvs_line(const char* key, bool present, const char* value, bool mask_value) {
  if (!key) {
    return;
  }
  if (mask_value) {
    const char* masked = (value && value[0] != '\0') ? "********" : "(empty)";
    ESP_LOGI(kTag, "[WIFI][NVS] key=%s present=%d value=%s", key, present ? 1 : 0, masked);
    return;
  }
  ESP_LOGI(kTag, "[WIFI][NVS] key=%s present=%d value=%s", key, present ? 1 : 0,
           value ? value : "");
}

void log_wifi_status_snapshot(const WifiStatusSnapshot& snapshot) {
  ESP_LOGI(kTag, "[WIFI][STATUS] state=%s enabled=%d attempt=%lu max=%lu backoff_ms=%lu next_ms=%lu ip=%s gw=%s mask=%s rssi=%d",
           wifi_state_to_string(snapshot.state), snapshot.enabled ? 1 : 0,
           static_cast<unsigned long>(snapshot.attempt),
           static_cast<unsigned long>(snapshot.max_retries),
           static_cast<unsigned long>(snapshot.backoff_ms),
           static_cast<unsigned long>(snapshot.next_backoff_ms), snapshot.ip, snapshot.gw, snapshot.mask,
           snapshot.rssi);
}

void log_wifi_status() {
  WifiStatusSnapshot snapshot;
  snapshot.state = g_wifi_state;
  snapshot.enabled = g_wifi_enabled;
  snapshot.attempt = g_wifi_attempt;
  snapshot.max_retries = g_wifi_max_retries;
  snapshot.backoff_ms = g_wifi_backoff_ms;
  snapshot.next_backoff_ms = g_wifi_next_backoff_ms;
  snapshot.rssi = g_wifi_last_rssi;
  std::strncpy(snapshot.ip, g_wifi_ip, sizeof(snapshot.ip) - 1);
  snapshot.ip[sizeof(snapshot.ip) - 1] = '\0';
  std::strncpy(snapshot.gw, g_wifi_gw, sizeof(snapshot.gw) - 1);
  snapshot.gw[sizeof(snapshot.gw) - 1] = '\0';
  std::strncpy(snapshot.mask, g_wifi_mask, sizeof(snapshot.mask) - 1);
  snapshot.mask[sizeof(snapshot.mask) - 1] = '\0';
  log_wifi_status_snapshot(snapshot);
}

void wifi_queue_send(WifiEventType type, int32_t reason) {
  if (!g_wifi_event_queue) {
    return;
  }
  const WifiEvent event{type, reason};
  (void)xQueueSend(g_wifi_event_queue, &event, 0);
}

void wifi_event_handler(void*,
                       esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT) {
    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
      const auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
      if (event) {
        wifi_queue_send(WifiEventType::kDisconnected, static_cast<int32_t>(event->reason));
      } else {
        wifi_queue_send(WifiEventType::kDisconnected, 0);
      }
      return;
    }
    if (event_id == WIFI_EVENT_STA_CONNECTED) {
      wifi_queue_send(WifiEventType::kConnected, 0);
      return;
    }
    if (event_id == WIFI_EVENT_STA_START) {
      wifi_queue_send(WifiEventType::kStart, 0);
      return;
    }
  }

  if (event_base == IP_EVENT) {
  if (event_id == IP_EVENT_STA_GOT_IP) {
      const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
      if (event) {
        g_wifi_has_ip = true;
        format_ip_addr(event->ip_info.ip, g_wifi_ip, sizeof(g_wifi_ip));
        format_ip_addr(event->ip_info.gw, g_wifi_gw, sizeof(g_wifi_gw));
        format_ip_addr(event->ip_info.netmask, g_wifi_mask, sizeof(g_wifi_mask));
      }
  #if LUCE_HAS_MDNS
      mdns_handle_wifi_got_ip();
  #endif
  #if LUCE_HAS_MQTT
      mqtt_handle_wifi_got_ip();
  #endif
  #if LUCE_HAS_HTTP
      http_handle_wifi_got_ip();
  #endif
      wifi_queue_send(WifiEventType::kGotIp, 0);
      return;
    }
    if (event_id == IP_EVENT_STA_LOST_IP) {
      g_wifi_has_ip = false;
  #if LUCE_HAS_MDNS
      mdns_handle_wifi_lost_ip();
  #endif
  #if LUCE_HAS_MQTT
      mqtt_handle_wifi_lost_ip();
  #endif
  #if LUCE_HAS_HTTP
      http_handle_wifi_lost_ip();
  #endif
      wifi_queue_send(WifiEventType::kDisconnected, 0);
    }
  }
}

bool wifi_read_u32_key(nvs_handle_t handle, const char* key, uint32_t* value, bool* present) {
  if (!handle || !value || !key || !present) {
    return false;
  }
  uint32_t candidate = 0;
  const esp_err_t err = nvs_get_u32(handle, key, &candidate);
  if (err == ESP_OK) {
    *value = candidate;
    *present = true;
    return true;
  }
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    *present = false;
    return false;
  }
  ESP_LOGW(kTag, "[WIFI][NVS] read error key=%s err=%s", key, esp_err_to_name(err));
  *present = false;
  return false;
}

bool wifi_read_u8_key(nvs_handle_t handle, const char* key, uint8_t* value, bool* present) {
  if (!handle || !value || !key || !present) {
    return false;
  }
  uint8_t candidate = 0;
  const esp_err_t err = nvs_get_u8(handle, key, &candidate);
  if (err == ESP_OK) {
    *value = candidate;
    *present = true;
    return true;
  }
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    *present = false;
    return false;
  }
  ESP_LOGW(kTag, "[WIFI][NVS] read error key=%s err=%s", key, esp_err_to_name(err));
  *present = false;
  return false;
}

bool wifi_read_string_key(nvs_handle_t handle, const char* key, char* out, std::size_t out_size,
                         bool* present) {
  if (!key || !out || out_size == 0 || !present) {
    return false;
  }
  *present = false;
  out[0] = '\0';

  std::size_t required = out_size;
  const esp_err_t err = nvs_get_str(handle, key, out, &required);
  if (err == ESP_OK) {
    *present = true;
    return true;
  }
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  ESP_LOGW(kTag, "[WIFI][NVS] read error key=%s err=%s", key, esp_err_to_name(err));
  return false;
}

uint32_t clamp_minmax_u32(uint32_t value, uint32_t min, uint32_t max) {
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}

bool wifi_load_config(WifiConfig& config) {
  char tmp[24];
  config = {};
  std::strncpy(config.hostname, "luce-esp32", sizeof(config.hostname) - 1);
  config.enabled = (LUCE_WIFI_AUTOSTART != 0);

  nvs_handle_t handle = 0;
  const esp_err_t init_err = nvs_open(kWifiConfigNamespace, NVS_READONLY, &handle);
  if (init_err != ESP_OK) {
    wifi_log_nvs_line("ssid", false, "(none)", false);
    wifi_log_nvs_line("pass", false, "(none)", true);
    wifi_log_nvs_line("hostname", false, "(none)", false);
    wifi_log_nvs_line("enabled", false, "0", false);
    wifi_log_nvs_line("max_retries", false, "0", false);
    wifi_log_nvs_line("backoff_min_ms", false, "0", false);
    wifi_log_nvs_line("backoff_max_ms", false, "0", false);
    return false;
  }

  bool present_ssid = false;
  bool present_pass = false;
  bool present_hostname = false;
  bool present_enabled = false;
  bool present_retries = false;
  bool present_backoff_min = false;
  bool present_backoff_max = false;

  wifi_read_string_key(handle, "ssid", config.ssid, sizeof(config.ssid), &present_ssid);
  wifi_log_nvs_line("ssid", present_ssid, config.ssid, false);
  if (!present_ssid) {
    config.ssid[0] = '\0';
  }

  wifi_read_string_key(handle, "pass", config.pass, sizeof(config.pass), &present_pass);
  if (!present_pass) {
    config.pass[0] = '\0';
  }
  wifi_log_nvs_line("pass", present_pass, config.pass, true);

  wifi_read_string_key(handle, "hostname", config.hostname, sizeof(config.hostname),
                      &present_hostname);
  if (!present_hostname) {
    std::strncpy(config.hostname, "luce-esp32", sizeof(config.hostname) - 1);
  }
  wifi_log_nvs_line("hostname", present_hostname, config.hostname, false);

  uint8_t enabled = 0;
  if (wifi_read_u8_key(handle, "enabled", &enabled, &present_enabled) &&
      (enabled == 0 || enabled == 1)) {
    config.enabled = (enabled != 0);
  } else if (present_enabled) {
    ESP_LOGW(kTag, "[WIFI][NVS] key=enabled value invalid, using default 0");
  }
  wifi_log_nvs_line("enabled", present_enabled, config.enabled ? "1" : "0", false);

  if (wifi_read_u32_key(handle, "max_retries", &config.max_retries, &present_retries)) {
    config.max_retries = clamp_minmax_u32(config.max_retries, 0, 1000);
  } else if (!present_retries) {
    config.max_retries = kDefaultWifiMaxRetries;
  }
  std::snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(config.max_retries));
  wifi_log_nvs_line("max_retries", present_retries, tmp, false);

  if (wifi_read_u32_key(handle, "backoff_min_ms", &config.backoff_min_ms, &present_backoff_min)) {
    if (config.backoff_min_ms > 60000) {
      config.backoff_min_ms = kDefaultWifiBackoffMinMs;
    }
  } else {
    config.backoff_min_ms = kDefaultWifiBackoffMinMs;
  }

  if (wifi_read_u32_key(handle, "backoff_max_ms", &config.backoff_max_ms, &present_backoff_max)) {
    if (config.backoff_max_ms < 100 || config.backoff_max_ms > 60000) {
      config.backoff_max_ms = kDefaultWifiBackoffMaxMs;
    }
  } else {
    config.backoff_max_ms = kDefaultWifiBackoffMaxMs;
  }

  if (config.backoff_min_ms > config.backoff_max_ms) {
    ESP_LOGW(kTag, "[WIFI][NVS] invalid pair: backoff_min_ms=%lu > backoff_max_ms=%lu; correcting",
             static_cast<unsigned long>(config.backoff_min_ms),
             static_cast<unsigned long>(config.backoff_max_ms));
    config.backoff_min_ms = kDefaultWifiBackoffMinMs;
    config.backoff_max_ms = kDefaultWifiBackoffMaxMs;
  }

  std::snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(config.backoff_min_ms));
  wifi_log_nvs_line("backoff_min_ms", present_backoff_min, tmp, false);
  std::snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(config.backoff_max_ms));
  wifi_log_nvs_line("backoff_max_ms", present_backoff_max, tmp, false);

  nvs_close(handle);
  return true;
}

bool wifi_init_wifi_stack() {
  if (g_wifi_stack_ready) {
    return true;
  }

  g_wifi_backoff_ms = g_wifi_config.backoff_min_ms;
  g_wifi_max_retries = g_wifi_config.max_retries;
  g_wifi_backoff_min_ms = g_wifi_config.backoff_min_ms;
  g_wifi_backoff_max_ms = g_wifi_config.backoff_max_ms;

  const esp_err_t netif_err = esp_netif_init();
  if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "[WIFI][ERROR] esp_netif_init failed: %s", esp_err_to_name(netif_err));
    return false;
  }

  const esp_err_t loop_err = esp_event_loop_create_default();
  if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "[WIFI][ERROR] esp_event_loop_create_default failed: %s",
             esp_err_to_name(loop_err));
    return false;
  }

  g_wifi_netif = esp_netif_create_default_wifi_sta();
  if (!g_wifi_netif) {
    ESP_LOGW(kTag, "[WIFI][ERROR] esp_netif_create_default_wifi_sta returned null");
    return false;
  }

  const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  const esp_err_t wifi_init_err = esp_wifi_init(&init_cfg);
  if (wifi_init_err != ESP_OK && wifi_init_err != ESP_ERR_WIFI_INIT_STATE) {
    ESP_LOGW(kTag, "[WIFI][ERROR] esp_wifi_init failed: %s", esp_err_to_name(wifi_init_err));
    return false;
  }

  if (g_wifi_event_any_instance == nullptr) {
    const esp_err_t reg_wifi =
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                          &g_wifi_event_any_instance);
    if (reg_wifi != ESP_OK) {
      ESP_LOGW(kTag, "[WIFI][ERROR] failed to register Wi-Fi event handler: %s",
               esp_err_to_name(reg_wifi));
      return false;
    }
  }

  if (g_wifi_event_ip_instance == nullptr) {
    const esp_err_t reg_ip = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                                 &wifi_event_handler, nullptr,
                                                                 &g_wifi_event_ip_instance);
    if (reg_ip != ESP_OK) {
      ESP_LOGW(kTag, "[WIFI][ERROR] failed to register IP event handler: %s",
               esp_err_to_name(reg_ip));
      return false;
    }
  }

  const esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (mode_err != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][ERROR] esp_wifi_set_mode failed: %s", esp_err_to_name(mode_err));
    return false;
  }

  g_wifi_stack_ready = true;
  g_wifi_initialized = true;
  ESP_LOGI(kTag, "[WIFI] stack initialized (hostname=%s)", g_wifi_config.hostname);
  return true;
}

bool wifi_has_ip() {
  return g_wifi_has_ip;
}

bool wifi_send_connect() {
  if (!g_wifi_initialized) {
    return false;
  }
  if (g_wifi_config.ssid[0] == '\0') {
    ESP_LOGW(kTag, "[WIFI][ERROR] cannot connect: SSID is empty");
    return false;
  }

  if (g_wifi_config.max_retries != 0 && g_wifi_attempt >= g_wifi_max_retries) {
    set_wifi_state(WifiState::kStopped, "max_retries_exceeded");
    return false;
  }

  wifi_config_t config;
  std::memset(&config, 0, sizeof(config));
  std::strncpy(reinterpret_cast<char*>(config.sta.ssid), g_wifi_config.ssid,
               sizeof(config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(config.sta.password), g_wifi_config.pass,
               sizeof(config.sta.password) - 1);
  config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  config.sta.pmf_cfg.capable = true;
  config.sta.pmf_cfg.required = false;

  const esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &config);
  if (cfg_err != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][ERROR] wifi set config failed: %s", esp_err_to_name(cfg_err));
    return false;
  }
  if (g_wifi_config.hostname[0] != '\0') {
    (void)esp_netif_set_hostname(g_wifi_netif, g_wifi_config.hostname);
  }

  const esp_err_t start_err = esp_wifi_start();
  if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_STATE) {
    ESP_LOGW(kTag, "[WIFI][ERROR] wifi start failed: %s", esp_err_to_name(start_err));
    return false;
  }
  const esp_err_t connect_err = esp_wifi_connect();
  if (connect_err != ESP_OK) {
    g_wifi_last_error_reason = connect_err;
    ESP_LOGW(kTag, "[WIFI][ERROR] wifi connect failed: %s", esp_err_to_name(connect_err));
    return false;
  }
  g_wifi_attempt += 1;
  g_wifi_next_backoff_ms = g_wifi_backoff_ms;
  return true;
}

void wifi_schedule_backoff() {
  if (g_wifi_backoff_ms == 0) {
    g_wifi_backoff_ms = g_wifi_backoff_min_ms;
  } else {
    const uint32_t doubled = g_wifi_backoff_ms << 1;
    if (doubled > g_wifi_backoff_ms && doubled <= g_wifi_backoff_max_ms) {
      g_wifi_backoff_ms = doubled;
    } else {
      g_wifi_backoff_ms = g_wifi_backoff_max_ms;
    }
  }
  if (g_wifi_backoff_ms < g_wifi_backoff_min_ms) {
    g_wifi_backoff_ms = g_wifi_backoff_min_ms;
  }
  g_wifi_next_connect_tick = xTaskGetTickCount() + pdMS_TO_TICKS(g_wifi_backoff_ms);
  ESP_LOGI(kTag, "[WIFI][BACKOFF] next_ms=%lu attempt=%lu max=%lu", 
           static_cast<unsigned long>(g_wifi_backoff_ms),
           static_cast<unsigned long>(g_wifi_attempt),
           static_cast<unsigned long>(g_wifi_max_retries));
}

void wifi_status_for_cli() {
  log_wifi_status();
}

void wifi_scan_for_cli() {
  wifi_scan_config_t scan_cfg{};
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_cfg.scan_time.active.min = 120;
  scan_cfg.scan_time.active.max = 120;
  scan_cfg.show_hidden = false;

  const esp_err_t start_rc = esp_wifi_scan_start(&scan_cfg, true);
  if (start_rc != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][SCAN] start failed: %s", esp_err_to_name(start_rc));
    return;
  }

  uint16_t ap_count = 0;
  if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][SCAN] failed to query AP count");
    return;
  }
  if (ap_count == 0) {
    ESP_LOGI(kTag, "[WIFI][SCAN] count=0 networks=[]");
    return;
  }

  uint16_t read_count = static_cast<uint16_t>(
      ap_count < kDefaultWifiScanCount ? ap_count : kDefaultWifiScanCount);
  wifi_ap_record_t records[16];
  if (esp_wifi_scan_get_ap_records(&read_count, records) != ESP_OK) {
    ESP_LOGW(kTag, "[WIFI][SCAN] failed to read AP records");
    return;
  }

  char list[128] = "";
  for (uint16_t idx = 0; idx < read_count; ++idx) {
    if (idx > 0) {
      std::strncat(list, ",", sizeof(list) - std::strlen(list) - 1);
    }
    char ssid[33] = "";
    std::strncpy(ssid, reinterpret_cast<const char*>(records[idx].ssid), sizeof(ssid) - 1);
    std::strncat(list, ssid, sizeof(list) - std::strlen(list) - 1);
  }
  ESP_LOGI(kTag, "[WIFI][SCAN] count=%u networks=%s", static_cast<unsigned>(ap_count), list);
}

void wifi_task(void*) {
  if (!wifi_load_config(g_wifi_config)) {
    g_wifi_enabled = (LUCE_WIFI_AUTOSTART != 0);
  } else {
    g_wifi_enabled = g_wifi_config.enabled || (LUCE_WIFI_AUTOSTART != 0);
  }
  g_wifi_backoff_min_ms = g_wifi_config.backoff_min_ms;
  g_wifi_backoff_max_ms = g_wifi_config.backoff_max_ms;
  g_wifi_max_retries = g_wifi_config.max_retries;

  ESP_LOGI(kTag,
           "[WIFI][NVS] key=ssid present=%d value=%s",
           g_wifi_config.ssid[0] != '\0' ? 1 : 0, g_wifi_config.ssid[0] != '\0' ? g_wifi_config.ssid
                                                                              : "(empty)");
  ESP_LOGI(kTag, "[WIFI][NVS] key=pass present=%d value=%s",
           g_wifi_config.pass[0] != '\0' ? 1 : 0, g_wifi_config.pass[0] != '\0' ? "********" : "(empty)");
  ESP_LOGI(kTag, "[WIFI][NVS] config summary ssid='%s' pass=%s hostname='%s' enabled=%d max_retries=%lu backoff_min_ms=%lu backoff_max_ms=%lu",
           g_wifi_config.ssid[0] != '\0' ? g_wifi_config.ssid : "(empty)",
           g_wifi_config.pass[0] != '\0' ? "********" : "(empty)",
           g_wifi_config.hostname[0] != '\0' ? g_wifi_config.hostname : "(empty)",
           g_wifi_enabled ? 1 : 0, static_cast<unsigned long>(g_wifi_max_retries),
           static_cast<unsigned long>(g_wifi_backoff_min_ms),
           static_cast<unsigned long>(g_wifi_backoff_max_ms));

  if (!wifi_init_wifi_stack()) {
    set_wifi_state(WifiState::kStopped, "stack_init_failed");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(kWifiIdlePollMs));
    }
  }

  if (!g_wifi_enabled && !LUCE_WIFI_AUTOSTART) {
    set_wifi_state(WifiState::kDisabled, "disabled_by_config");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(kWifiStatusIntervalMs));
      log_wifi_status();
    }
  }

  g_wifi_configured = true;
  set_wifi_state(WifiState::kInit, "config_loaded");
  set_wifi_state(WifiState::kConnecting, "startup_connect");
  if (!wifi_send_connect()) {
    set_wifi_state(WifiState::kBackoff, "connect_request_failed");
    wifi_schedule_backoff();
  }

  TickType_t last_status_tick = xTaskGetTickCount();
  while (true) {
    WifiEvent evt;
    const TickType_t now = xTaskGetTickCount();
    if (xQueueReceive(g_wifi_event_queue, &evt, pdMS_TO_TICKS(kWifiIdlePollMs)) == pdTRUE) {
      switch (evt.type) {
        case WifiEventType::kConnected: {
          g_wifi_last_error_reason = 0;
          set_wifi_state(WifiState::kConnecting, "connected");
          break;
        }
        case WifiEventType::kGotIp: {
          set_wifi_state(WifiState::kGotIp, "got_ip");
          g_wifi_attempt = 0;
          g_wifi_backoff_ms = g_wifi_backoff_min_ms;
          wifi_ap_record_t rec;
          if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            g_wifi_last_rssi = rec.rssi;
          }
          log_wifi_status();
          break;
        }
        case WifiEventType::kDisconnected: {
          g_wifi_has_ip = false;
          g_wifi_last_error_reason = evt.reason;
          g_wifi_backoff_ms = g_wifi_backoff_ms == 0 ? g_wifi_backoff_min_ms : g_wifi_backoff_ms;
          set_wifi_state(WifiState::kBackoff, "disconnected");
          if (g_wifi_max_retries != 0 && g_wifi_attempt >= g_wifi_max_retries) {
            set_wifi_state(WifiState::kStopped, "max_retries_exceeded");
          } else {
            wifi_schedule_backoff();
          }
          break;
        }
        case WifiEventType::kStart:
        case WifiEventType::kScanDone:
        default:
          break;
      }
    }

    if (g_wifi_state == WifiState::kBackoff && now >= g_wifi_next_connect_tick) {
      if ((g_wifi_max_retries == 0) || (g_wifi_attempt < g_wifi_max_retries)) {
        set_wifi_state(WifiState::kConnecting, "retry_connect");
        if (!wifi_send_connect()) {
          wifi_schedule_backoff();
        } else {
          g_wifi_next_connect_tick = 0;
        }
      }
    }

    if ((now - last_status_tick) >= pdMS_TO_TICKS(kWifiStatusIntervalMs)) {
      last_status_tick = now;
      log_wifi_status();
    }
    vTaskDelay(pdMS_TO_TICKS(kWifiNoopDelayMs));
  }
}

void wifi_startup() {
  if (g_wifi_manager_task) {
    return;
  }
  g_wifi_event_queue = xQueueCreate(kWifiQueueLength, sizeof(WifiEvent));
  if (!g_wifi_event_queue) {
    ESP_LOGW(kTag, "[WIFI][ERROR] failed to create Wi-Fi event queue");
    return;
  }
  if (xTaskCreate(wifi_task, "wifi_mgr", 6144, nullptr, 3, &g_wifi_manager_task) != pdPASS) {
    ESP_LOGW(kTag, "[WIFI][ERROR] failed to create Wi-Fi manager task");
    return;
  }
}
#endif  // LUCE_HAS_WIFI

#if LUCE_HAS_MDNS
constexpr char kMdnsConfigNamespace[] = "mdns";
constexpr char kNetConfigNamespace[] = "net";
constexpr char kMdnsDefaultHostnamePrefix[] = "luce-";
constexpr char kMdnsDefaultInstance[] = "Luce Stage";
constexpr char kMdnsServiceType[] = "_luce";
constexpr char kMdnsServiceProto[] = "_tcp";
constexpr uint16_t kMdnsServicePort = 80;
constexpr uint16_t kMdnsTxtValueSize = 64;
constexpr uint16_t kMdnsTxtBuildValueSize = 40;

MdnsConfig g_mdns_config {};

struct MdnsRuntime {
  MdnsState state = MdnsState::kDisabledByConfig;
  char hostname[33] = {0};
  char instance[33] = {0};
  char txt_fw[kMdnsTxtValueSize] = {0};
  char txt_stage[kMdnsTxtValueSize] = {0};
  char txt_device_id[kMdnsTxtValueSize] = {0};
  char txt_build[kMdnsTxtBuildValueSize] = {0};
  int service_slot = -1;
  int last_errno = 0;
  bool started = false;
  bool initialized = false;
};

MdnsRuntime g_mdns_runtime {};

void fill_mdns_default_hostname() {
  uint8_t mac[6] = {0};
  const esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (mac_err == ESP_OK) {
    std::snprintf(g_mdns_runtime.hostname, sizeof(g_mdns_runtime.hostname), "%s%02X%02X", 
                  kMdnsDefaultHostnamePrefix, mac[4], mac[5]);
    return;
  }
  std::snprintf(g_mdns_runtime.hostname, sizeof(g_mdns_runtime.hostname), "%slocal", kMdnsDefaultHostnamePrefix);
}

const char* mdns_state_to_string(MdnsState state) {
  switch (state) {
    case MdnsState::kDisabled:
      return "DISABLED";
    case MdnsState::kDisabledByConfig:
      return "DISABLED_BY_CONFIG";
    case MdnsState::kInit:
      return "INIT";
    case MdnsState::kStarted:
      return "STARTED";
    case MdnsState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

void mdns_set_state(MdnsState next_state, const char* reason) {
  if (g_mdns_runtime.state == next_state) {
    return;
  }
  g_mdns_runtime.state = next_state;
  ESP_LOGI(kTag, "[mDNS] state=%s reason=%s", mdns_state_to_string(next_state),
           reason ? reason : "(none)");
}

void mdns_add_txt_item(mdns_service* service, const char* key, const char* value) {
  if (!service || !key || !value) {
    return;
  }
  char item[128] = {0};
  const int32_t len = std::snprintf(item, sizeof(item), "%s=%s", key, value);
  if (len <= 0) {
    return;
  }
  (void)mdns_resp_add_service_txtitem(service, item, static_cast<uint8_t>(len));
}

void mdns_build_txt(mdns_service* service, void* userdata) {
  (void)userdata;
  if (!service) {
    return;
  }
  mdns_add_txt_item(service, "fw", g_mdns_runtime.txt_fw);
  mdns_add_txt_item(service, "stage", g_mdns_runtime.txt_stage);
  mdns_add_txt_item(service, "device", g_mdns_runtime.txt_device_id);
  mdns_add_txt_item(service, "build", g_mdns_runtime.txt_build);
}

bool mdns_read_bool_key(nvs_handle_t handle, const char* key, bool* value, bool* present) {
  if (!handle || !key || !value || !present) {
    return false;
  }
  uint8_t flag = 0;
  const esp_err_t err = nvs_get_u8(handle, key, &flag);
  if (err == ESP_OK) {
    *value = (flag != 0);
    *present = true;
    return true;
  }
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    *present = false;
    return false;
  }
  ESP_LOGW(kTag, "[mDNS][NVS] read error key=%s err=%s", key, esp_err_to_name(err));
  *present = false;
  return false;
}

bool mdns_read_string_key(nvs_handle_t handle, const char* key, char* value, std::size_t value_len,
                         bool* present) {
  if (!handle || !key || !value || value_len == 0 || !present) {
    return false;
  }
  size_t required = 0;
  const esp_err_t err = nvs_get_str(handle, key, nullptr, &required);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      *present = false;
      return false;
    }
    ESP_LOGW(kTag, "[mDNS][NVS] read error key=%s err=%s", key, esp_err_to_name(err));
    *present = false;
    return false;
  }
  if (required > value_len) {
    required = value_len;
  }
  if (nvs_get_str(handle, key, value, &required) != ESP_OK) {
    *present = false;
    return false;
  }
  if (required == 0) {
    *present = false;
    return false;
  }
  *present = true;
  return true;
}

bool mdns_load_config() {
  bool enabled_present = false;
  bool has_hostname = false;
  bool has_instance = false;

  g_mdns_config.enabled = false;
  g_mdns_config.hostname[0] = '\0';
  g_mdns_config.instance[0] = '\0';
  g_mdns_runtime.initialized = false;
  std::memset(g_mdns_runtime.txt_fw, 0, sizeof(g_mdns_runtime.txt_fw));
  std::memset(g_mdns_runtime.txt_stage, 0, sizeof(g_mdns_runtime.txt_stage));
  std::memset(g_mdns_runtime.txt_device_id, 0, sizeof(g_mdns_runtime.txt_device_id));
  std::memset(g_mdns_runtime.txt_build, 0, sizeof(g_mdns_runtime.txt_build));
  g_mdns_runtime.txt_fw[sizeof(g_mdns_runtime.txt_fw) - 1] = '\0';
  g_mdns_runtime.txt_stage[sizeof(g_mdns_runtime.txt_stage) - 1] = '\0';
  g_mdns_runtime.txt_device_id[sizeof(g_mdns_runtime.txt_device_id) - 1] = '\0';
  g_mdns_runtime.txt_build[sizeof(g_mdns_runtime.txt_build) - 1] = '\0';

  nvs_handle_t mdns_handle = 0;
  nvs_handle_t net_handle = 0;
  bool loaded = false;

  if (nvs_open(kMdnsConfigNamespace, NVS_READONLY, &mdns_handle) == ESP_OK) {
    mdns_read_bool_key(mdns_handle, "enabled", &g_mdns_config.enabled, &enabled_present);
    if (mdns_read_string_key(mdns_handle, "instance", g_mdns_config.instance,
                             sizeof(g_mdns_config.instance), &has_instance)) {
      g_mdns_config.instance[sizeof(g_mdns_config.instance) - 1] = '\0';
    } else {
      std::snprintf(g_mdns_config.instance, sizeof(g_mdns_config.instance), "%s", kMdnsDefaultInstance);
    }
    nvs_close(mdns_handle);
    loaded = true;
  } else {
    std::snprintf(g_mdns_config.instance, sizeof(g_mdns_config.instance), "%s", kMdnsDefaultInstance);
  }

  if (nvs_open(kNetConfigNamespace, NVS_READONLY, &net_handle) == ESP_OK) {
    has_hostname = false;
    if (!mdns_read_string_key(net_handle, "hostname", g_mdns_config.hostname,
                             sizeof(g_mdns_config.hostname), &has_hostname)) {
      fill_mdns_default_hostname();
    }
    nvs_close(net_handle);
    loaded = loaded || (has_hostname || has_instance || enabled_present);
  } else {
    fill_mdns_default_hostname();
  }

  if (!loaded) {
    if (g_mdns_config.hostname[0] == '\0') {
      fill_mdns_default_hostname();
    }
    if (g_mdns_config.instance[0] == '\0') {
      std::snprintf(g_mdns_config.instance, sizeof(g_mdns_config.instance), "%s", kMdnsDefaultInstance);
    }
  }

  if (!has_hostname) {
    ESP_LOGI(kTag, "[mDNS][NVS] key=hostname present=0 value=%s", g_mdns_config.hostname);
  } else {
    ESP_LOGI(kTag, "[mDNS][NVS] key=hostname present=1 value=%s", g_mdns_config.hostname);
  }
  ESP_LOGI(kTag, "[mDNS][NVS] key=instance present=%d value=%s",
           has_instance || (g_mdns_config.instance[0] != '\0'), g_mdns_config.instance);
  ESP_LOGI(kTag, "[mDNS][NVS] key=enabled present=%d value=%d", enabled_present,
           g_mdns_config.enabled ? 1 : 0);

  g_mdns_runtime.initialized = true;
  g_mdns_runtime.hostname[0] = '\0';
  g_mdns_runtime.instance[0] = '\0';
  std::strncpy(g_mdns_runtime.hostname, g_mdns_config.hostname, sizeof(g_mdns_runtime.hostname) - 1);
  std::strncpy(g_mdns_runtime.instance, g_mdns_config.instance, sizeof(g_mdns_runtime.instance) - 1);
  std::snprintf(g_mdns_runtime.txt_fw, sizeof(g_mdns_runtime.txt_fw), "%s", LUCE_PROJECT_VERSION);
  std::snprintf(g_mdns_runtime.txt_stage, sizeof(g_mdns_runtime.txt_stage), "stage%d", LUCE_STAGE);
  std::snprintf(g_mdns_runtime.txt_device_id, sizeof(g_mdns_runtime.txt_device_id), "%s", g_mdns_runtime.hostname);
  std::snprintf(g_mdns_runtime.txt_build, sizeof(g_mdns_runtime.txt_build), "%s %s",
                __DATE__, __TIME__);

  ESP_LOGI(kTag, "[mDNS] enabled=%d hostname=%s", g_mdns_config.enabled ? 1 : 0,
           g_mdns_runtime.hostname[0] != '\0' ? g_mdns_runtime.hostname : "(empty)");
  ESP_LOGI(kTag, "[mDNS] startup config loaded (instance=%s)", g_mdns_runtime.instance);

  if (!g_mdns_config.enabled) {
    mdns_set_state(MdnsState::kDisabledByConfig, "enabled=0");
    return false;
  }

  mdns_set_state(MdnsState::kDisabled, "config loaded");
  return true;
}

void mdns_stop_service() {
  if (!g_mdns_runtime.started) {
    return;
  }
  netif* netif = netif_default;
  if (!netif) {
    g_mdns_runtime.started = false;
    g_mdns_runtime.state = MdnsState::kDisabled;
    return;
  }

  if (g_mdns_runtime.service_slot >= 0) {
    (void)mdns_resp_del_service(netif, static_cast<uint8_t>(g_mdns_runtime.service_slot));
    g_mdns_runtime.service_slot = -1;
  }
  (void)mdns_resp_remove_netif(netif);
  g_mdns_runtime.started = false;
  mdns_set_state(MdnsState::kDisabled, "stopped");
}

void mdns_start_service() {
  if (!g_mdns_config.enabled) {
    return;
  }

  if (g_mdns_runtime.started) {
    return;
  }

  if (!wifi_has_ip()) {
    mdns_set_state(MdnsState::kInit, "waiting for ip");
    return;
  }

  if (!g_mdns_config.hostname[0]) {
    mdns_set_state(MdnsState::kFailed, "missing hostname");
    return;
  }

  netif* netif = netif_default;
  if (!netif) {
    mdns_set_state(MdnsState::kFailed, "netif not initialized");
    return;
  }

  if (!g_mdns_runtime.initialized) {
    (void)mdns_load_config();
  }

  if (!g_mdns_config.enabled) {
    mdns_set_state(MdnsState::kDisabledByConfig, "disabled by config");
    return;
  }

  mdns_resp_init();
  const err_t netif_err = mdns_resp_add_netif(netif, g_mdns_runtime.hostname);
  if (netif_err != ERR_OK) {
    g_mdns_runtime.last_errno = netif_err;
    mdns_set_state(MdnsState::kFailed, "add netif failed");
    ESP_LOGW(kTag, "[mDNS] FAILED err=%d", netif_err);
    return;
  }

  g_mdns_runtime.service_slot = mdns_resp_add_service(netif, g_mdns_runtime.instance,
                                                     kMdnsServiceType, DNSSD_PROTO_TCP,
                                                     kMdnsServicePort, mdns_build_txt,
                                                     nullptr);
  if (g_mdns_runtime.service_slot < 0) {
    const int slot_err = g_mdns_runtime.service_slot;
    g_mdns_runtime.last_errno = slot_err;
    (void)mdns_resp_remove_netif(netif);
    mdns_set_state(MdnsState::kFailed, "service registration failed");
    ESP_LOGW(kTag, "[mDNS] FAILED err=%d", slot_err);
    return;
  }

  g_mdns_runtime.started = true;
  mdns_set_state(MdnsState::kStarted, "advertising");
  ESP_LOGI(kTag, "[mDNS] started");
  ESP_LOGI(kTag, "[mDNS] service=instance=%s service=%s.%s port=%u",
           g_mdns_runtime.instance, kMdnsServiceType, kMdnsServiceProto,
           static_cast<unsigned>(kMdnsServicePort));
}

void mdns_handle_wifi_got_ip() {
  if (!g_mdns_config.enabled) {
    return;
  }
  mdns_start_service();
}

void mdns_handle_wifi_lost_ip() {
  mdns_stop_service();
}

void mdns_startup() {
  if (!mdns_load_config()) {
    return;
  }
  if (g_mdns_config.enabled) {
    mdns_set_state(MdnsState::kInit, "ready");
  } else {
    g_mdns_runtime.started = false;
  }
}

void mdns_log_status() {
  const char* state = mdns_state_to_string(g_mdns_runtime.state);
  char services[128] = "(none)";
  if (g_mdns_runtime.started && g_mdns_runtime.service_slot >= 0) {
    std::snprintf(services, sizeof(services), "%s.%s", kMdnsServiceType, kMdnsServiceProto);
  }
  ESP_LOGI(kTag, "CLI command mdns.status: state=%s enabled=%d hostname=%s services=%s", state,
           g_mdns_config.enabled ? 1 : 0,
           g_mdns_runtime.hostname[0] != '\0' ? g_mdns_runtime.hostname : "(empty)", services);
}
#endif  // LUCE_HAS_MDNS

#if LUCE_HAS_MQTT
constexpr char kMqttConfigNamespace[] = "mqtt";
constexpr uint32_t kMqttDefaultKeepaliveS = 120;
constexpr uint32_t kMqttMinKeepaliveS = 30;
constexpr uint32_t kMqttMaxKeepaliveS = 7200;
constexpr uint8_t kMqttMinQos = 0;
constexpr uint8_t kMqttMaxQos = 1;
constexpr uint32_t kMqttMaxRetryBackoffMs = 60000;
constexpr uint16_t kMqttMaxReconnectCycles = 9999;
constexpr TickType_t kMqttLoopDelayMs = 250;
constexpr TickType_t kMqttLogIntervalMs = 10000;
constexpr TickType_t kMqttPublishIntervalMs = 30000;
constexpr std::size_t kMqttPayloadBuffer = 256;
constexpr char kMqttDefaultUri[] = "mqtt://localhost:1883";
constexpr char kMqttDefaultBaseTopic[] = "luce/stage9";
constexpr char kMqttDefaultClientId[] = "";
constexpr char kMqttDefaultCaEmbedded[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBhTCCASugAwIBAgIUNkY8M9QY8fXw1h5n6q3m4k8Yz9EwCgYIKoZIzj0EAwIwFjEUMBIGA1UEAwwLZXhhbXBsZS5sb2NhbDAeFw0yNTAxMDEwMDAwMDBaFw0zNTAxMDEwMDAwMDBaMBYxFDASBgNVBAMMC2V4YW1wbGUubG9jYWwwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATQ7b4fYQ3fXQ7JqXQ9Y8kW6n2r4Q0Y8Wm8Nf7rL3u9x3W2nJx1NqJjW1g5kYhQp3M+u9b2x3iQ3X8f4V2P9rO2QZQ9o0cwRDAOBgNVHQ8BAf8EBAMCAqQwDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiBrv5J7b2aG8z3fW2X5uPzqD3h8qLkQYp+zv6k8Q7xJg4AIgG8q7vT9Y6R8m5JX6Y0rj5uQF9Q6J7f1oN+uKJm5Qy5I=\n"
    "-----END CERTIFICATE-----\n";

MqttConfig g_mqtt_config {};
MqttRuntime g_mqtt_runtime {};

const char* mqtt_state_name(MqttState state) {
  switch (state) {
    case MqttState::kDisabled:
      return "DISABLED";
    case MqttState::kInitialized:
      return "INITIALIZED";
    case MqttState::kConnecting:
      return "CONNECTING";
    case MqttState::kConnected:
      return "CONNECTED";
    case MqttState::kBackoff:
      return "BACKOFF";
    case MqttState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

void mqtt_set_defaults(MqttConfig& config) {
  config = {};
  config.enabled = false;
  std::snprintf(config.uri, sizeof(config.uri), "%s", kMqttDefaultUri);
  std::snprintf(config.base_topic, sizeof(config.base_topic), "%s", kMqttDefaultBaseTopic);
  std::snprintf(config.ca_pem_source, sizeof(config.ca_pem_source), "embedded");
  config.tls_enabled = false;
  config.qos = kMqttMinQos;
  config.keepalive_s = kMqttDefaultKeepaliveS;
}

void mqtt_copy_truncated(char* dst, std::size_t dst_size, const char* src) {
  if (!dst || dst_size == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  const std::size_t copy_len = std::min<std::size_t>(std::strlen(src), dst_size - 1);
  std::memcpy(dst, src, copy_len);
  dst[copy_len] = '\0';
}

uint32_t mqtt_clamp_u32_u(uint32_t value, uint32_t min_value, uint32_t max_value, uint32_t fallback) {
  if (value < min_value || value > max_value) {
    return fallback;
  }
  return value;
}

void mqtt_set_state(MqttState state, const char* reason) {
  if (!reason) {
    reason = "unknown";
  }
  if (g_mqtt_runtime.state == state) {
    std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "%s", reason);
    if (g_mqtt_runtime.last_state[0] != '\0' && std::strcmp(g_mqtt_runtime.last_state, mqtt_state_name(state)) == 0) {
      return;
    }
    std::snprintf(g_mqtt_runtime.last_state, sizeof(g_mqtt_runtime.last_state), "%s", mqtt_state_name(state));
    ESP_LOGI(kTag, "[MQTT][LIFECYCLE] state=%s reason=%s", mqtt_state_name(state), reason);
    return;
  }
  g_mqtt_runtime.state = state;
  std::snprintf(g_mqtt_runtime.last_state, sizeof(g_mqtt_runtime.last_state), "%s", mqtt_state_name(state));
  std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "%s", reason);
  ESP_LOGI(kTag, "[MQTT][LIFECYCLE] state=%s reason=%s", mqtt_state_name(state), reason);
}

bool mqtt_load_dev_ca_pem(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return false;
  }
  const std::size_t len = std::strlen(kMqttDefaultCaEmbedded);
  if (len + 1 >= out_size) {
    ESP_LOGW(kTag, "[MQTT] embedded CA truncated");
    return false;
  }
  std::snprintf(out, out_size, "%s", kMqttDefaultCaEmbedded);
  return true;
}

bool mqtt_build_topic(char* out, std::size_t out_size, const char* suffix) {
  if (!out || out_size == 0) {
    return false;
  }
  const char* topic_base = (g_mqtt_config.base_topic[0] != '\0') ? g_mqtt_config.base_topic
                                                               : kMqttDefaultBaseTopic;
  if (!suffix || suffix[0] == '\0') {
    std::snprintf(out, out_size, "%s", topic_base);
    return true;
  }
  std::snprintf(out, out_size, "%s/%s", topic_base, suffix);
  return true;
}

bool mqtt_load_config(MqttConfig& config) {
  mqtt_set_defaults(config);
  nvs_handle_t handle = 0;
  const esp_err_t open_err = nvs_open(kMqttConfigNamespace, NVS_READONLY, &handle);
  if (open_err != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][NVS] namespace missing, using defaults (enabled=0): %s",
             esp_err_to_name(open_err));
    return false;
  }

  bool present = false;
  uint8_t enabled = 0;
  if (wifi_read_u8_key(handle, "enabled", &enabled, &present) &&
      (enabled == 0 || enabled == 1)) {
    config.enabled = (enabled == 1);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=enabled present=%d value=%d", present ? 1 : 0,
           config.enabled ? 1 : 0);

  bool string_present = false;
  char tmp[1536] = {0};
  if (wifi_read_string_key(handle, "uri", tmp, sizeof(tmp), &string_present) && string_present &&
      tmp[0] != '\0') {
    mqtt_copy_truncated(config.uri, sizeof(config.uri), tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=uri present=%d value=%s", string_present ? 1 : 0, config.uri);

  if (wifi_read_string_key(handle, "client_id", tmp, sizeof(tmp), &string_present) && string_present &&
      tmp[0] != '\0') {
    mqtt_copy_truncated(config.client_id, sizeof(config.client_id), tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=client_id present=%d value=%s", string_present ? 1 : 0,
           config.client_id[0] != '\0' ? config.client_id : kMqttDefaultClientId);

  if (wifi_read_string_key(handle, "base_topic", tmp, sizeof(tmp), &string_present) && string_present &&
      tmp[0] != '\0') {
    mqtt_copy_truncated(config.base_topic, sizeof(config.base_topic), tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=base_topic present=%d value=%s", string_present ? 1 : 0,
           config.base_topic[0] != '\0' ? config.base_topic : kMqttDefaultBaseTopic);

  if (wifi_read_string_key(handle, "username", tmp, sizeof(tmp), &string_present) && string_present) {
    mqtt_copy_truncated(config.username, sizeof(config.username), tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=username present=%d value=%s", string_present ? 1 : 0,
           config.username[0] != '\0' ? "(set)" : "(none)");

  if (wifi_read_string_key(handle, "password", tmp, sizeof(tmp), &string_present) && string_present) {
    mqtt_copy_truncated(config.password, sizeof(config.password), tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=password present=%d value=%s", string_present ? 1 : 0,
           config.password[0] != '\0' ? "(set)" : "(none)");

  uint8_t tls_enabled = 0;
  if (wifi_read_u8_key(handle, "tls_enabled", &tls_enabled, &present) &&
      (tls_enabled == 0 || tls_enabled == 1)) {
    config.tls_enabled = (tls_enabled == 1);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=tls_enabled present=%d value=%d", present ? 1 : 0,
           config.tls_enabled ? 1 : 0);

  if (wifi_read_string_key(handle, "ca_pem_source", tmp, sizeof(tmp), &string_present) &&
      string_present) {
    mqtt_copy_truncated(config.ca_pem_source, sizeof(config.ca_pem_source), tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=ca_pem_source present=%d value=%s", string_present ? 1 : 0,
           config.ca_pem_source[0] != '\0' ? config.ca_pem_source : "embedded");

  if (wifi_read_string_key(handle, "ca_pem", tmp, sizeof(tmp), &string_present) &&
      string_present && tmp[0] != '\0' && std::strncmp(config.ca_pem_source, "embedded", 8) != 0) {
    mqtt_copy_truncated(config.ca_pem, sizeof(config.ca_pem), tmp);
  }

  uint32_t qos = config.qos;
  if (wifi_read_u32_key(handle, "qos", &qos, &present)) {
    config.qos = mqtt_clamp_u32_u(qos, kMqttMinQos, kMqttMaxQos, kMqttMinQos);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=qos present=%d value=%lu", present ? 1 : 0,
           static_cast<unsigned long>(config.qos));

  uint32_t keepalive = config.keepalive_s;
  if (wifi_read_u32_key(handle, "keepalive_s", &keepalive, &present)) {
    config.keepalive_s = mqtt_clamp_u32_u(keepalive, kMqttMinKeepaliveS, kMqttMaxKeepaliveS,
                                          kMqttDefaultKeepaliveS);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=keepalive_s present=%d value=%lu", present ? 1 : 0,
           static_cast<unsigned long>(config.keepalive_s));

  if (std::strncmp(config.ca_pem_source, "embedded", 8) == 0 && config.ca_pem[0] == '\0') {
    (void)mqtt_load_dev_ca_pem(config.ca_pem, sizeof(config.ca_pem));
  } else if (!config.tls_enabled) {
    config.ca_pem[0] = '\0';
  }

  nvs_close(handle);
  return true;
}

void mqtt_handle_wifi_got_ip() {
  if (!g_mqtt_config.enabled) {
    return;
  }
  g_mqtt_runtime.retry_backoff_ms = kMqttMinRetryBackoffMs;
  g_mqtt_runtime.next_retry_tick = 0;
  if (g_mqtt_runtime.state == MqttState::kBackoff || g_mqtt_runtime.state == MqttState::kFailed) {
    mqtt_set_state(MqttState::kInitialized, "got ip");
  }
}

void mqtt_handle_wifi_lost_ip() {
  if (g_mqtt_runtime.client) {
    (void)esp_mqtt_client_stop(g_mqtt_runtime.client);
    (void)esp_mqtt_client_destroy(g_mqtt_runtime.client);
    g_mqtt_runtime.client = nullptr;
  }
  g_mqtt_runtime.connected = false;
  g_mqtt_runtime.state = MqttState::kBackoff;
  g_mqtt_runtime.next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
  g_mqtt_runtime.retry_backoff_ms = kMqttMinRetryBackoffMs;
  std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "wifi lost");
  ESP_LOGW(kTag, "[MQTT] wifi lost");
}

void mqtt_log_status_line() {
  char published_utc[64] = "n/a";
  if (g_mqtt_runtime.last_pub_unix > 0) {
    const time_t published = g_mqtt_runtime.last_pub_unix;
    const std::tm* published_tm = std::gmtime(&published);
    if (published_tm) {
      std::snprintf(published_utc, sizeof(published_utc), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                    published_tm->tm_year + 1900, published_tm->tm_mon + 1, published_tm->tm_mday,
                    published_tm->tm_hour, published_tm->tm_min, published_tm->tm_sec);
    }
  }
  const time_t now = time(nullptr);
  const uint64_t last_publish_age_s = (g_mqtt_runtime.last_pub_unix > 0 && now >= g_mqtt_runtime.last_pub_unix)
                                        ? static_cast<uint64_t>(now - g_mqtt_runtime.last_pub_unix)
                                        : 0ULL;
  const char* uri = g_mqtt_runtime.effective_uri[0] != '\0' ? g_mqtt_runtime.effective_uri : "(unset)";
  ESP_LOGI(kTag,
           "CLI command mqtt.status: state=%s enabled=%d connected=%d reason=%s "
           "connect_count=%lu publish_count=%lu last_topic=%s last_rc=%d last_payload=%u "
           "last_latency_ms=%lu last_pub_age_s=%llu last_pub_unix=%lld utc=%s uri=%s tls=%d qos=%u keepalive_s=%lu",
           g_mqtt_runtime.last_state, g_mqtt_config.enabled ? 1 : 0, g_mqtt_runtime.connected ? 1 : 0,
           g_mqtt_runtime.last_reason, static_cast<unsigned long>(g_mqtt_runtime.connect_count),
           static_cast<unsigned long>(g_mqtt_runtime.publish_count), g_mqtt_runtime.last_topic,
           g_mqtt_runtime.last_pub_rc, static_cast<unsigned long>(g_mqtt_runtime.last_pub_payload_size),
           static_cast<unsigned long>(g_mqtt_runtime.last_publish_latency_ms), static_cast<unsigned long long>(last_publish_age_s),
           static_cast<long long>(g_mqtt_runtime.last_pub_unix), published_utc, uri,
           g_mqtt_config.tls_enabled ? 1 : 0, g_mqtt_config.qos,
           static_cast<unsigned long>(g_mqtt_config.keepalive_s));
}

void mqtt_event_handler(void*,
                       esp_event_base_t,
                       int32_t event_id,
                       void* event_data) {
  const auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
  switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
      g_mqtt_runtime.connected = true;
      g_mqtt_runtime.last_connected = true;
      g_mqtt_runtime.last_disconnected = false;
      g_mqtt_runtime.last_connect_tick = xTaskGetTickCount();
      g_mqtt_runtime.connect_count += 1;
      g_mqtt_runtime.reconnects = 0;
      g_mqtt_runtime.retry_backoff_ms = kMqttMinRetryBackoffMs;
      g_mqtt_runtime.next_retry_tick = 0;
      mqtt_set_state(MqttState::kConnected, "connected");
      ESP_LOGI(kTag, "[MQTT] connected client_id=%s", g_mqtt_config.client_id[0] != '\0'
                                                      ? g_mqtt_config.client_id
                                                      : "auto");
      (void)mqtt_publish_status(true);
      break;
    }
    case MQTT_EVENT_DISCONNECTED: {
      g_mqtt_runtime.connected = false;
      g_mqtt_runtime.last_connected = false;
      g_mqtt_runtime.last_disconnected = true;
      g_mqtt_runtime.last_disconnect_tick = xTaskGetTickCount();
      mqtt_set_state(MqttState::kBackoff, "disconnected");
      const int reason_code = event && event->error_handle && event->error_handle->error_type
                                 ? event->error_handle->error_type
                                 : 0;
      if (g_mqtt_runtime.retry_backoff_ms < kMqttMaxRetryBackoffMs) {
        g_mqtt_runtime.retry_backoff_ms *= 2;
      }
      if (g_mqtt_runtime.retry_backoff_ms > kMqttMaxRetryBackoffMs) {
        g_mqtt_runtime.retry_backoff_ms = kMqttMaxRetryBackoffMs;
      }
      g_mqtt_runtime.next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
      ESP_LOGW(kTag, "[MQTT] disconnected reason=%d reconnect_ms=%lu", reason_code,
               static_cast<unsigned long>(g_mqtt_runtime.retry_backoff_ms));
      break;
    }
    case MQTT_EVENT_PUBLISHED: {
      ESP_LOGI(kTag, "[MQTT] publish acked msg_id=%d", event ? event->msg_id : -1);
      break;
    }
    case MQTT_EVENT_ERROR: {
      int source = 0;
      int code = 0;
      if (event && event->error_handle) {
        source = event->error_handle->error_type;
        code = event->error_handle->connect_return_code;
      }
      g_mqtt_runtime.state = MqttState::kFailed;
      std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "mqtt error=%d code=%d",
                    source, code);
      ESP_LOGW(kTag, "[MQTT] event error source=%d code=%d", source, code);
      mqtt_set_state(MqttState::kFailed, g_mqtt_runtime.last_reason);
      g_mqtt_runtime.reconnects += 1;
      if (g_mqtt_runtime.reconnects >= kMqttMaxReconnectCycles) {
        g_mqtt_runtime.reconnects = kMqttMaxReconnectCycles;
      }
      break;
    }
    default:
      break;
  }
}

bool mqtt_build_client_config(esp_mqtt_client_config_t& out_cfg) {
  out_cfg = {};
  out_cfg.network.reconnect_timeout_ms = kMqttMaxRetryBackoffMs;
  out_cfg.network.disable_auto_reconnect = true;
  out_cfg.session.keepalive = static_cast<int>(g_mqtt_config.keepalive_s);
  out_cfg.task.stack_size = static_cast<int>(kMqttTaskStackWords);
  out_cfg.task.priority = 2;

  mqtt_copy_truncated(g_mqtt_runtime.effective_uri, sizeof(g_mqtt_runtime.effective_uri), g_mqtt_config.uri);
  if (!std::strstr(g_mqtt_runtime.effective_uri, "://")) {
    if (g_mqtt_config.tls_enabled) {
      const char prefix[] = "mqtts://";
      const std::size_t prefix_len = sizeof(prefix) - 1;
      const std::size_t copy_len = std::min<std::size_t>(
          std::strlen(g_mqtt_config.uri), sizeof(g_mqtt_runtime.effective_uri) - prefix_len - 1);
      std::memcpy(g_mqtt_runtime.effective_uri, prefix, prefix_len);
      std::memcpy(g_mqtt_runtime.effective_uri + prefix_len, g_mqtt_config.uri, copy_len);
      g_mqtt_runtime.effective_uri[prefix_len + copy_len] = '\0';
    } else {
      const char prefix[] = "mqtt://";
      const std::size_t prefix_len = sizeof(prefix) - 1;
      const std::size_t copy_len = std::min<std::size_t>(
          std::strlen(g_mqtt_config.uri), sizeof(g_mqtt_runtime.effective_uri) - prefix_len - 1);
      std::memcpy(g_mqtt_runtime.effective_uri, prefix, prefix_len);
      std::memcpy(g_mqtt_runtime.effective_uri + prefix_len, g_mqtt_config.uri, copy_len);
      g_mqtt_runtime.effective_uri[prefix_len + copy_len] = '\0';
    }
  }
  out_cfg.broker.address.uri = g_mqtt_runtime.effective_uri;

  if (g_mqtt_config.client_id[0] != '\0') {
    out_cfg.credentials.client_id = g_mqtt_config.client_id;
  }
  if (g_mqtt_config.username[0] != '\0') {
    out_cfg.credentials.username = g_mqtt_config.username;
  }
  if (g_mqtt_config.password[0] != '\0') {
    out_cfg.credentials.authentication.password = g_mqtt_config.password;
  }

  if (g_mqtt_config.tls_enabled) {
    if (std::strncmp(g_mqtt_config.ca_pem_source, "embedded", 8) == 0) {
      if (g_mqtt_config.ca_pem[0] == '\0') {
        (void)mqtt_load_dev_ca_pem(g_mqtt_config.ca_pem, sizeof(g_mqtt_config.ca_pem));
      }
      if (g_mqtt_config.ca_pem[0] != '\0') {
        out_cfg.broker.verification.certificate = g_mqtt_config.ca_pem;
        out_cfg.broker.verification.certificate_len = std::strlen(g_mqtt_config.ca_pem);
      } else {
        ESP_LOGW(kTag, "[MQTT] TLS requested but certificate unavailable; verify may fail");
      }
    } else {
      ESP_LOGW(kTag, "[MQTT] TLS enabled with unsupported source=%s", g_mqtt_config.ca_pem_source);
    }
  }

  char lwt_topic[128] = {0};
  if (mqtt_build_topic(lwt_topic, sizeof(lwt_topic), "lwt")) {
    std::snprintf(g_mqtt_runtime.last_will_topic, sizeof(g_mqtt_runtime.last_will_topic), "%s", lwt_topic);
    out_cfg.session.last_will.topic = g_mqtt_runtime.last_will_topic;
  }
  out_cfg.session.last_will.msg = "offline";
  out_cfg.session.last_will.msg_len = 7;
  out_cfg.session.last_will.qos = 0;
  out_cfg.session.last_will.retain = 0;
  return true;
}

int mqtt_publish_payload(const char* topic, const char* payload, int payload_len, uint8_t qos) {
  if (!g_mqtt_runtime.client || !topic || !payload || !g_mqtt_runtime.connected) {
    return ESP_ERR_INVALID_STATE;
  }
  if (qos > kMqttMaxQos) {
    qos = kMqttMaxQos;
  }
  const TickType_t start = xTaskGetTickCount();
  const int rc = esp_mqtt_client_publish(g_mqtt_runtime.client, topic, payload, payload_len, qos, 0);
  const uint32_t latency_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);
  g_mqtt_runtime.last_publish_latency_ms = latency_ms;
  std::snprintf(g_mqtt_runtime.last_topic, sizeof(g_mqtt_runtime.last_topic), "%s", topic);
  g_mqtt_runtime.last_pub_rc = rc;
  g_mqtt_runtime.last_pub_payload_size = payload_len < 0 ? 0U : static_cast<uint32_t>(payload_len);
  g_mqtt_runtime.publish_count += 1;
  g_mqtt_runtime.last_pub_unix = time(nullptr);

  if (rc >= 0) {
    g_mqtt_runtime.publish_backoff_failures = 0;
    g_mqtt_runtime.last_reason[0] = '\0';
    std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "publish rc=%d", rc);
    ESP_LOGI(kTag, "[MQTT] publish rc=%d topic=%s size=%d latency_ms=%lu", rc, topic, payload_len,
             static_cast<unsigned long>(latency_ms));
  } else {
    g_mqtt_runtime.publish_backoff_failures += 1;
    std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason),
                 "publish failed rc=%d", rc);
    ESP_LOGW(kTag, "[MQTT] publish failed rc=%d topic=%s size=%d", rc, topic, payload_len);
  }
  return rc;
}

int mqtt_publish_status(bool include_time) {
  char topic[128] = {0};
  if (!mqtt_build_topic(topic, sizeof(topic), "telemetry/state")) {
    return ESP_ERR_INVALID_STATE;
  }
  char payload[kMqttPayloadBuffer] = {0};
  char utc_now[64] = "n/a";
  if (include_time) {
    const time_t now = time(nullptr);
    const std::tm* utc_tm = std::gmtime(&now);
    if (utc_tm) {
      std::snprintf(utc_now, sizeof(utc_now), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                    utc_tm->tm_year + 1900, utc_tm->tm_mon + 1, utc_tm->tm_mday,
                    utc_tm->tm_hour, utc_tm->tm_min, utc_tm->tm_sec);
    }
  }
  char relay_buf[16] = {0};
  char button_buf[16] = {0};
  format_mcp_mask_line(relay_buf, sizeof(relay_buf), g_relay_mask, g_button_mask,
                       McpMaskFormat::kStatusCommand);
  const uint64_t age_s = (g_mqtt_runtime.last_pub_unix > 0 && time(nullptr) >= g_mqtt_runtime.last_pub_unix)
                            ? static_cast<uint64_t>(time(nullptr) - g_mqtt_runtime.last_pub_unix)
                            : 0ULL;
  std::snprintf(payload, sizeof(payload),
               "{\"fw\":\"%s\",\"stage\":%d,\"ip\":\"%s\",\"relay\":\"%s\",\"buttons\":\"%s\","
               "\"wifi_rssi\":%d,\"ntp_unix\":%lld,\"ntp_age_s\":%llu,\"utc\":\"%s\",\"connected\":%s}",
               LUCE_PROJECT_VERSION, LUCE_STAGE, g_wifi_ip, relay_buf, button_buf, g_wifi_last_rssi,
               static_cast<long long>(g_mqtt_runtime.last_pub_unix), static_cast<unsigned long long>(age_s),
               utc_now, g_mqtt_runtime.connected ? "true" : "false");
  return mqtt_publish_payload(topic, payload, static_cast<int>(std::strlen(payload)),
                             static_cast<uint8_t>(g_mqtt_config.qos));
}

int mqtt_publish_test_payload() {
  char topic[128] = {0};
  if (!mqtt_build_topic(topic, sizeof(topic), "telemetry/pubtest")) {
    return ESP_ERR_INVALID_STATE;
  }
  const char payload[] = "{\"event\":\"mqtt_pubtest\",\"source\":\"serial_cli\"}";
  return mqtt_publish_payload(topic, payload, static_cast<int>(sizeof(payload) - 1),
                             static_cast<uint8_t>(g_mqtt_config.qos));
}

void mqtt_task(void*) {
  if (!mqtt_load_config(g_mqtt_config)) {
    mqtt_set_defaults(g_mqtt_config);
  }
  if (!g_mqtt_config.enabled) {
    mqtt_set_state(MqttState::kDisabled, "disabled-by-config");
    g_mqtt_runtime.effective_uri[0] = '\0';
  } else {
    mqtt_set_state(MqttState::kInitialized, "loaded");
  }
  g_mqtt_runtime.client = nullptr;
  g_mqtt_runtime.last_reason[0] = '\0';
  g_mqtt_runtime.connect_count = 0;
  g_mqtt_runtime.publish_count = 0;
  g_mqtt_runtime.retry_backoff_ms = kMqttMinRetryBackoffMs;
  g_mqtt_runtime.next_retry_tick = 0;

  TickType_t last_status_tick = xTaskGetTickCount();
  TickType_t last_publish_tick = xTaskGetTickCount();

  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if (!g_mqtt_config.enabled) {
      if (g_mqtt_runtime.client) {
        (void)esp_mqtt_client_stop(g_mqtt_runtime.client);
        (void)esp_mqtt_client_destroy(g_mqtt_runtime.client);
        g_mqtt_runtime.client = nullptr;
      }
      g_mqtt_runtime.connected = false;
      mqtt_set_state(MqttState::kDisabled, "disabled-by-config");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!wifi_has_ip()) {
      if (g_mqtt_runtime.connected || g_mqtt_runtime.client) {
        mqtt_handle_wifi_lost_ip();
      } else {
        mqtt_set_state(MqttState::kBackoff, "no-ip");
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!g_mqtt_runtime.client) {
      if (g_mqtt_runtime.state != MqttState::kConnecting && g_mqtt_runtime.state != MqttState::kFailed) {
        mqtt_set_state(MqttState::kInitialized, "creating-client");
      }
      esp_mqtt_client_config_t cfg {};
      if (!mqtt_build_client_config(cfg)) {
        mqtt_set_state(MqttState::kFailed, "bad config");
      } else {
        g_mqtt_runtime.client = esp_mqtt_client_init(&cfg);
        if (g_mqtt_runtime.client) {
          const esp_err_t ev = esp_mqtt_client_register_event(g_mqtt_runtime.client, MQTT_EVENT_ANY,
                                                             &mqtt_event_handler, nullptr);
          if (ev != ESP_OK) {
            ESP_LOGW(kTag, "[MQTT] register event failed: %s", esp_err_to_name(ev));
          }
          mqtt_set_state(MqttState::kConnecting, "start");
          const esp_err_t start_rc = esp_mqtt_client_start(g_mqtt_runtime.client);
          if (start_rc != ESP_OK) {
            ESP_LOGW(kTag, "[MQTT] start failed: %s", esp_err_to_name(start_rc));
            (void)esp_mqtt_client_destroy(g_mqtt_runtime.client);
            g_mqtt_runtime.client = nullptr;
            mqtt_set_state(MqttState::kFailed, "start failed");
            g_mqtt_runtime.reconnects += 1;
            g_mqtt_runtime.next_retry_tick = now + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
            g_mqtt_runtime.retry_backoff_ms =
                std::min(g_mqtt_runtime.retry_backoff_ms * 2, kMqttMaxRetryBackoffMs);
          } else {
            g_mqtt_runtime.state = MqttState::kConnecting;
          }
        } else {
          mqtt_set_state(MqttState::kFailed, "client alloc failed");
          g_mqtt_runtime.next_retry_tick = now + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
        }
      }
    } else if (!g_mqtt_runtime.connected) {
      if (g_mqtt_runtime.state != MqttState::kConnecting &&
          g_mqtt_runtime.state != MqttState::kBackoff) {
        mqtt_set_state(MqttState::kConnecting, "reconnect");
      }
      if (now >= g_mqtt_runtime.next_retry_tick &&
          (g_mqtt_runtime.state == MqttState::kBackoff || g_mqtt_runtime.state == MqttState::kFailed)) {
        const esp_err_t rc = esp_mqtt_client_reconnect(g_mqtt_runtime.client);
        if (rc != ESP_OK) {
          mqtt_set_state(MqttState::kFailed, "reconnect failed");
          g_mqtt_runtime.reconnects += 1;
          if (g_mqtt_runtime.reconnects >= kMqttMaxReconnectCycles) {
            g_mqtt_runtime.reconnects = kMqttMaxReconnectCycles;
          }
          g_mqtt_runtime.retry_backoff_ms = std::min(g_mqtt_runtime.retry_backoff_ms * 2, kMqttMaxRetryBackoffMs);
          g_mqtt_runtime.next_retry_tick = now + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
          ESP_LOGW(kTag, "[MQTT] reconnect rc=%s retry_ms=%lu", esp_err_to_name(rc),
                   static_cast<unsigned long>(g_mqtt_runtime.retry_backoff_ms));
        } else {
          g_mqtt_runtime.state = MqttState::kConnecting;
          g_mqtt_runtime.next_retry_tick = 0;
        }
      }
    } else if ((now - last_publish_tick) >= pdMS_TO_TICKS(kMqttPublishIntervalMs)) {
      const int rc = mqtt_publish_status(true);
      if (rc != ESP_OK) {
        g_mqtt_runtime.last_reason[0] = '\0';
        std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "publish rc=%d", rc);
      }
      last_publish_tick = now;
    }

    if ((now - last_status_tick) >= pdMS_TO_TICKS(kMqttLogIntervalMs)) {
      last_status_tick = now;
      mqtt_log_status_line();
    }
    vTaskDelay(pdMS_TO_TICKS(kMqttLoopDelayMs));
  }
}

void mqtt_startup() {
  if (xTaskCreate(mqtt_task, "mqtt", kMqttTaskStackWords, nullptr, 2, &g_mqtt_task) != pdPASS) {
    ESP_LOGW(kTag, "[MQTT] startup: task create failed");
  }
}
#endif

#if LUCE_HAS_HTTP
constexpr char kHttpConfigNamespace[] = "http";
constexpr uint16_t kHttpDefaultPort = 443;
constexpr uint16_t kHttpMaxConfigPort = 65535;
constexpr std::size_t kHttpLogBuffer = 64;
constexpr std::size_t kHttpJsonBuffer = 256;
constexpr std::size_t kHttpBodyLimitBytes = 256;
constexpr char kHttpAuthBearerPrefix[] = "Bearer ";

constexpr char kHttpDefaultToken[] = "luce-http";
constexpr char kHttpRouteHealth[] = "/api/health";
constexpr char kHttpRouteInfo[] = "/api/info";
constexpr char kHttpRouteState[] = "/api/state";

constexpr char kHttpDevCert[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICpjCCAY4CCQDMeA8ybFpcLjANBgkqhkiG9w0BAQsFADAVMRMwEQYDVQQDDAps\n"
    "dWNlLmxvY2FsMB4XDTI2MDIyMjIzMDgxNloXDTI3MDIyMjIzMDgxNlowFTETMBEG\n"
    "A1UEAwwKbHVjZS5sb2NhbDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEB\n"
    "AL5STL7/ODhEQxAQZPzXZt6EdImfLT+C/VeJyXxpECY58eFYrBkAELPYc5BWAIPT\n"
    "pcyxxCtstWMWQgXWdAHXARy6UAXjE7aQ33k0+Y2LIiz9xlCDLa1AONiRJTaBJen4\n"
    "NuKcEjUNynmKZTJ3gMIsTrADHO8oTxYb6O9RgFJXBy1p3lH40Fd62/NpsSN3zxSO\n"
    "/Daenjie841FyPD9gW1YLV9bQNuxDZe+8JSPc6kRecBKLVSJFHkREERpgX6zDwtj\n"
    "sGL1hptou2VlibdlRNPIztap5FRkhYxP03xGNJzwPtHXvagA7gbbZt/8PfV874qK\n"
    "dlsimdoZa4+azz1QPSWf6UECAwEAATANBgkqhkiG9w0BAQsFAAOCAQEAmPkVdwTt\n"
    "oD5FjVjZCGmJqG4ElWlSzMGkwWfi+CSKlUAoGy0Jmjq9zQL2893d0uVOew08jyiB\n"
    "QgnG1L3tygFJbbE2TfmD41Yze9eypBCEz6hk39FmbGl9EJcK719HCamjJzh7Lfio\n"
    "qzUw9HvGR0IMD3kCep28LrPVAPW/TNwDCTWyS+9Q5lMheKbBZuu9tgw7YHuulvIc\n"
    "eApV0eXa4910sUXW+uGNACNRLT2JSiVfa95ocyZ26tZlO0oUOAhxmb0Mvp1ISocO\n"
    "SCaFUm5sKYvnF8D9fF4BnfHgz5UiOwnSt8r7AKEX8178stvgEjtwKd6ibaZWZhvz\n"
    "Vnfe6i7No6/slA==\n"
    "-----END CERTIFICATE-----\n";
constexpr char kHttpDevPrivateKey[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQC+Uky+/zg4REMQ\n"
    "EGT812behHSJny0/gv1Xicl8aRAmOfHhWKwZABCz2HOQVgCD06XMscQrbLVjFkIF\n"
    "1nQB1wEculAF4xO2kN95NPmNiyIs/cZQgy2tQDjYkSU2gSXp+DbinBI1Dcp5imUy\n"
    "d4DCLE6wAxzvKE8WG+jvUYBSVwctad5R+NBXetvzabEjd88Ujvw2np44nvONRcjw\n"
    "/YFtWC1fW0DbsQ2XvvCUj3OpEXnASi1UiRR5ERBEaYF+sw8LY7Bi9YabaLtlZYm3\n"
    "ZUTTyM7WqeRUZIWMT9N8RjSc8D7R172oAO4G22bf/D31fO+KinZbIpnaGWuPms89\n"
    "UD0ln+lBAgMBAAECggEAPPVhTX+zgxoiHMATiIR5l2X3aakJNiF/gY1Jcsa3/HZs\n"
    "yc+795n0v5XhleZl7dNZdJGvknUUN/OGHBaPO5Og8JGgVfJgewY1/b2A/NwGi0CR\n"
    "R3Jsq+Q5EOyUbbu56BGviq+QiVuscXdpaFusawUEAw5MMzHG+v7fgd+p6TWkv99t\n"
    "4S3pcvonk/7XmWDdogfHBKi9NSPa4oe5hXO1DirKoFmpk6328z+mrx7qXzOd//sN\n"
    "9aRSlHTXcBzhhySP0YtWU5kl2W05pRcH0/Nn65l+/mAzx4c+N4JWCPkF9ef+gCUL\n"
    "UbUVki76D0oogkTeSjRgdL3iaWQGno+gvKOjYruHLQKBgQDy4IsmM8HwQS6B9XCj\n"
    "FLVczaPf5wWJXNlYrQtBnUi7E+3yHCDaKYI7VXi508A8euL8MMevv6qPOHjAAOLI\n"
    "tdHgdQA+ZCGbSGhS97mEhm/B1bQXnn51IMVDIWwHQxIFYNhwrj/wzHy8+L/bSNbn\n"
    "UF3aukYMF+CKWhkS6SlUk09yAwKBgQDIms+ZwxajTPoyxlve8r80yjgBCvxCuzpG\n"
    "6FqQq/mhgKIgymxNH9X44veiD/qS/+YsoYa6uyOmBko6VqCwBVtGTUMDFWvxgnYZ\n"
    "Qn3JgoQbSVbZ5brevOE+itd1VLjW71zMqup5viycC7LIplB8EsYkzd8paLmLVPf7\n"
    "R5kYtzUWawKBgBEyFKf/whtgggpxdigVr0GCzbdsg9fV2w2MMt/SYvPb1Vzu4OSR\n"
    "S8cnpgSCGXouuSNh0MGAsHKzbNkrNuM+/D0IC5xfOoHj/n7hSyE243K1zqpdblac\n"
    "m1rFYwCgnwYCdVCFBcHmuG4ormy4G38FEaAK0CrLBfrFpkDQgTybsWRBAoGAF3ZL\n"
    "y48OqcDKDoA2pIe9pz3zeOPBB0kAkuSAGyWSB7qUu8MREaAklXxuPA0kYGb/k768\n"
    "lEBo9fUMX3BcUNn/h+RnbwflXRTGHUQylAvoyYw1VTzSM1Th/z+b3YQwLitGrkVb\n"
    "MSv16bZQjbkt9qT3ebx+Wkh+UvZ4HnKMTGC5G8sCgYArbBM7C0j2dHLP4TzwtpMQ\n"
    "ZzVyKtSZ2RDJ66MAmjQDFI0ey2vgamJh5VJy0vNKGgAsAZviBxyGQFmeeVZNbpSz\n"
    "nuN/bNMgicgEatZynzyAHBJT3NkUhnJKuK59s6JF+s7l2hM2OiVSpebig1+YI+fe\n"
    "ZBJ9UZciff+NShL6BYEpGg==\n"
    "-----END PRIVATE KEY-----\n";

enum class HttpUriType : uint8_t {
  kHealth = 0,
  kInfo,
  kState,
};

HttpConfig g_http_config {};
HttpRuntime g_http_runtime {};
httpd_handle_t g_http_server = nullptr;

const char* http_state_name(HttpState state) {
  switch (state) {
    case HttpState::kDisabled:
      return "DISABLED";
    case HttpState::kInit:
      return "INIT";
    case HttpState::kReady:
      return "READY";
    case HttpState::kStarted:
      return "STARTED";
    case HttpState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

void http_set_defaults(HttpConfig& config) {
  config = {};
  config.enabled = false;
  config.port = kHttpDefaultPort;
  config.tls_dev_mode = true;
  std::snprintf(config.token, sizeof(config.token), "%s", kHttpDefaultToken);
}

const char* http_method_name(int method) {
  switch (method) {
    case HTTP_DELETE:
      return "DELETE";
    case HTTP_GET:
      return "GET";
    case HTTP_HEAD:
      return "HEAD";
    case HTTP_POST:
      return "POST";
    case HTTP_PUT:
      return "PUT";
    case HTTP_PATCH:
      return "PATCH";
    case HTTP_OPTIONS:
      return "OPTIONS";
    default:
      return "UNKNOWN";
  }
}

void http_load_remote_ip(const httpd_req_t* req, char* out, std::size_t out_size) {
  if (!req || !out || out_size == 0) {
    return;
  }
  out[0] = '\0';

  const int socket = httpd_req_to_sockfd(const_cast<httpd_req_t*>(req));
  if (socket < 0) {
    std::snprintf(out, out_size, "socket:%d", socket);
    return;
  }

  sockaddr_in addr {};
  socklen_t addr_len = sizeof(addr);
  if (getpeername(socket, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0) {
    const char* addr_text = inet_ntoa(addr.sin_addr);
    std::snprintf(out, out_size, "%s", addr_text ? addr_text : "0.0.0.0");
    return;
  }
  std::snprintf(out, out_size, "(unknown)");
}

void http_set_runtime_state(HttpState state, const char* reason) {
  g_http_runtime.state = state;
  if (reason && reason[0] != '\0') {
    ESP_LOGI(kTag, "[HTTP] state=%s reason=%s", http_state_name(state), reason);
  } else {
    ESP_LOGI(kTag, "[HTTP] state=%s", http_state_name(state));
  }
}

void http_log_request(const httpd_req_t* req, const char* route, int status,
                     const char* remote_ip, uint64_t duration_ms) {
  const char* method = req ? http_method_name(req->method) : "UNKNOWN";
  ESP_LOGI(kTag, "[HTTP] method=%s path=%s status=%d remote=%s duration_ms=%llu",
           method, route ? route : "(unknown)", status, remote_ip ? remote_ip : "(unknown)",
           static_cast<unsigned long long>(duration_ms));
}

void http_record_request(const httpd_req_t* req, int status, const char* route, uint64_t start_us,
                        bool unauthorized, const HttpUriType uri_type) {
  g_http_runtime.request_count += 1;
  if (unauthorized) {
    g_http_runtime.unauthorized_count += 1;
  }
  if (status >= 400) {
    g_http_runtime.error_count += 1;
  }

  char remote_ip[kHttpLogBuffer] = "unknown";
  http_load_remote_ip(req, remote_ip, sizeof(remote_ip));
  const uint64_t duration_ms = (esp_timer_get_time() - start_us) / 1000ULL;
  (void)uri_type;
  http_log_request(req, route, status, remote_ip, duration_ms);
}

bool http_send_json(httpd_req_t* req, const char* body, int status, const char* route, uint64_t start_us,
                   bool unauthorized, HttpUriType uri_type) {
  if (!req || !body) {
    http_record_request(req, 500, route, start_us, true, uri_type);
    return false;
  }

  char status_line[24] = "200 OK";
  if (status != 200) {
    if (status == 401) {
      std::snprintf(status_line, sizeof(status_line), "401 Unauthorized");
    } else if (status == 403) {
      std::snprintf(status_line, sizeof(status_line), "403 Forbidden");
    } else if (status == 404) {
      std::snprintf(status_line, sizeof(status_line), "404 Not Found");
    } else if (status == 405) {
      std::snprintf(status_line, sizeof(status_line), "405 Method Not Allowed");
    } else if (status == 413) {
      std::snprintf(status_line, sizeof(status_line), "413 Payload Too Large");
    } else {
      std::snprintf(status_line, sizeof(status_line), "%d Error", status);
    }
    (void)httpd_resp_set_status(req, status_line);
  }
  (void)httpd_resp_set_type(req, "application/json");
  const esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  http_record_request(req, status, route, start_us, unauthorized, uri_type);
  return err == ESP_OK;
}

bool http_is_body_too_large(const httpd_req_t* req) {
  return req && req->content_len > kHttpBodyLimitBytes;
}

bool http_authorize_request(httpd_req_t* req, const HttpConfig& config, bool log_noauth) {
  if (!req) {
    return false;
  }
  if (!config.enabled) {
    return false;
  }
  if (config.token[0] == '\0') {
    if (log_noauth) {
      char remote_ip[kHttpLogBuffer] = "unknown";
      http_load_remote_ip(req, remote_ip, sizeof(remote_ip));
      ESP_LOGW(kTag, "[HTTP] auth fail ip=%s reason=missing_token", remote_ip);
    }
    return false;
  }
  char auth_header[128] = "";
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
    if (log_noauth) {
      char remote_ip[kHttpLogBuffer] = "unknown";
      http_load_remote_ip(req, remote_ip, sizeof(remote_ip));
      ESP_LOGW(kTag, "[HTTP] auth fail ip=%s reason=no_header", remote_ip);
    }
    return false;
  }

  if (std::strncmp(auth_header, kHttpAuthBearerPrefix, std::strlen(kHttpAuthBearerPrefix)) != 0) {
    if (log_noauth) {
      char remote_ip[kHttpLogBuffer] = "unknown";
      http_load_remote_ip(req, remote_ip, sizeof(remote_ip));
      ESP_LOGW(kTag, "[HTTP] auth fail ip=%s reason=bad_scheme", remote_ip);
    }
    return false;
  }
  const char* presented = auth_header + std::strlen(kHttpAuthBearerPrefix);
  if (std::strcmp(presented, config.token) != 0) {
    if (log_noauth) {
      char remote_ip[kHttpLogBuffer] = "unknown";
      http_load_remote_ip(req, remote_ip, sizeof(remote_ip));
      ESP_LOGW(kTag, "[HTTP] auth fail ip=%s reason=bad_token", remote_ip);
    }
    return false;
  }
  return true;
}

bool http_load_config(HttpConfig& config) {
  http_set_defaults(config);

  nvs_handle_t handle = 0;
  if (nvs_open(kHttpConfigNamespace, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGI(kTag, "[HTTP][NVS] namespace missing, using defaults (enabled=0)");
    return false;
  }

  bool present = false;
  uint8_t enabled = 0;
  (void)wifi_read_u8_key(handle, "enabled", &enabled, &present);
  if (present && (enabled == 0 || enabled == 1)) {
    config.enabled = (enabled != 0);
  }
  ESP_LOGI(kTag, "[HTTP][NVS] key=enabled present=%d value=%d", present ? 1 : 0,
           config.enabled ? 1 : 0);

  uint32_t port = config.port;
  if (wifi_read_u32_key(handle, "port", &port, &present) && port > 0 && port <= kHttpMaxConfigPort) {
    config.port = static_cast<uint16_t>(port);
  }
  ESP_LOGI(kTag, "[HTTP][NVS] key=port present=%d value=%u", present ? 1 : 0,
           static_cast<unsigned>(config.port));

  bool tls_present = false;
  uint8_t tls_dev = config.tls_dev_mode ? 1 : 0;
  if (wifi_read_u8_key(handle, "tls_dev_mode", &tls_dev, &tls_present) &&
      (tls_dev == 0 || tls_dev == 1)) {
    config.tls_dev_mode = (tls_dev != 0);
  }
  ESP_LOGI(kTag, "[HTTP][NVS] key=tls_dev_mode present=%d value=%d", tls_present ? 1 : 0,
           config.tls_dev_mode ? 1 : 0);

  bool token_present = false;
  if (wifi_read_string_key(handle, "token", config.token, sizeof(config.token), &token_present) &&
      token_present && config.token[0] != '\0') {
    ESP_LOGI(kTag, "[HTTP][NVS] key=token present=%d value=(set)", 1);
  } else {
    std::snprintf(config.token, sizeof(config.token), "%s", kHttpDefaultToken);
    ESP_LOGI(kTag, "[HTTP][NVS] key=token present=%d value=%s", token_present ? 1 : 0,
             "(default)");
  }
  nvs_close(handle);
  return true;
}

esp_err_t http_health_handler(httpd_req_t* req) {
  const uint64_t start_us = esp_timer_get_time();
  if (!req) {
    return ESP_FAIL;
  }
  if (req->method != HTTP_GET) {
    const char payload[] = "{\"error\":\"method_not_allowed\"}";
    (void)http_send_json(req, payload, 405, kHttpRouteHealth, start_us, false, HttpUriType::kHealth);
    return ESP_OK;
  }
  if (http_is_body_too_large(req)) {
    const char payload[] = "{\"error\":\"payload_too_large\"}";
    (void)http_send_json(req, payload, 413, kHttpRouteHealth, start_us, false,
                         HttpUriType::kHealth);
    return ESP_OK;
  }
  char payload[kHttpJsonBuffer];
  const time_t now = time(nullptr);
  const int rc = std::snprintf(payload, sizeof(payload),
                               "{\"service\":\"luce\",\"stage\":%d,\"status\":\"ok\","
                               "\"build\":\"%s %s\",\"sha\":\"%s\",\"uptime_s\":%llu}",
                               LUCE_STAGE, __DATE__, __TIME__, LUCE_GIT_SHA,
                               static_cast<unsigned long long>(now));
  if (rc <= 0) {
    const char err[] = "{\"error\":\"response_failed\"}";
    (void)http_send_json(req, err, 500, kHttpRouteHealth, start_us, false, HttpUriType::kHealth);
    return ESP_OK;
  }
  (void)http_send_json(req, payload, 200, kHttpRouteHealth, start_us, false, HttpUriType::kHealth);
  return ESP_OK;
}

esp_err_t http_info_handler(httpd_req_t* req) {
  const uint64_t start_us = esp_timer_get_time();
  if (!req) {
    return ESP_FAIL;
  }
  if (req->method != HTTP_GET) {
    const char payload[] = "{\"error\":\"method_not_allowed\"}";
    (void)http_send_json(req, payload, 405, kHttpRouteInfo, start_us, false, HttpUriType::kInfo);
    return ESP_OK;
  }
  if (http_is_body_too_large(req)) {
    const char payload[] = "{\"error\":\"payload_too_large\"}";
    (void)http_send_json(req, payload, 413, kHttpRouteInfo, start_us, false, HttpUriType::kInfo);
    return ESP_OK;
  }
  if (!http_authorize_request(req, g_http_config, true)) {
    const char payload[] = "{\"error\":\"unauthorized\"}";
    (void)http_send_json(req, payload, 401, kHttpRouteInfo, start_us, true, HttpUriType::kInfo);
    return ESP_OK;
  }
  char payload[kHttpJsonBuffer];
  const int rc =
      std::snprintf(payload, sizeof(payload),
                    "{\"service\":\"luce\",\"stage\":%d,\"wifi_ip\":\"%s\",\"http_enabled\":%d,"
                    "\"http_port\":%u,\"tls\":\"self_signed\"}",
                    LUCE_STAGE, g_wifi_ip, g_http_config.enabled ? 1 : 0, g_http_config.port);
  if (rc <= 0) {
    const char err[] = "{\"error\":\"response_failed\"}";
    (void)http_send_json(req, err, 500, kHttpRouteInfo, start_us, true, HttpUriType::kInfo);
    return ESP_OK;
  }
  (void)http_send_json(req, payload, 200, kHttpRouteInfo, start_us, false, HttpUriType::kInfo);
  return ESP_OK;
}

esp_err_t http_state_handler(httpd_req_t* req) {
  const uint64_t start_us = esp_timer_get_time();
  if (!req) {
    return ESP_FAIL;
  }
  if (req->method != HTTP_GET) {
    const char payload[] = "{\"error\":\"method_not_allowed\"}";
    (void)http_send_json(req, payload, 405, kHttpRouteState, start_us, false, HttpUriType::kState);
    return ESP_OK;
  }
  if (http_is_body_too_large(req)) {
    const char payload[] = "{\"error\":\"payload_too_large\"}";
    (void)http_send_json(req, payload, 413, kHttpRouteState, start_us, false, HttpUriType::kState);
    return ESP_OK;
  }
  if (!http_authorize_request(req, g_http_config, true)) {
    const char payload[] = "{\"error\":\"unauthorized\"}";
    (void)http_send_json(req, payload, 403, kHttpRouteState, start_us, true, HttpUriType::kState);
    return ESP_OK;
  }
  char payload[kHttpJsonBuffer];
  char relay_mask[24] = {0};
  char button_mask[24] = {0};
  format_mcp_mask_line(relay_mask, sizeof(relay_mask), g_relay_mask, g_button_mask,
                       McpMaskFormat::kStatusCommand);

#if LUCE_HAS_NTP
  const int64_t now = static_cast<int64_t>(time(nullptr));
  const time_t ntp_last_sync = ntp_last_sync_unix();
  const int64_t sync_age = (now >= 0 && now >= static_cast<int64_t>(ntp_last_sync))
                              ? (now - static_cast<int64_t>(ntp_last_sync))
                              : 0;
  const char* ntp_state = ntp_state_name(ntp_state_snapshot());
  char ntp_utc_buf[64] = "n/a";
  if (ntp_last_sync > 0) {
    ntp_format_utc(ntp_utc_buf, sizeof(ntp_utc_buf), ntp_last_sync);
  }
  const int rc = std::snprintf(payload, sizeof(payload),
                               "{\"state\":\"%s\",\"wifi_ip\":\"%s\",\"relay\":\"%s\",\"buttons\":\"%s\","
                               "\"ntp_state\":\"%s\",\"ntp_unix\":%lld,\"ntp_age_s\":%lld,"
                               "\"ntp_utc\":\"%s\",\"requests\":%lu,\"unauth\":%lu}",
                               http_state_name(g_http_runtime.state), g_wifi_ip, relay_mask, button_mask,
                               ntp_state, static_cast<long long>(ntp_last_sync),
                               static_cast<long long>(sync_age), ntp_utc_buf,
                               static_cast<unsigned long>(g_http_runtime.request_count),
                               static_cast<unsigned long>(g_http_runtime.unauthorized_count));
  (void)rc;
#else
  const int rc = std::snprintf(payload, sizeof(payload),
                               "{\"state\":\"%s\",\"wifi_ip\":\"%s\",\"relay\":\"%s\",\"buttons\":\"%s\","
                               "\"requests\":%lu,\"unauth\":%lu}",
                               http_state_name(g_http_runtime.state), g_wifi_ip, relay_mask, button_mask,
                               static_cast<unsigned long>(g_http_runtime.request_count),
                               static_cast<unsigned long>(g_http_runtime.unauthorized_count));
#endif
  if (rc <= 0) {
    const char err[] = "{\"error\":\"response_failed\"}";
    (void)http_send_json(req, err, 500, kHttpRouteState, start_us, true, HttpUriType::kState);
    return ESP_OK;
  }
  (void)http_send_json(req, payload, 200, kHttpRouteState, start_us, false, HttpUriType::kState);
  return ESP_OK;
}

void http_register_handlers(httpd_handle_t server) {
  if (!server) {
    http_set_runtime_state(HttpState::kFailed, "handler registration no server");
    return;
  }

  bool ok = true;
  const httpd_uri_t health_uri = {
      .uri = kHttpRouteHealth,
      .method = HTTP_GET,
      .handler = http_health_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t info_uri = {
      .uri = kHttpRouteInfo,
      .method = HTTP_GET,
      .handler = http_info_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t state_uri = {
      .uri = kHttpRouteState,
      .method = HTTP_GET,
      .handler = http_state_handler,
      .user_ctx = nullptr,
  };
  if (httpd_register_uri_handler(server, &health_uri) != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP] register uri failed: %s", kHttpRouteHealth);
    ok = false;
  }
  if (httpd_register_uri_handler(server, &info_uri) != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP] register uri failed: %s", kHttpRouteInfo);
    ok = false;
  }
  if (httpd_register_uri_handler(server, &state_uri) != ESP_OK) {
    ESP_LOGW(kTag, "[HTTP] register uri failed: %s", kHttpRouteState);
    ok = false;
  }

  if (!ok) {
    const esp_err_t stop_rc = httpd_ssl_stop(server);
    g_http_server = nullptr;
    g_http_runtime.started = false;
    http_set_runtime_state(HttpState::kFailed, "handler registration failed");
    if (stop_rc != ESP_OK) {
      g_http_runtime.error_count += 1;
      ESP_LOGW(kTag, "[HTTP] shutdown after handler registration failure: %s", esp_err_to_name(stop_rc));
    } else {
      g_http_runtime.error_count += 1;
      ESP_LOGI(kTag, "[HTTP] stopped after handler registration failure");
    }
  }
}

void http_start_if_ready() {
  if (!g_http_config.enabled) {
    return;
  }
  if (g_http_runtime.started) {
    return;
  }
  if (!wifi_has_ip()) {
    http_set_runtime_state(HttpState::kReady, "waiting for wifi");
    return;
  }
  if (!g_http_config.tls_dev_mode) {
    ESP_LOGW(kTag, "[HTTP] tls_dev_mode=0 but no alternate provisioning supported in firmware yet");
  }
  httpd_ssl_config_t https_config = HTTPD_SSL_CONFIG_DEFAULT();
  https_config.servercert = reinterpret_cast<const uint8_t*>(kHttpDevCert);
  https_config.servercert_len = sizeof(kHttpDevCert) - 1;
  https_config.prvtkey_pem = reinterpret_cast<const uint8_t*>(kHttpDevPrivateKey);
  https_config.prvtkey_len = sizeof(kHttpDevPrivateKey) - 1;
  https_config.httpd.server_port = g_http_config.port;
  https_config.port_secure = g_http_config.port;
  ESP_LOGI(kTag, "[HTTP] starting with port=%u", g_http_config.port);
  http_set_runtime_state(HttpState::kInit, "creating server");
  const esp_err_t err = httpd_ssl_start(&g_http_server, &https_config);
  if (err != ESP_OK) {
    g_http_runtime.started = false;
    http_set_runtime_state(HttpState::kFailed, "startup failed");
    ESP_LOGW(kTag, "[HTTP] FAILED err=%s", esp_err_to_name(err));
    return;
  }
  http_register_handlers(g_http_server);
  if (g_http_runtime.state == HttpState::kFailed) {
    return;
  }
  g_http_runtime.started = true;
  http_set_runtime_state(HttpState::kStarted, "running");
  ESP_LOGI(kTag, "[HTTP] started");
  ESP_LOGI(kTag, "[HTTP] route=%s, %s, %s", kHttpRouteHealth, kHttpRouteInfo, kHttpRouteState);
}

void http_shutdown() {
  if (!g_http_runtime.started || !g_http_server) {
    g_http_runtime.started = false;
    g_http_runtime.state = HttpState::kReady;
    return;
  }
  const esp_err_t err = httpd_ssl_stop(g_http_server);
  g_http_server = nullptr;
  g_http_runtime.started = false;
  if (err != ESP_OK) {
    g_http_runtime.error_count += 1;
    ESP_LOGW(kTag, "[HTTP] shutdown failed: %s", esp_err_to_name(err));
    http_set_runtime_state(HttpState::kFailed, "shutdown failed");
    return;
  }
  http_set_runtime_state(HttpState::kReady, "stopped");
  ESP_LOGI(kTag, "[HTTP] stopped");
}

void http_startup() {
  if (!http_load_config(g_http_config)) {
    g_http_runtime.state = HttpState::kDisabled;
  }
  if (!g_http_config.enabled) {
    g_http_runtime.state = HttpState::kDisabled;
    g_http_runtime.started = false;
    ESP_LOGI(kTag, "[HTTP] enabled=%d", 0);
    return;
  }
  g_http_runtime.state = HttpState::kInit;
  ESP_LOGI(kTag, "[HTTP] enabled=%d", 1);
  http_start_if_ready();
}

void http_handle_wifi_got_ip() {
  if (!g_http_config.enabled) {
    return;
  }
  if (g_http_runtime.started) {
    return;
  }
  http_start_if_ready();
}

void http_handle_wifi_lost_ip() {
  if (!g_http_runtime.started) {
    return;
  }
  http_shutdown();
}

void cli_cmd_http_status() {
#if LUCE_HAS_CLI
  ESP_LOGI(kTag,
           "CLI command http.status: enabled=%d started=%d state=%s port=%u requests=%lu unauth=%lu errors=%lu",
           g_http_config.enabled ? 1 : 0, g_http_runtime.started ? 1 : 0,
           http_state_name(g_http_runtime.state), g_http_config.port,
           static_cast<unsigned long>(g_http_runtime.request_count),
           static_cast<unsigned long>(g_http_runtime.unauthorized_count),
           static_cast<unsigned long>(g_http_runtime.error_count));
#else
  ESP_LOGW(kTag, "CLI command http.status: unsupported (LUCE_HAS_HTTP=0)");
#endif
}
#endif  // LUCE_HAS_HTTP

// CLI helpers.
void cli_cmd_mqtt_status() {
#if LUCE_HAS_MQTT
  mqtt_log_status_line();
#else
  ESP_LOGW(kTag, "CLI command mqtt.status: unsupported (LUCE_HAS_MQTT=0)");
#endif
}

void cli_cmd_mqtt_pubtest() {
#if LUCE_HAS_MQTT
  if (!g_mqtt_config.enabled) {
    ESP_LOGW(kTag, "CLI command mqtt.pubtest: mqtt disabled");
    return;
  }
  if (!g_mqtt_runtime.connected) {
    ESP_LOGW(kTag, "CLI command mqtt.pubtest: not connected");
    return;
  }
  const int rc = mqtt_publish_test_payload();
  if (rc < 0) {
    ESP_LOGW(kTag, "CLI command mqtt.pubtest: publish rc=%d", rc);
  } else {
    ESP_LOGI(kTag, "CLI command mqtt.pubtest: publish rc=%d", rc);
  }
#else
  ESP_LOGW(kTag, "CLI command mqtt.pubtest: unsupported (LUCE_HAS_MQTT=0)");
#endif
}
#if 0
    std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "%s", reason);
    return;
  }
  g_mqtt_runtime.state = state;
  std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "%s", reason);
  std::snprintf(g_mqtt_runtime.last_state, sizeof(g_mqtt_runtime.last_state), "%s", mqtt_state_name(state));
  ESP_LOGI(kTag, "[MQTT][LIFECYCLE] state=%s reason=%s", mqtt_state_name(state), reason);
}

bool mqtt_load_dev_ca_pem(char* out, std::size_t out_size) {
  if (!out || out_size == 0) {
    return false;
  }
  const std::size_t len = std::strlen(kMqttDefaultCaEmbedded);
  if (len + 1 >= out_size) {
    ESP_LOGW(kTag, "[MQTT] embedded CA truncated");
    return false;
  }
  std::snprintf(out, out_size, "%s", kMqttDefaultCaEmbedded);
  return true;
}

bool mqtt_build_topic(char* out, std::size_t out_size, const char* suffix) {
  if (!out || out_size == 0) {
    return false;
  }
  if (!suffix || suffix[0] == '\0') {
    std::snprintf(out, out_size, "%s", g_mqtt_config.base_topic[0] != '\0' ? g_mqtt_config.base_topic
                                                                         : kMqttDefaultBaseTopic);
    return true;
  }
  std::snprintf(out, out_size, "%s/%s", g_mqtt_config.base_topic[0] != '\0' ? g_mqtt_config.base_topic
                                                                             : kMqttDefaultBaseTopic,
                suffix);
  return true;
}

bool mqtt_load_config(MqttConfig& config) {
  mqtt_set_defaults(config);
  nvs_handle_t handle = 0;
  const esp_err_t open_err = nvs_open(kMqttConfigNamespace, NVS_READONLY, &handle);
  if (open_err != ESP_OK) {
    ESP_LOGW(kTag, "[MQTT][NVS] namespace missing, using defaults (enabled=0): %s",
             esp_err_to_name(open_err));
    return false;
  }

  bool present = false;
  uint8_t enabled = 0;
  if (wifi_read_u8_key(handle, "enabled", &enabled, &present) && (enabled == 0 || enabled == 1)) {
    config.enabled = (enabled != 0);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=enabled present=%d value=%d", present ? 1 : 0,
           config.enabled ? 1 : 0);

  bool string_present = false;
  char tmp[1536] = {0};
  if (wifi_read_string_key(handle, "uri", tmp, sizeof(tmp), &string_present) && string_present) {
    std::snprintf(config.uri, sizeof(config.uri), "%s", tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=uri present=%d value=%s", string_present ? 1 : 0, config.uri);

  if (wifi_read_string_key(handle, "client_id", tmp, sizeof(tmp), &string_present) && string_present) {
    std::snprintf(config.client_id, sizeof(config.client_id), "%s", tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=client_id present=%d value=%s", string_present ? 1 : 0,
           config.client_id[0] != '\0' ? config.client_id : "(auto)");

  if (wifi_read_string_key(handle, "base_topic", tmp, sizeof(tmp), &string_present) && string_present &&
      tmp[0] != '\0') {
    std::snprintf(config.base_topic, sizeof(config.base_topic), "%s", tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=base_topic present=%d value=%s", string_present ? 1 : 0,
           config.base_topic[0] != '\0' ? config.base_topic : kMqttDefaultBaseTopic);

  if (wifi_read_string_key(handle, "username", tmp, sizeof(tmp), &string_present) && string_present) {
    std::snprintf(config.username, sizeof(config.username), "%s", tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=username present=%d value=%s", string_present ? 1 : 0,
           config.username[0] != '\0' ? "(set)" : "(none)");

  if (wifi_read_string_key(handle, "password", tmp, sizeof(tmp), &string_present) && string_present) {
    std::snprintf(config.password, sizeof(config.password), "%s", tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=password present=%d value=%s", string_present ? 1 : 0,
           config.password[0] != '\0' ? "(set)" : "(none)");

  uint8_t tls_enabled = 0;
  if (wifi_read_u8_key(handle, "tls_enabled", &tls_enabled, &present) && (tls_enabled == 0 || tls_enabled == 1)) {
    config.tls_enabled = (tls_enabled == 1);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=tls_enabled present=%d value=%d", present ? 1 : 0,
           config.tls_enabled ? 1 : 0);

  if (wifi_read_string_key(handle, "ca_pem_source", tmp, sizeof(tmp), &string_present) && string_present) {
    std::snprintf(config.ca_pem_source, sizeof(config.ca_pem_source), "%s", tmp);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=ca_pem_source present=%d value=%s", string_present ? 1 : 0,
           config.ca_pem_source);

  if (wifi_read_string_key(handle, "ca_pem", tmp, sizeof(tmp), &string_present) && string_present &&
      tmp[0] != '\0' && std::strncmp(config.ca_pem_source, "embedded", 8) != 0) {
    std::snprintf(config.ca_pem, sizeof(config.ca_pem), "%s", tmp);
  }
  if (wifi_read_u32_key(handle, "qos", &config.qos, &present)) {
    config.qos = mqtt_clamp_u32_u(config.qos, kMqttMinQos, kMqttMaxQos, kMqttMinQos);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=qos present=%d value=%lu", present ? 1 : 0,
           static_cast<unsigned long>(config.qos));

  uint32_t keepalive = config.keepalive_s;
  if (wifi_read_u32_key(handle, "keepalive_s", &keepalive, &present)) {
    config.keepalive_s = mqtt_clamp_u32_u(keepalive, kMqttMinKeepaliveS, kMqttMaxKeepaliveS,
                                          kMqttDefaultKeepaliveS);
  }
  ESP_LOGI(kTag, "[MQTT][NVS] key=keepalive_s present=%d value=%lu", present ? 1 : 0,
           static_cast<unsigned long>(config.keepalive_s));

  if (std::strncmp(config.ca_pem_source, "embedded", 8) == 0) {
    if (!mqtt_load_dev_ca_pem(config.ca_pem, sizeof(config.ca_pem))) {
      ESP_LOGW(kTag, "[MQTT] embedded CA unavailable; TLS may fail without cert");
    }
  }

  nvs_close(handle);
  return true;
}

void mqtt_handle_wifi_got_ip() {
  if (!g_mqtt_config.enabled) {
    return;
  }
  g_mqtt_runtime.next_retry_tick = 0;
  g_mqtt_runtime.retry_backoff_ms = kMqttMinRetryBackoffMs;
}

void mqtt_handle_wifi_lost_ip() {
  if (g_mqtt_runtime.client) {
    (void)esp_mqtt_client_stop(g_mqtt_runtime.client);
    (void)esp_mqtt_client_disconnect(g_mqtt_runtime.client);
    g_mqtt_runtime.connected = false;
    g_mqtt_runtime.last_connected = false;
    mqtt_set_state(MqttState::kBackoff, "wifi lost");
    g_mqtt_runtime.next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
  }
}

void mqtt_log_status_line() {
  char time_text[64] = "n/a";
  if (g_mqtt_runtime.last_pub_unix > 0) {
    const time_t published = g_mqtt_runtime.last_pub_unix;
    const std::tm tm_info = *std::gmtime(&published);
    std::snprintf(time_text, sizeof(time_text), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour,
                  tm_info.tm_min, tm_info.tm_sec);
  }
  ESP_LOGI(kTag,
           "CLI command mqtt.status: state=%s enabled=%d connected=%d reason=%s connect_count=%lu "
           "publish_count=%lu last_topic=%s last_payload=%d last_latency_ms=%u last_unix=%lld utc=%s "
           "uri=%s",
           mqtt_state_name(g_mqtt_runtime.state), g_mqtt_config.enabled ? 1 : 0,
           g_mqtt_runtime.connected ? 1 : 0, g_mqtt_runtime.last_reason,
           static_cast<unsigned long>(g_mqtt_runtime.connect_count),
           static_cast<unsigned long>(g_mqtt_runtime.publish_count), g_mqtt_runtime.last_topic,
           g_mqtt_runtime.last_pub_payload, static_cast<unsigned>(0),
           static_cast<long long>(g_mqtt_runtime.last_pub_unix), time_text,
           g_mqtt_runtime.effective_uri[0] != '\0' ? g_mqtt_runtime.effective_uri : "(unset)");
}

void mqtt_event_handler(void*,
                       esp_event_base_t,
                       int32_t event_id,
                       void* event_data) {
  const auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
  switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
      g_mqtt_runtime.connected = true;
      g_mqtt_runtime.last_connected = true;
      g_mqtt_runtime.last_connect_tick = xTaskGetTickCount();
      g_mqtt_runtime.connect_count += 1;
      g_mqtt_runtime.reconnects = 0;
      g_mqtt_runtime.retry_backoff_ms = kMqttMinRetryBackoffMs;
      g_mqtt_runtime.next_retry_tick = 0;
      mqtt_set_state(MqttState::kConnected, "connected");
      const TickType_t now = xTaskGetTickCount();
      const uint32_t latency_ms = static_cast<uint32_t>(now > g_mqtt_runtime.last_connect_tick ? 0 : 0);
      ESP_LOGI(kTag, "[MQTT] connected rc=%d client_id=%s latency_ms=%lu", event ? event->msg_id : 0,
               g_mqtt_config.client_id[0] != '\0' ? g_mqtt_config.client_id : "(auto)",
               static_cast<unsigned long>(latency_ms));
      mqtt_publish_payload("/status", "online", 6, static_cast<uint8_t>(kMqttMinQos));
      break;
    }
    case MQTT_EVENT_DISCONNECTED: {
      g_mqtt_runtime.connected = false;
      g_mqtt_runtime.last_disconnected = true;
      g_mqtt_runtime.last_disconnect_tick = xTaskGetTickCount();
      mqtt_set_state(MqttState::kBackoff, "disconnected");
      if (g_mqtt_runtime.retry_backoff_ms < kMqttMaxRetryBackoffMs) {
        g_mqtt_runtime.retry_backoff_ms = g_mqtt_runtime.retry_backoff_ms * 2;
        if (g_mqtt_runtime.retry_backoff_ms == 0 ||
            g_mqtt_runtime.retry_backoff_ms > kMqttMaxRetryBackoffMs) {
          g_mqtt_runtime.retry_backoff_ms = kMqttMaxRetryBackoffMs;
        }
      }
      g_mqtt_runtime.next_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
      const int reason_code = event ? event->error_handle && event->error_handle->error_type ? event->error_handle->error_type : 0 : 0;
      ESP_LOGW(kTag, "[MQTT] disconnected reason=%d reconnect_ms=%lu", reason_code,
               static_cast<unsigned long>(g_mqtt_runtime.retry_backoff_ms));
      break;
    }
    case MQTT_EVENT_PUBLISHED: {
      const int msg_id = event ? event->msg_id : -1;
      ESP_LOGI(kTag, "[MQTT] publish acked msg_id=%d", msg_id);
      break;
    }
    case MQTT_EVENT_ERROR: {
      int source = 0;
      int code = 0;
      if (event && event->error_handle) {
        source = event->error_handle->error_type;
        code = event->error_handle->connect_return_code;
      }
      g_mqtt_runtime.state = MqttState::kFailed;
      std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "mqtt error=%d code=%d",
                    source, code);
      ESP_LOGW(kTag, "[MQTT] event error source=%d code=%d", source, code);
      mqtt_set_state(MqttState::kFailed, g_mqtt_runtime.last_reason);
      g_mqtt_runtime.reconnects += 1;
      if (g_mqtt_runtime.reconnects >= kMqttMaxReconnectCycles) {
        g_mqtt_runtime.reconnects = kMqttMaxReconnectCycles;
      }
      break;
    }
    default:
      break;
  }
}

bool mqtt_build_client_config(esp_mqtt_client_config_t& out_cfg) {
  out_cfg = {};
  out_cfg.network.reconnect_timeout_ms = kMqttMaxRetryBackoffMs;
  out_cfg.network.disable_auto_reconnect = true;
  out_cfg.session.keepalive = static_cast<int>(g_mqtt_config.keepalive_s);
  out_cfg.task.stack_size = static_cast<int>(kMqttTaskStackWords);
  out_cfg.task.priority = 2;

  std::strncpy(g_mqtt_runtime.effective_uri, g_mqtt_config.uri,
               sizeof(g_mqtt_runtime.effective_uri) - 1);
  g_mqtt_runtime.effective_uri[sizeof(g_mqtt_runtime.effective_uri) - 1] = '\0';
  if (g_mqtt_config.tls_enabled && std::strstr(g_mqtt_runtime.effective_uri, "mqtt") == nullptr) {
    std::snprintf(g_mqtt_runtime.effective_uri, sizeof(g_mqtt_runtime.effective_uri), "mqtts://%s",
                 g_mqtt_config.uri);
  } else if (g_mqtt_config.tls_enabled && std::strstr(g_mqtt_runtime.effective_uri, "mqtts://") == nullptr &&
             std::strstr(g_mqtt_runtime.effective_uri, "mqtt://") == nullptr &&
             std::strstr(g_mqtt_runtime.effective_uri, "ssl://") == nullptr &&
             std::strstr(g_mqtt_runtime.effective_uri, "tcp://") == nullptr &&
             std::strstr(g_mqtt_runtime.effective_uri, "ws://") == nullptr &&
             std::strstr(g_mqtt_runtime.effective_uri, "wss://") == nullptr) {
    std::snprintf(g_mqtt_runtime.effective_uri, sizeof(g_mqtt_runtime.effective_uri), "mqtts://%s",
                 g_mqtt_config.uri);
  }
  out_cfg.broker.address.uri = g_mqtt_runtime.effective_uri;

  if (g_mqtt_config.client_id[0] != '\0') {
    out_cfg.credentials.client_id = g_mqtt_config.client_id;
  }
  if (g_mqtt_config.username[0] != '\0') {
    out_cfg.credentials.username = g_mqtt_config.username;
  }
  if (g_mqtt_config.password[0] != '\0') {
    out_cfg.credentials.authentication.password = g_mqtt_config.password;
  }

  if (g_mqtt_config.tls_enabled) {
    if (std::strncmp(g_mqtt_config.ca_pem_source, "embedded", 8) == 0) {
      if (g_mqtt_config.ca_pem[0] != '\0') {
        out_cfg.broker.verification.certificate = g_mqtt_config.ca_pem;
        out_cfg.broker.verification.certificate_len = std::strlen(g_mqtt_config.ca_pem);
      } else {
        ESP_LOGW(kTag, "[MQTT] TLS requested but no embedded cert available");
      }
    } else if (std::strncmp(g_mqtt_config.ca_pem_source, "nvs", 3) == 0) {
      ESP_LOGW(kTag, "[MQTT] TLS cert source=nvs not implemented");
    } else if (std::strncmp(g_mqtt_config.ca_pem_source, "partition", 9) == 0) {
      ESP_LOGW(kTag, "[MQTT] TLS cert source=partition not implemented");
    } else {
      ESP_LOGW(kTag, "[MQTT] TLS cert source invalid=%s defaulting embedded", g_mqtt_config.ca_pem_source);
      if (g_mqtt_config.ca_pem[0] != '\0') {
        out_cfg.broker.verification.certificate = g_mqtt_config.ca_pem;
        out_cfg.broker.verification.certificate_len = std::strlen(g_mqtt_config.ca_pem);
      }
    }
  }

  char lwt_topic[128] = {0};
  if (mqtt_build_topic(lwt_topic, sizeof(lwt_topic), "lwt")) {
    out_cfg.session.last_will.topic = lwt_topic;
  }
  out_cfg.session.last_will.msg = "offline";
  out_cfg.session.last_will.msg_len = 7;
  out_cfg.session.last_will.qos = 0;
  out_cfg.session.last_will.retain = 0;
  return true;
}

int mqtt_publish_payload(const char* topic, const char* payload, int payload_len, uint8_t qos) {
  if (!g_mqtt_runtime.client || !topic || !payload) {
    return ESP_ERR_INVALID_ARG;
  }
  if (qos > kMqttMaxQos) {
    qos = kMqttMaxQos;
  }
  const TickType_t start = xTaskGetTickCount();
  const int rc = esp_mqtt_client_publish(g_mqtt_runtime.client, topic, payload, payload_len, qos, 0);
  const uint32_t latency_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start);
  std::snprintf(g_mqtt_runtime.last_topic, sizeof(g_mqtt_runtime.last_topic), "%s", topic);
  g_mqtt_runtime.last_pub_rc = rc;
  g_mqtt_runtime.last_payload_size = payload_len;
  g_mqtt_runtime.last_publish_ms = latency_ms;
  g_mqtt_runtime.last_pub_rc = rc;
  g_mqtt_runtime.publish_count += 1;
  g_mqtt_runtime.last_pub_unix = time(nullptr);

  if (rc == ESP_OK) {
    g_mqtt_runtime.last_has_time = true;
    g_mqtt_runtime.publish_backoff_failures = 0;
    ESP_LOGI(kTag, "[MQTT] publish rc=%d topic=%s size=%d latency_ms=%lu", rc, topic, payload_len,
             static_cast<unsigned long>(latency_ms));
  } else {
    g_mqtt_runtime.publish_backoff_failures += 1;
    g_mqtt_runtime.last_has_time = false;
    ESP_LOGW(kTag, "[MQTT] publish failed rc=%d topic=%s size=%d", rc, topic, payload_len);
  }
  return rc;
}

int mqtt_publish_status(bool include_time) {
  char topic[128] = {0};
  if (!mqtt_build_topic(topic, sizeof(topic), "telemetry/state")) {
    return ESP_ERR_INVALID_STATE;
  }
  char payload[kMqttPayloadBuffer] = {0};
  char utc_now[64] = {0};
  char fw[LUCE_PROJECT_VERSION?0:1];

  if (include_time && g_ntp_runtime.last_sync_unix > 0) {
    ntp_format_utc(utc_now, sizeof(utc_now), time(nullptr));
  } else {
    std::snprintf(utc_now, sizeof(utc_now), "n/a");
  }

  char relay_buf[16] = {0};
  char button_buf[16] = {0};
  format_mcp_mask_line(relay_buf, sizeof(relay_buf), g_relay_mask, g_button_mask,
                       McpMaskFormat::kStatusCommand);
  std::snprintf(payload, sizeof(payload),
               "{\"fw\":\"%s\",\"stage\":%d,\"ip\":\"%s\",\"relay\":\"%s\",\"buttons\":\"%s\",\"wifi\":%d,"
               "\"utc\":\"%s\",\"connected\":%s}",
               LUCE_PROJECT_VERSION, LUCE_STAGE, g_wifi_ip, relay_buf, button_buf, g_wifi_last_rssi,
               utc_now, g_mqtt_runtime.connected ? "true" : "false");
  return mqtt_publish_payload(topic, payload, static_cast<int>(std::strlen(payload)),
                             static_cast<uint8_t>(g_mqtt_config.qos));
}

void mqtt_task(void*) {
  if (!mqtt_load_config(g_mqtt_config)) {
    mqtt_set_defaults(g_mqtt_config);
  }

  std::strncpy(g_mqtt_runtime.effective_uri, g_mqtt_config.uri, sizeof(g_mqtt_runtime.effective_uri) - 1);

  if (!g_mqtt_config.enabled) {
    mqtt_set_state(MqttState::kDisabled, "disabled-by-config");
    mqtt_log_status_line();
  } else {
    mqtt_set_state(MqttState::kInitialized, "loaded");
  }

  TickType_t last_status_tick = xTaskGetTickCount();
  TickType_t last_publish_tick = xTaskGetTickCount();

  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if (!g_mqtt_config.enabled) {
      if (g_mqtt_runtime.client) {
        (void)esp_mqtt_client_stop(g_mqtt_runtime.client);
        (void)esp_mqtt_client_destroy(g_mqtt_runtime.client);
        g_mqtt_runtime.client = nullptr;
      }
      g_mqtt_runtime.connected = false;
      mqtt_set_state(MqttState::kDisabled, "disabled-by-config");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!wifi_has_ip()) {
      if (g_mqtt_runtime.connected || g_mqtt_runtime.client) {
        mqtt_handle_wifi_lost_ip();
      } else {
        mqtt_set_state(MqttState::kBackoff, "no-ip");
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!g_mqtt_runtime.client) {
      esp_mqtt_client_config_t cfg {};
      if (mqtt_build_client_config(cfg)) {
        g_mqtt_runtime.client = esp_mqtt_client_init(&cfg);
        if (g_mqtt_runtime.client) {
          const esp_err_t ev = esp_mqtt_client_register_event(g_mqtt_runtime.client, ESP_EVENT_ANY_ID,
                                                             &mqtt_event_handler, nullptr);
          if (ev != ESP_OK) {
            ESP_LOGW(kTag, "[MQTT] register event failed: %s", esp_err_to_name(ev));
          }
          mqtt_set_state(MqttState::kConnecting, "start");
          const esp_err_t start_rc = esp_mqtt_client_start(g_mqtt_runtime.client);
          if (start_rc != ESP_OK) {
            ESP_LOGW(kTag, "[MQTT] start failed: %s", esp_err_to_name(start_rc));
            (void)esp_mqtt_client_destroy(g_mqtt_runtime.client);
            g_mqtt_runtime.client = nullptr;
            mqtt_set_state(MqttState::kFailed, "start failed");
            g_mqtt_runtime.reconnects += 1;
            g_mqtt_runtime.next_retry_tick = now + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
            g_mqtt_runtime.retry_backoff_ms = std::min(g_mqtt_runtime.retry_backoff_ms * 2, kMqttMaxRetryBackoffMs);
          } else {
            g_mqtt_runtime.state = MqttState::kConnecting;
          }
        } else {
          mqtt_set_state(MqttState::kFailed, "client alloc failed");
          g_mqtt_runtime.next_retry_tick = now + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
        }
      }
    } else if (!g_mqtt_runtime.connected) {
      if (g_mqtt_runtime.state != MqttState::kConnecting && g_mqtt_runtime.state != MqttState::kBackoff) {
        mqtt_set_state(MqttState::kConnecting, "reconnect");
      }
      if (now >= g_mqtt_runtime.next_retry_tick &&
          (g_mqtt_runtime.state == MqttState::kBackoff || g_mqtt_runtime.state == MqttState::kFailed)) {
        const esp_err_t rc = esp_mqtt_client_reconnect(g_mqtt_runtime.client);
        if (rc != ESP_OK) {
          mqtt_set_state(MqttState::kFailed, "reconnect failed");
          g_mqtt_runtime.reconnects += 1;
          if (g_mqtt_runtime.retry_backoff_ms < kMqttMaxRetryBackoffMs) {
            g_mqtt_runtime.retry_backoff_ms = std::min(g_mqtt_runtime.retry_backoff_ms * 2, kMqttMaxRetryBackoffMs);
          }
          g_mqtt_runtime.next_retry_tick = now + pdMS_TO_TICKS(g_mqtt_runtime.retry_backoff_ms);
          ESP_LOGW(kTag, "[MQTT] reconnect rc=%s retry_ms=%lu", esp_err_to_name(rc),
                   static_cast<unsigned long>(g_mqtt_runtime.retry_backoff_ms));
        } else {
          g_mqtt_runtime.state = MqttState::kConnecting;
          g_mqtt_runtime.next_retry_tick = 0;
        }
      }
    } else if (g_mqtt_runtime.connected &&
               (now - last_publish_tick) >= pdMS_TO_TICKS(kMqttPublishIntervalMs)) {
      const int rc = mqtt_publish_status(true);
      if (rc != ESP_OK) {
        g_mqtt_runtime.last_reason[0] = 0;
        std::snprintf(g_mqtt_runtime.last_reason, sizeof(g_mqtt_runtime.last_reason), "publish rc=%d", rc);
      }
      last_publish_tick = now;
    }

    if ((now - last_status_tick) >= pdMS_TO_TICKS(kMqttLogIntervalMs)) {
      last_status_tick = now;
      mqtt_log_status_line();
    }

    vTaskDelay(pdMS_TO_TICKS(kMqttLoopDelayMs));
  }
}

void mqtt_startup() {
  if (xTaskCreate(mqtt_task, "mqtt", kMqttTaskStackWords, nullptr, 2, &g_mqtt_task) != pdPASS) {
    ESP_LOGW(kTag, "[MQTT] startup: task create failed");
  }
}
#endif  // Disable duplicate MQTT block

#if LUCE_HAS_NTP
constexpr char kNtpConfigNamespace[] = "ntp";
constexpr uint32_t kNtpDefaultSyncTimeoutS = 30;
constexpr uint32_t kNtpDefaultSyncIntervalS = 3600;
constexpr uint32_t kNtpMinSyncTimeoutS = 5;
constexpr uint32_t kNtpMaxSyncTimeoutS = 600;
constexpr uint32_t kNtpMinSyncIntervalS = 60;
constexpr uint32_t kNtpMaxSyncIntervalS = 86400;
constexpr uint32_t kNtpMaxRetryAttempts = 3;
constexpr uint32_t kNtpRetryDelayMs = 3000;
constexpr TickType_t kNtpLoopDelayMs = 500;
constexpr TickType_t kNtpLogIntervalMs = 10000;

struct NtpRuntimeState {
  bool active = false;
  NtpState state = NtpState::kDisabled;
  time_t last_sync_unix = 0;
  TickType_t last_sync_tick = 0;
  TickType_t sync_start_tick = 0;
  TickType_t next_attempt_tick = 0;
  uint32_t sync_retry_count = 0;
  char last_reason[64] = "not-initialized";
};

TaskHandle_t g_ntp_task = nullptr;
NtpConfig g_ntp_config {};
NtpRuntimeState g_ntp_runtime {};

const char* ntp_state_name(NtpState state) {
  switch (state) {
    case NtpState::kDisabled:
      return "DISABLED";
    case NtpState::kUnsynced:
      return "UNSYNCED";
    case NtpState::kSyncing:
      return "SYNCING";
    case NtpState::kSynced:
      return "SYNCED";
    case NtpState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

time_t ntp_last_sync_unix() {
  return g_ntp_runtime.last_sync_unix;
}

NtpState ntp_state_snapshot() {
  return g_ntp_runtime.state;
}

void ntp_set_last_reason(const char* reason) {
  if (!reason) {
    reason = "unknown";
  }
  std::snprintf(g_ntp_runtime.last_reason, sizeof(g_ntp_runtime.last_reason), "%s", reason);
}

void ntp_set_state(NtpState state, const char* reason) {
  if (g_ntp_runtime.state == state) {
    if (reason && reason[0] != '\0') {
      ntp_set_last_reason(reason);
    }
    return;
  }
  g_ntp_runtime.state = state;
  ntp_set_last_reason(reason);
  ESP_LOGI(kTag, "[NTP][LIFECYCLE] state=%s reason=%s retry=%lu", ntp_state_name(state),
           g_ntp_runtime.last_reason, static_cast<unsigned long>(g_ntp_runtime.sync_retry_count));
}

void ntp_set_default_config(NtpConfig& config) {
  config = {};
  config.enabled = false;
  std::snprintf(config.server1, sizeof(config.server1), "pool.ntp.org");
  std::snprintf(config.server2, sizeof(config.server2), "time.google.com");
  config.server3[0] = '\0';
  config.sync_timeout_s = kNtpDefaultSyncTimeoutS;
  config.sync_interval_s = kNtpDefaultSyncIntervalS;
}

uint32_t ntp_clamp_u32(uint32_t value, uint32_t min_value, uint32_t max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

bool ntp_load_config(NtpConfig& config) {
  ntp_set_default_config(config);

  nvs_handle_t handle = 0;
  const esp_err_t open_err = nvs_open(kNtpConfigNamespace, NVS_READONLY, &handle);
  if (open_err != ESP_OK) {
    ESP_LOGW(kTag, "[NTP][NVS] namespace missing, using defaults (enabled=0): %s",
             esp_err_to_name(open_err));
    return false;
  }

  bool present = false;
  uint8_t enabled = 0;
  if (wifi_read_u8_key(handle, "enabled", &enabled, &present) && (enabled == 0 || enabled == 1)) {
    config.enabled = enabled == 1;
  } else if (present) {
    ESP_LOGW(kTag, "[NTP][NVS] key=enabled invalid, using default OFF");
  }
  ESP_LOGI(kTag, "[NTP][NVS] key=enabled present=%d value=%d", present ? 1 : 0,
           config.enabled ? 1 : 0);

  char tmp[33] = "";
  bool present_server = false;
  if (wifi_read_string_key(handle, "server1", tmp, sizeof(tmp), &present_server) && present_server) {
    std::snprintf(config.server1, sizeof(config.server1), "%s", tmp);
  }
  ESP_LOGI(kTag, "[NTP][NVS] key=server1 present=%d value=%s", present_server ? 1 : 0, config.server1);

  if (wifi_read_string_key(handle, "server2", tmp, sizeof(tmp), &present_server) && present_server) {
    std::snprintf(config.server2, sizeof(config.server2), "%s", tmp);
  }
  ESP_LOGI(kTag, "[NTP][NVS] key=server2 present=%d value=%s", present_server ? 1 : 0, config.server2);

  if (wifi_read_string_key(handle, "server3", tmp, sizeof(tmp), &present_server) && present_server) {
    std::snprintf(config.server3, sizeof(config.server3), "%s", tmp);
  }
  ESP_LOGI(kTag, "[NTP][NVS] key=server3 present=%d value=%s", present_server ? 1 : 0, config.server3[0] ? config.server3 : "(empty)");

  uint32_t timeout_s = config.sync_timeout_s;
  if (wifi_read_u32_key(handle, "sync_timeout_s", &timeout_s, &present)) {
    config.sync_timeout_s = ntp_clamp_u32(timeout_s, kNtpMinSyncTimeoutS, kNtpMaxSyncTimeoutS);
  }
  ESP_LOGI(kTag, "[NTP][NVS] key=sync_timeout_s present=%d value=%lu", present ? 1 : 0,
           static_cast<unsigned long>(config.sync_timeout_s));

  uint32_t interval_s = config.sync_interval_s;
  if (wifi_read_u32_key(handle, "sync_interval_s", &interval_s, &present)) {
    config.sync_interval_s = ntp_clamp_u32(interval_s, kNtpMinSyncIntervalS, kNtpMaxSyncIntervalS);
  }
  ESP_LOGI(kTag, "[NTP][NVS] key=sync_interval_s present=%d value=%lu", present ? 1 : 0,
           static_cast<unsigned long>(config.sync_interval_s));

  nvs_close(handle);
  return true;
}

bool ntp_has_time() {
  return (g_ntp_runtime.state == NtpState::kSynced) && (g_ntp_runtime.last_sync_unix > 0);
}

std::size_t ntp_format_utc(char* out, std::size_t out_size, time_t epoch_seconds) {
  if (!out || out_size == 0) {
    return 0;
  }
  if (epoch_seconds <= 0) {
    return std::snprintf(out, out_size, "n/a");
  }

  std::tm tm_info{};
  if (gmtime_r(&epoch_seconds, &tm_info) == nullptr) {
    return std::snprintf(out, out_size, "n/a");
  }
  return std::snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_info.tm_year + 1900,
                       tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min,
                       tm_info.tm_sec);
}

void ntp_configure_service(const NtpConfig& config) {
  esp_sntp_stop();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

  if (config.server1[0] != '\0') {
    esp_sntp_setservername(0, config.server1);
  }
  if (config.server2[0] != '\0') {
    esp_sntp_setservername(1, config.server2);
  }
  if (config.server3[0] != '\0') {
    esp_sntp_setservername(2, config.server3);
  }
  esp_sntp_set_sync_interval(config.sync_interval_s * 1000U);
}

void ntp_log_status_line() {
  const time_t now = time(nullptr);
  const uint64_t age_s = ntp_has_time() && now >= g_ntp_runtime.last_sync_unix
                           ? static_cast<uint64_t>(now - g_ntp_runtime.last_sync_unix)
                           : 0;
  char utc[64] = "n/a";
  ntp_format_utc(utc, sizeof(utc), g_ntp_runtime.last_sync_unix);

  if (ntp_has_time()) {
    ESP_LOGI(kTag, "[NTP] status state=%s synced_unix=%lld age=%llus utc=%s",
             ntp_state_name(g_ntp_runtime.state), static_cast<long long>(g_ntp_runtime.last_sync_unix),
             static_cast<unsigned long long>(age_s), utc);
  } else {
    ESP_LOGI(kTag, "[NTP] status state=%s unsynced_reason=\"%s\"", ntp_state_name(g_ntp_runtime.state),
             g_ntp_runtime.last_reason);
  }
}

void ntp_task(void*) {
  if (!ntp_load_config(g_ntp_config)) {
    ntp_set_default_config(g_ntp_config);
  }

  if (!g_ntp_config.enabled) {
    ntp_set_state(NtpState::kDisabled, "not-enabled");
    ntp_log_status_line();
  } else {
    ntp_set_state(NtpState::kUnsynced, "boot:waiting-for-wifi");
  }

  TickType_t last_log_tick = xTaskGetTickCount();

  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if (!g_ntp_config.enabled) {
      if (esp_sntp_enabled()) {
        esp_sntp_stop();
      }
      g_ntp_runtime.active = false;
      ntp_set_state(NtpState::kDisabled, "not-enabled");
    } else if (!wifi_has_ip()) {
      if (esp_sntp_enabled()) {
        esp_sntp_stop();
      }
      g_ntp_runtime.active = false;
      ntp_set_state(NtpState::kUnsynced, "waiting for wifi ip");
    } else {
      if (!g_ntp_runtime.active && now >= g_ntp_runtime.next_attempt_tick &&
          g_ntp_runtime.state != NtpState::kSynced) {
        ntp_configure_service(g_ntp_config);
        g_ntp_runtime.sync_start_tick = now;
        g_ntp_runtime.next_attempt_tick = 0;
        g_ntp_runtime.active = true;
        ntp_set_state(NtpState::kSyncing, "sntp init");
        esp_sntp_init();
      }

      if (g_ntp_runtime.state == NtpState::kSyncing) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
          g_ntp_runtime.last_sync_unix = time(nullptr);
          g_ntp_runtime.last_sync_tick = now;
          g_ntp_runtime.sync_retry_count = 0;
          g_ntp_runtime.active = false;
          ntp_set_state(NtpState::kSynced, "time synchronized");
          ntp_log_status_line();
          char utc[64] = "n/a";
          const time_t synced = g_ntp_runtime.last_sync_unix;
          ntp_format_utc(utc, sizeof(utc), synced);
          ESP_LOGI(kTag, "[NTP] first successful sync: unix=%lld utc=%s", static_cast<long long>(synced),
                   utc);
        } else {
          const TickType_t age_ms = now - g_ntp_runtime.sync_start_tick;
          if (age_ms > pdMS_TO_TICKS(g_ntp_config.sync_timeout_s * 1000U)) {
            g_ntp_runtime.sync_retry_count += 1;
            if (g_ntp_runtime.sync_retry_count >= kNtpMaxRetryAttempts) {
              ntp_set_state(NtpState::kFailed, "sync timeout: max retries reached");
              g_ntp_runtime.next_attempt_tick =
                  now + pdMS_TO_TICKS(g_ntp_config.sync_interval_s * 1000U);
            } else {
              char reason[64];
              std::snprintf(reason, sizeof(reason), "sync timeout: retry %lu/%lu",
                            static_cast<unsigned long>(g_ntp_runtime.sync_retry_count),
                            static_cast<unsigned long>(kNtpMaxRetryAttempts));
              ntp_set_state(NtpState::kFailed, reason);
              g_ntp_runtime.next_attempt_tick = now + pdMS_TO_TICKS(kNtpRetryDelayMs);
            }
            g_ntp_runtime.active = false;
            esp_sntp_stop();
          }
        }
      } else if (g_ntp_runtime.state == NtpState::kFailed) {
        if (now >= g_ntp_runtime.next_attempt_tick) {
          ntp_set_state(NtpState::kUnsynced, "retry window elapsed");
          g_ntp_runtime.sync_retry_count = 0;
          g_ntp_runtime.next_attempt_tick = 0;
        }
      } else if (g_ntp_runtime.state == NtpState::kSynced) {
        if ((now - g_ntp_runtime.last_sync_tick) >= pdMS_TO_TICKS(g_ntp_config.sync_interval_s * 1000U)) {
          ntp_set_state(NtpState::kUnsynced, "periodic resync due");
          g_ntp_runtime.next_attempt_tick = now;
          g_ntp_runtime.sync_retry_count = 0;
        }
      }
    }

    if ((now - last_log_tick) >= pdMS_TO_TICKS(kNtpLogIntervalMs)) {
      last_log_tick = now;
      ntp_log_status_line();
    }
    vTaskDelay(pdMS_TO_TICKS(kNtpLoopDelayMs));
  }
}

void ntp_startup() {
  if (g_ntp_task) {
    return;
  }
  if (xTaskCreate(ntp_task, "ntp", 4096, nullptr, 2, &g_ntp_task) != pdPASS) {
    ESP_LOGW(kTag, "[NTP] failed to create SNTP task");
    return;
  }
}
#endif  // LUCE_HAS_NTP

#if LUCE_HAS_NVS
const char* nvs_type_name(nvs_type_t type) {
  switch (type) {
    case NVS_TYPE_U8:
      return "U8";
    case NVS_TYPE_I8:
      return "I8";
    case NVS_TYPE_U16:
      return "U16";
    case NVS_TYPE_I16:
      return "I16";
    case NVS_TYPE_U32:
      return "U32";
    case NVS_TYPE_I32:
      return "I32";
    case NVS_TYPE_U64:
      return "U64";
    case NVS_TYPE_I64:
      return "I64";
    case NVS_TYPE_STR:
      return "STR";
    case NVS_TYPE_BLOB:
      return "BLOB";
    default:
      return "OTHER";
  }
}
void dump_nvs_value(nvs_handle_t handle, const nvs_entry_info_t& info) {
  switch (info.type) {
    case NVS_TYPE_U8: {
      uint8_t v = 0;
      if (nvs_get_u8(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u8): %u", v);
      }
      break;
    }
    case NVS_TYPE_I8: {
      int8_t v = 0;
      if (nvs_get_i8(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i8): %d", v);
      }
      break;
    }
    case NVS_TYPE_U16: {
      uint16_t v = 0;
      if (nvs_get_u16(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u16): %u", v);
      }
      break;
    }
    case NVS_TYPE_I16: {
      int16_t v = 0;
      if (nvs_get_i16(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i16): %d", v);
      }
      break;
    }
    case NVS_TYPE_U32: {
      uint32_t v = 0;
      if (nvs_get_u32(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u32): %lu", (unsigned long)v);
      }
      break;
    }
    case NVS_TYPE_I32: {
      int32_t v = 0;
      if (nvs_get_i32(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i32): %ld", (long)v);
      }
      break;
    }
    case NVS_TYPE_U64: {
      uint64_t v = 0;
      if (nvs_get_u64(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u64): 0x%016" PRIx64, v);
      }
      break;
    }
    case NVS_TYPE_I64: {
      int64_t v = 0;
      if (nvs_get_i64(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i64): %lld", (long long)v);
      }
      break;
    }
    case NVS_TYPE_STR: {
      size_t required = 0;
      if (nvs_get_str(handle, info.key, nullptr, &required) == ESP_OK && required > 0) {
        char str_val[129] = {0};
        size_t capacity = required < sizeof(str_val) ? required : sizeof(str_val);
        if (nvs_get_str(handle, info.key, str_val, &capacity) == ESP_OK) {
          ESP_LOGI(kTag, "    value (str): %s", str_val);
        }
      }
      break;
    }
    case NVS_TYPE_BLOB: {
      size_t required = 0;
      if (nvs_get_blob(handle, info.key, nullptr, &required) == ESP_OK && required > 0) {
        size_t copy_size = required < 32 ? required : 32;
        uint8_t data[32] = {0};
        if (nvs_get_blob(handle, info.key, data, &copy_size) == ESP_OK && copy_size > 0) {
          char blob_preview[33] = {0};
          for (size_t i = 0; i < copy_size; ++i) {
            std::snprintf(blob_preview + (i * 2), 3, "%02x", data[i]);
          }
          ESP_LOGI(kTag, "    value (blob, %u bytes): %s%s", (unsigned)required,
                   blob_preview, required > sizeof(data) ? "..." : "");
        }
      }
      break;
    }
    default:
      ESP_LOGW(kTag, "    value unsupported for type=%s", nvs_type_name(info.type));
      break;
  }
}

void dump_nvs_entries() {
  nvs_iterator_t iterator = nullptr;
  esp_err_t err = nvs_entry_find(nullptr, nullptr, NVS_TYPE_ANY, &iterator);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "NVS: no entries found");
    return;
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS entry scan failed: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(kTag, "NVS entries (namespace/key/type/value):");
  nvs_handle_t current = 0;
  std::string current_ns;
  while (true) {
    nvs_entry_info_t entry;
    if (nvs_entry_info(iterator, &entry) != ESP_OK) {
      break;
    }

    if (current_ns != entry.namespace_name) {
      if (current != 0) {
        nvs_close(current);
        current = 0;
      }
      if (nvs_open(entry.namespace_name, NVS_READONLY, &current) != ESP_OK) {
        ESP_LOGW(kTag, "NVS: failed to open namespace '%s'", entry.namespace_name);
        current = 0;
      }
      current_ns.assign(entry.namespace_name);
    }

    ESP_LOGI(kTag, "  ns=%s key=%s type=%s", entry.namespace_name, entry.key,
             nvs_type_name(entry.type));
    if (current != 0) {
      dump_nvs_value(current, entry);
    }

    err = nvs_entry_next(&iterator);
    if (err != ESP_OK) {
      break;
    }
  }

  if (current != 0) {
    nvs_close(current);
  }
  nvs_release_iterator(iterator);
  if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
    ESP_LOGW(kTag, "NVS entry scan ended with error: %s", esp_err_to_name(err));
  }
}

void update_boot_state_record() {
  ESP_LOGI(kTag, "NVS init: starting");
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "NVS init: nvs_flash_init returned %s; erasing and retrying", esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }

  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS init failed: %s", esp_err_to_name(err));
    return;
  }

  nvs_handle_t handle;
  err = nvs_open("boot", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS open failed: %s", esp_err_to_name(err));
    return;
  }

  uint32_t boot_count = 0;
  err = nvs_get_u32(handle, "boot_count", &boot_count);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    boot_count = 0;
  } else if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS get boot_count failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  boot_count += 1;
  err = nvs_set_u32(handle, "boot_count", boot_count);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS set boot_count failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  const uint32_t reset_reason = static_cast<uint32_t>(esp_reset_reason());
  err = nvs_set_u32(handle, "last_reset_reason", reset_reason);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS set last_reset_reason failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  err = nvs_commit(handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS commit failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }
  nvs_close(handle);

  ESP_LOGI(kTag, "NVS state: boot_count=%lu last_reset_reason=%lu", (unsigned long)boot_count,
           (unsigned long)reset_reason);
  dump_nvs_entries();
}
#endif  // LUCE_HAS_NVS

#if LUCE_HAS_I2C
constexpr i2c_port_t kI2CPort = I2C_NUM_0;
constexpr gpio_num_t kI2CSclPin = GPIO_NUM_22;
constexpr gpio_num_t kI2CSdaPin = GPIO_NUM_23;
// ITB (GPIO19) is used for MCP interrupt signaling; ITA is intentionally unused on this hardware.
constexpr gpio_num_t kMcpIntPin = GPIO_NUM_19;
constexpr uint32_t kI2CClockHz = 100000;
constexpr uint8_t kMcpAddress = 0x20;

// MCP23017 register map for BANK=0
constexpr uint8_t kIocon = 0x0A;
constexpr uint8_t kIodira = 0x00;
constexpr uint8_t kIodirb = 0x01;
constexpr uint8_t kGppua = 0x0C;
constexpr uint8_t kGppub = 0x0D;
constexpr uint8_t kGpioa = 0x12;
constexpr uint8_t kGpiob = 0x13;

// Relay channels are active LOW on MCP GPIOA for this board revision:
// written bit=1 means OFF, bit=0 means ON.
constexpr bool kRelayActiveHigh = false;
constexpr uint8_t kRelayOffValue = kRelayActiveHigh ? 0x00 : 0xFF;
constexpr uint8_t kI2CSampleAddressStart = 0x08;
constexpr uint8_t kI2CSampleAddressEnd = 0x77;
constexpr uint8_t kButtonDebounceThreshold = 3;
constexpr TickType_t kRelayStepDelay = pdMS_TO_TICKS(250);
constexpr TickType_t kButtonSamplePeriod = pdMS_TO_TICKS(40);
constexpr TickType_t kIntSamplePeriod = pdMS_TO_TICKS(200);

struct Mcp23017State {
  bool connected = false;
  uint8_t relay_mask = kRelayOffValue;
};

bool g_i2c_initialized = false;
bool g_mcp_available = false;
uint8_t g_relay_mask = kRelayOffValue;
uint8_t g_button_mask = 0x00;
#if LUCE_HAS_LCD
bool g_lcd_present = false;
#endif

struct I2cScanResult {
  bool mcp = false;
  bool lcd = false;
  int found_count = 0;
};

I2cScanResult scan_i2c_bus();

// LCD I2C backpack is fixed at 0x27 on the current hardware map and 3.3V bus-compatible.
constexpr uint8_t kLcdAddress = 0x27;

#if LUCE_HAS_LCD
// Common I2C backpack mapping assumed:
// P0=RS, P1=RW, P2=EN, P3=BL, P4=DB4, P5=DB5, P6=DB6, P7=DB7
constexpr uint8_t kLcdPcfRsBit = 0;
constexpr uint8_t kLcdPcfRwBit = 1;
constexpr uint8_t kLcdPcfEnBit = 2;
constexpr uint8_t kLcdPcfBacklightBit = 3;
constexpr uint8_t kLcdCols = 20;
constexpr uint8_t kLcdRows = 4;

class Pcf8574Hd44780 {
 public:
  Pcf8574Hd44780(i2c_port_t port, uint8_t address) : port_(port), address_(address) {}

  bool begin() {
    vTaskDelay(pdMS_TO_TICKS(50));

    if (!write_nibble(0x03, false) || !write_nibble(0x03, false) || !write_nibble(0x03, false) ||
        !write_nibble(0x02, false)) {
      return false;
    }
    if (!send_command(0x28) || !send_command(0x08) || !send_command(0x0C) ||
        !send_command(0x06) || !send_command(0x01)) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3));
    return true;
  }

  void set_mcp_ok(bool ok) { mcp_ok_ = ok; }

  bool write_status_lines(uint8_t relay_mask, uint8_t button_mask) {
    char line1[21] = {0};
    char line2[21] = {0};
    char line3[21] = {0};
    char line4[21] = {0};

    const uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
    std::snprintf(line1, sizeof(line1), "LUCE S3 %lus", (unsigned long)uptime_s);
    std::snprintf(line2, sizeof(line2), "I2C:%s MCP:%s", "ok", mcp_ok_ ? "ok" : "no");
    std::snprintf(line3, sizeof(line3), "REL:0x%02X", relay_mask);
    std::snprintf(line4, sizeof(line4), "BTN:0x%02X", button_mask);

    return write_line(0, line1) && write_line(1, line2) && write_line(2, line3) &&
           write_line(3, line4);
  }

  bool write_text_line(uint8_t row, const char* text) {
    char safe_text[21] = {0};
    size_t text_len = 0;
    if (text) {
      text_len = std::strlen(text);
      if (text_len > kLcdCols) {
        text_len = kLcdCols;
      }
    }
    for (size_t idx = 0; idx < kLcdCols; ++idx) {
      if (idx < text_len) {
        safe_text[idx] = text[idx];
      } else {
        safe_text[idx] = ' ';
      }
    }
    return write_line(row, safe_text);
  }

  bool write_text(uint8_t row, const char* text) {
    return write_text_line(row, text);
  }

 private:
  bool send_command(uint8_t cmd) { return send_byte(cmd, false); }

  bool send_byte(uint8_t value, bool is_data) {
    const uint8_t hi = (value >> 4) & 0x0F;
    const uint8_t lo = value & 0x0F;
    return write_nibble(hi, is_data) && write_nibble(lo, is_data);
  }

  bool write_nibble(uint8_t nibble, bool is_data) {
    const uint8_t half_byte = static_cast<uint8_t>((nibble & 0x0F) << 4);
    uint8_t frame = half_byte;
    frame &= static_cast<uint8_t>(~(1u << kLcdPcfRwBit));
    if (is_data) {
      frame |= static_cast<uint8_t>(1u << kLcdPcfRsBit);
    }
    frame |= static_cast<uint8_t>(1u << kLcdPcfBacklightBit);
    return pulse_en(frame);
  }

  bool pulse_en(uint8_t frame) {
    if (!write_pcf(frame | (1u << kLcdPcfEnBit))) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    if (!write_pcf(frame & ~(1u << kLcdPcfEnBit))) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
    return true;
  }

  bool write_pcf(uint8_t value) {
    return i2c_master_write_to_device(port_, address_, &value, sizeof(value), pdMS_TO_TICKS(100)) ==
           ESP_OK;
  }

  bool set_cursor(uint8_t row, uint8_t col) {
    static constexpr uint8_t kRowAddress[] = {0x00, 0x40, 0x14, 0x54};
    if (row >= kLcdRows || col >= kLcdCols) {
      return false;
    }
    return send_command(static_cast<uint8_t>(0x80 | (kRowAddress[row] + col)));
  }

  bool write_line(uint8_t row, const char* text) {
    char padded[kLcdCols + 1] = {0};
    for (size_t idx = 0; idx < kLcdCols; ++idx) {
      padded[idx] = ' ';
    }
    if (text) {
      for (size_t idx = 0; idx < kLcdCols && text[idx] != '\0'; ++idx) {
        padded[idx] = text[idx];
      }
    }

    if (!set_cursor(row, 0)) {
      return false;
    }

    for (uint8_t idx = 0; idx < kLcdCols; ++idx) {
      if (!send_byte(static_cast<uint8_t>(padded[idx]), true)) {
        return false;
      }
    }
    return true;
  }

  i2c_port_t port_;
  uint8_t address_;
  bool mcp_ok_ = false;
};

Pcf8574Hd44780 g_lcd(kI2CPort, kLcdAddress);
#endif  // LUCE_HAS_LCD

esp_err_t i2c_probe_device(uint8_t address, TickType_t timeout_ticks = pdMS_TO_TICKS(20)) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (!cmd) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = i2c_master_start(cmd);
  if (err == ESP_OK) {
    err = i2c_master_write_byte(cmd, static_cast<uint8_t>((address << 1) | I2C_MASTER_WRITE),
                                true);
  }
  if (err == ESP_OK) {
    err = i2c_master_stop(cmd);
  }
  if (err == ESP_OK) {
    err = i2c_master_cmd_begin(kI2CPort, cmd, timeout_ticks);
  }

  i2c_cmd_link_delete(cmd);
  return err;
}

uint8_t relay_mask_for_channel(int channel) {
  const uint8_t bit = static_cast<uint8_t>(1u << channel);
  return kRelayActiveHigh ? bit : static_cast<uint8_t>(kRelayOffValue & ~bit);
}

esp_err_t mcp_write_reg(uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  return i2c_master_write_to_device(kI2CPort, kMcpAddress, payload, sizeof(payload),
                                   pdMS_TO_TICKS(100));
}

esp_err_t mcp_read_reg(uint8_t reg, uint8_t* value) {
  if (!value) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_master_write_read_device(kI2CPort, kMcpAddress, &reg, sizeof(reg), value,
                                     sizeof(*value), pdMS_TO_TICKS(100));
}

void update_lcd_status() {
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  if (!g_lcd_present) {
    return;
  }
  if (!g_mcp_available) {
    g_lcd.write_status_lines(kRelayOffValue, g_button_mask);
    return;
  }
  g_lcd.write_status_lines(g_relay_mask, g_button_mask);
#endif
#endif
}

bool init_i2c() {
  // Contract:
  // - ok=true only after successful i2c_param_config() and driver install.
  // - on failure, g_i2c_initialized must remain false and caller may retry.
  i2c_config_t config;
  std::memset(&config, 0, sizeof(config));
  config.mode = I2C_MODE_MASTER;
  config.sda_io_num = kI2CSdaPin;
  config.scl_io_num = kI2CSclPin;
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.master.clk_speed = kI2CClockHz;

  esp_err_t err = i2c_param_config(kI2CPort, &config);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "I2C config failed: %s", esp_err_to_name(err));
    return false;
  }

  err = i2c_driver_install(kI2CPort, I2C_MODE_MASTER, 0, 0, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "I2C install failed: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(kTag, "I2C init: port=%d SCL=%d SDA=%d", kI2CPort, kI2CSclPin, kI2CSdaPin);
  return true;
}

InitPathResult init_i2c_contract() {
  const bool ok = init_i2c();
  return ok ? init_result_success() : init_result_failure(ESP_FAIL);
}

InitPathResult run_i2c_scan_flow(I2cScanResult& scan, const char* context, bool attach_lcd) {
  scan = I2cScanResult{};
  InitPathResult contract = init_i2c_contract();
  if (!contract.ok) {
    if (context) {
      ESP_LOGW(kTag, "%s: init_i2c() failed (%s)", context, init_status_name(contract.status));
    }
    return contract;
  }
  g_i2c_initialized = true;

  scan = scan_i2c_bus();
  if (context) {
    ESP_LOGI(kTag, "%s: summary found=%d mcp=%d lcd=%d", context, scan.found_count, scan.mcp,
             scan.lcd);
  }
#if LUCE_HAS_LCD
  if (attach_lcd) {
    g_lcd_present = false;
  }
#endif
  if (!attach_lcd) {
    return init_result_success();
  }

#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  if (scan.lcd) {
    const bool lcd_present = g_lcd.begin();
    if (!lcd_present) {
      ESP_LOGW(kTag, "LCD detected at 0x%02X but initialization failed; continuing without LCD",
               kLcdAddress);
    } else {
      g_lcd.set_mcp_ok(false);
      g_lcd_present = true;
      ESP_LOGI(kTag, "LCD initialized at 0x%02X", kLcdAddress);
      g_lcd.write_status_lines(kRelayOffValue, 0x00);
    }
  }
#else
  (void)attach_lcd;
#endif
#else
  (void)attach_lcd;
#endif
  return init_result_success();
}

I2cScanResult scan_i2c_bus() {
  I2cScanResult result;

  for (uint8_t addr = kI2CSampleAddressStart; addr <= kI2CSampleAddressEnd; ++addr) {
    if (i2c_probe_device(addr) == ESP_OK) {
      result.found_count += 1;
      result.mcp = result.mcp || (addr == kMcpAddress);
      result.lcd = result.lcd || (addr == kLcdAddress);
      ESP_LOGI(kTag, "I2C found 0x%02X", addr);
    }
  }

  if (result.found_count == 0) {
    ESP_LOGW(kTag, "I2C scan: no devices detected on bus");
  } else {
    ESP_LOGI(kTag, "I2C scan summary: %d device(s) detected", result.found_count);
  }

  if (result.mcp) {
    ESP_LOGI(kTag, "I2C scan expects MCP23017 at 0x20: found");
  } else {
    ESP_LOGW(kTag, "I2C scan expects MCP23017 at 0x20: not found");
  }

  if (result.lcd) {
    ESP_LOGI(kTag, "I2C scan expects LCD at 0x%02X: found", kLcdAddress);
  } else {
    ESP_LOGW(kTag, "I2C scan expects LCD at 0x%02X: not found", kLcdAddress);
  }

  return result;
}

bool init_mcp23017(Mcp23017State& state) {
  // Contract:
  // - ok=true only when MCP presence probe and all setup writes succeed.
  // - on failure, caller must treat relay/button state as unavailable.
  ESP_LOGI(kTag, "MCP23017 init: start");
  state = {};
  if (i2c_probe_device(kMcpAddress, pdMS_TO_TICKS(100)) != ESP_OK) {
    ESP_LOGW(kTag, "MCP23017 not detected at 0x%02X", kMcpAddress);
    return false;
  }

  constexpr uint8_t kIoconValue = 0x00;
  const esp_err_t errors[] = {
      mcp_write_reg(kIocon, kIoconValue),
      mcp_write_reg(kIodira, 0x00),  // Port A outputs
      mcp_write_reg(kIodirb, 0xFF),  // Port B inputs
      mcp_write_reg(kGppub, 0xFF),   // Pull-ups on buttons
      mcp_write_reg(kGpioa, kRelayOffValue),
      mcp_write_reg(kGppua, 0x00),
      mcp_write_reg(kGpiob, 0x00),
  };

  for (std::size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    const esp_err_t err = errors[i];
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "MCP23017 init register write failed: %s", esp_err_to_name(err));
      return false;
    }
  }

  state.connected = true;
  state.relay_mask = kRelayOffValue;
  g_mcp_available = true;
  g_relay_mask = kRelayOffValue;
  g_button_mask = 0x00;
  // Buttons on GPIOB are configured as inputs with weak pull-ups (idle-high, active-low semantics).
  ESP_LOGI(kTag, "MCP23017 configured: relays OFF, buttons pullups enabled, IOCON=0x%02X", kIoconValue);
  return true;
}

esp_err_t set_relay_mask(Mcp23017State& state, uint8_t mask) {
  const esp_err_t err = mcp_write_reg(kGpioa, mask);
  if (err == ESP_OK) {
    state.relay_mask = mask;
    g_relay_mask = mask;
    ESP_LOGI(kTag, "Relay mask set: 0x%02X", mask);
    update_lcd_status();
  }
  return err;
}

#if LUCE_HAS_CLI
esp_err_t set_relay_mask_safe(uint8_t mask) {
  if (!g_i2c_initialized || !g_mcp_available) {
    return ESP_ERR_INVALID_STATE;
  }
  Mcp23017State state;
  state.connected = g_mcp_available;
  state.relay_mask = g_relay_mask;
  return set_relay_mask(state, mask);
}

uint8_t relay_mask_for_channel_state(int channel, bool on, uint8_t current_mask) {
  const uint8_t bit = static_cast<uint8_t>(1u << channel);
  if (kRelayActiveHigh) {
    return on ? static_cast<uint8_t>(current_mask | bit) : static_cast<uint8_t>(current_mask & ~bit);
  }
  return on ? static_cast<uint8_t>(current_mask & ~bit) : static_cast<uint8_t>(current_mask | bit);
}
#endif  // LUCE_HAS_CLI

bool read_button_inputs(uint8_t* value) {
  return mcp_read_reg(kGpiob, value) == ESP_OK;
}

void configure_int_pin() {
  gpio_config_t conf;
  std::memset(&conf, 0, sizeof(conf));
  conf.pin_bit_mask = (1ULL << kMcpIntPin);
  conf.mode = GPIO_MODE_INPUT;
  conf.pull_up_en = GPIO_PULLUP_ENABLE;
  conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  conf.intr_type = GPIO_INTR_DISABLE;

  const esp_err_t err = gpio_config(&conf);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "MCP INT pin config failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(kTag, "MCP INTB pin configured on GPIO%d", kMcpIntPin);
  }
}

void run_stage2_diagnostics() {
  log_heap_integrity("run_stage2_diagnostics_enter");
  I2cScanResult scan{};
  const InitPathResult scan_result = run_i2c_scan_flow(scan, nullptr, true);
  if (!scan_result.ok) {
    ESP_LOGW(kTag, "Stage2 abort: I2C bus init failed");
    return;
  }
  g_i2c_initialized = true;
  bool lcd_present = false;

#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  g_lcd_present = scan_result.ok && g_lcd_present;
  lcd_present = g_lcd_present;
#else
  (void)scan;
  (void)lcd_present;
#endif
#else
  (void)scan;
  (void)lcd_present;
#endif

  Mcp23017State mcp_state;
  if (!init_mcp23017(mcp_state)) {
    ESP_LOGW(kTag, "Stage2 diagnostics degraded: MCP23017 missing or unresponsive");
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
    if (lcd_present) {
      g_lcd.write_status_lines(kRelayOffValue, 0x00);
    }
#endif
#endif
    g_mcp_available = false;
    g_relay_mask = kRelayOffValue;
    g_button_mask = 0x00;
    update_lcd_status();
    while (true) {
      static TickType_t last_status_tick = 0;
      const TickType_t now = xTaskGetTickCount();
      if ((now - last_status_tick) >= pdMS_TO_TICKS(2000)) {
        last_status_tick = now;
        const uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
        log_runtime_status_line(uptime_s, true, false, kRelayOffValue, 0x00);
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
      if (lcd_present && !g_lcd.write_status_lines(kRelayOffValue, 0x00)) {
        ESP_LOGW(kTag, "LCD update failed (LCD-only mode)");
      }
#else
      (void)lcd_present;
#endif
#endif
      }

      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  configure_int_pin();
  g_mcp_available = mcp_state.connected;
  g_relay_mask = mcp_state.relay_mask;
  g_button_mask = 0x00;

  if (set_relay_mask(mcp_state, kRelayOffValue) != ESP_OK) {
    ESP_LOGW(kTag, "Stage2 diagnostics degraded: cannot write initial relay state");
    g_mcp_available = false;
    return;
  }


#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
  g_lcd.set_mcp_ok(scan.mcp);
  if (lcd_present) {
    g_lcd.write_status_lines(mcp_state.relay_mask, 0x00);
  }
#endif
#endif

  uint8_t debounce_counts[8] = {0};
  uint8_t debounced_buttons = 0x00;
  uint8_t last_reported_buttons = 0x00;
  uint8_t current_buttons = 0x00;
  uint8_t channel = 0;
  uint8_t relay_mask = kRelayOffValue;
  uint8_t button_mask = 0x00;
  bool have_button_snapshot = false;
  TickType_t last_relay_tick = 0;
  TickType_t last_button_tick = 0;
  TickType_t last_int_tick = 0;
  TickType_t last_status_tick = 0;
  int last_int_level = gpio_get_level(kMcpIntPin);

  ESP_LOGI(kTag,
           "Entering Stage2 runtime diagnostics loop (1 ch at a time relay sweep + button sample)");
  while (true) {
    const TickType_t now = xTaskGetTickCount();

    if ((now - last_relay_tick) >= kRelayStepDelay) {
      relay_mask = relay_mask_for_channel(channel);
      if (set_relay_mask(mcp_state, relay_mask) == ESP_OK) {
        ESP_LOGI(kTag, "Relay channel=%d sweep mask=0x%02X", static_cast<int>(channel), relay_mask);
        g_relay_mask = relay_mask;
      }
      channel = static_cast<uint8_t>((channel + 1) % 8);
      last_relay_tick = now;
    }

    if ((now - last_button_tick) >= kButtonSamplePeriod) {
      last_button_tick = now;
      if (read_button_inputs(&current_buttons)) {
        if (!have_button_snapshot) {
          debounced_buttons = current_buttons;
          last_reported_buttons = current_buttons;
          button_mask = debounced_buttons;
          have_button_snapshot = true;
          ESP_LOGI(kTag, "Button init mask: 0x%02X (1=pressed? check wiring polarity)", current_buttons);
        } else {
          bool changed = false;
          for (uint8_t bit = 0; bit < 8; ++bit) {
            bool raw = (current_buttons & (1u << bit)) != 0;
            bool stable = (debounced_buttons & (1u << bit)) != 0;

            if (raw == stable) {
              debounce_counts[bit] = 0;
              continue;
            }

            if (++debounce_counts[bit] >= kButtonDebounceThreshold) {
              debounced_buttons ^= (1u << bit);
              debounce_counts[bit] = 0;
              changed = true;
            }
          }
          if (changed && debounced_buttons != last_reported_buttons) {
            button_mask = debounced_buttons;
            g_button_mask = debounced_buttons;
            ESP_LOGI(kTag, "Button mask change: 0x%02X", debounced_buttons);
            last_reported_buttons = debounced_buttons;
          }
        }
      } else {
        ESP_LOGW(kTag, "Button read failed; retaining last stable mask");
      }
    }

    if ((now - last_int_tick) >= kIntSamplePeriod) {
      last_int_tick = now;
      const int level = gpio_get_level(kMcpIntPin);
      if (level != last_int_level) {
        ESP_LOGI(kTag, "MCP INTB changed to %d", level);
        last_int_level = level;
      }
    }

    if ((now - last_status_tick) >= pdMS_TO_TICKS(2000)) {
      last_status_tick = now;
      const uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
      log_runtime_status_line(uptime_s, true, true, relay_mask, button_mask);
#if LUCE_HAS_LCD
#if LUCE_STAGE4_LCD
      if (lcd_present) {
        if (!g_lcd.write_status_lines(relay_mask, button_mask)) {
          ESP_LOGW(kTag, "LCD update failed");
        }
      }
#endif
#endif
    }

    log_stage4_watermarks("diag_loop");
    if ((now - last_status_tick) >= pdMS_TO_TICKS(10000)) {
      log_heap_integrity("diag_loop_health");
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#if LUCE_HAS_CLI
namespace {
constexpr char kCliNetNamespace[] = "cli_net";
constexpr uint16_t kCliNetDefaultPort = 2323;
constexpr uint32_t kCliNetDefaultIdleTimeoutS = 120;
constexpr uint32_t kCliNetDefaultMaxAuthFails = 3;

constexpr uint16_t kCliNetMaxLine = 256;
constexpr uint16_t kCliNetMaxArgs = 8;
constexpr uint16_t kCliNetResponseBuffer = 256;

struct CliNetConfig {
  bool enabled = false;
  uint16_t port = kCliNetDefaultPort;
  char token[33] = {0};
  uint32_t idle_timeout_s = kCliNetDefaultIdleTimeoutS;
};

struct CliNetRuntime {
  bool initialized = false;
  bool session_open = false;
  bool session_authed = false;
  uint16_t active_port = kCliNetDefaultPort;
  int active_fd = -1;
  TickType_t last_activity_tick = 0;
  char active_ip[48] = "";
  uint32_t auth_failures = 0;
};

#if LUCE_HAS_TCP_CLI
CliNetConfig g_cli_net_config {};
CliNetRuntime g_cli_net_runtime {};
#endif
}

void cli_trim(char* line) {
  char* write = line;
  for (const char* read = line; *read != '\0'; ++read) {
    if (*read == '\r' || *read == '\n') {
      continue;
    }
    if (write != read) {
      *write = *read;
    }
    ++write;
  }
  *write = '\0';
}

bool parse_u32_with_base(const char* text, int base, uint32_t* value, char* token_context = nullptr) {
  if (!text || !*text || !value) {
    return false;
  }
  if (token_context) {
    std::strncpy(token_context, text, 31);
    token_context[31] = '\0';
  }

  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text, &end, base);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *value = static_cast<uint32_t>(parsed);
  return true;
}

std::size_t tokenize_cli_line(char* line, char* argv[], std::size_t max_args) {
  std::size_t argc = 0;
  char* next_token = nullptr;
  char* token = strtok_r(line, " \t", &next_token);
  while (token && argc < max_args) {
    argv[argc++] = token;
    token = strtok_r(nullptr, " \t", &next_token);
  }
  return argc;
}

void log_cli_arguments(const char* command, int argc, char* argv[]) {
  ESP_LOGI(kTag, "CLI cmd='%s' argc=%d", command, argc);
  for (int i = 0; i < argc; ++i) {
    ESP_LOGI(kTag, "CLI arg[%d]='%s'", i, argv[i]);
  }
}

void cli_print_help() {
  ESP_LOGI(kTag,
           "CLI commands: help, status, nvs_dump, i2c_scan, mcp_read, relay_set, relay_mask, buttons, lcd_print, reboot");
  ESP_LOGI(kTag, "  - wifi.status (if stage5 Wi-Fi active)");
  ESP_LOGI(kTag, "  - wifi.scan (if stage5 Wi-Fi active)");
  ESP_LOGI(kTag, "  - time.status (if stage6 NTP active)");
  ESP_LOGI(kTag, "  - mdns.status (if stage7 mDNS active)");
  ESP_LOGI(kTag, "  - mqtt.status (if stage9 MQTT active)");
  ESP_LOGI(kTag, "  - mqtt.pubtest (if stage9 MQTT active)");
  ESP_LOGI(kTag, "  - http.status (if stage10 HTTPS active)");
  ESP_LOGI(kTag, "  - cli_net.status");
  ESP_LOGI(kTag, "  - mcp_read <gpioa|gpiob>");
  ESP_LOGI(kTag, "  - relay_set <0..7> <0|1>");
  ESP_LOGI(kTag, "  - relay_mask <hex> (8-bit register value)");
  ESP_LOGI(kTag, "  - buttons");
  ESP_LOGI(kTag, "  - lcd_print <text> (if LCD enabled)");
}

void cli_cmd_status() {
  log_status_health_lines();
}

void cli_cmd_nvs_dump() {
#if LUCE_HAS_NVS
  ESP_LOGI(kTag, "CLI command nvs_dump: executing");
  dump_nvs_entries();
  ESP_LOGI(kTag, "CLI command nvs_dump: done");
#else
  ESP_LOGW(kTag, "CLI command nvs_dump: unsupported (LUCE_HAS_NVS=0)");
#endif
}

void cli_cmd_i2c_scan() {
#if LUCE_HAS_I2C
  I2cScanResult scan{};
  const InitPathResult scan_result = run_i2c_scan_flow(scan, "CLI command i2c_scan", false);
  g_i2c_initialized = scan_result.ok;
  if (!scan_result.ok) {
    ESP_LOGW(kTag, "CLI command i2c_scan: summary unavailable (scan not initialized)");
  }
#else
  ESP_LOGW(kTag, "CLI command i2c_scan: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_mcp_read(char* port) {
#if LUCE_HAS_I2C
  if (!g_mcp_available) {
    ESP_LOGW(kTag, "CLI command mcp_read: MCP unavailable");
    return;
  }
  uint8_t value = 0x00;
  const uint8_t reg = (std::strcmp(port, "gpioa") == 0 || std::strcmp(port, "a") == 0) ? kGpioa
                                                                                       : kGpiob;
  const esp_err_t err = mcp_read_reg(reg, &value);
  ESP_LOGI(kTag, "CLI command mcp_read %s rc=%s value=0x%02X", port, esp_err_to_name(err), value);
#else
  (void)port;
  ESP_LOGW(kTag, "CLI command mcp_read: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_relay_set(int channel, int on_off) {
#if LUCE_HAS_I2C
  const uint8_t new_mask = relay_mask_for_channel_state(channel, on_off != 0, g_relay_mask);
  const esp_err_t err = set_relay_mask_safe(new_mask);
  if (err == ESP_OK) {
    g_relay_mask = new_mask;
  }
  ESP_LOGI(kTag, "CLI command relay_set: ch=%d value=%d new_mask=0x%02X rc=%s", channel, on_off, new_mask,
           esp_err_to_name(err));
#else
  (void)channel;
  (void)on_off;
  ESP_LOGW(kTag, "CLI command relay_set: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_relay_mask(uint32_t value) {
#if LUCE_HAS_I2C
  const uint8_t mask = static_cast<uint8_t>(value & 0xFF);
  const esp_err_t err = set_relay_mask_safe(mask);
  if (err == ESP_OK) {
    g_relay_mask = mask;
  }
  ESP_LOGI(kTag, "CLI command relay_mask: mask=0x%02X rc=%s", mask, esp_err_to_name(err));
#else
  (void)value;
  ESP_LOGW(kTag, "CLI command relay_mask: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_buttons() {
#if LUCE_HAS_I2C
  if (!g_mcp_available) {
    ESP_LOGW(kTag, "CLI command buttons: MCP unavailable");
    return;
  }
  uint8_t value = 0x00;
  const esp_err_t err = mcp_read_reg(kGpiob, &value);
  ESP_LOGI(kTag, "CLI command buttons: rc=%s gpiob=0x%02X", esp_err_to_name(err), value);
#else
  ESP_LOGW(kTag, "CLI command buttons: unsupported (LUCE_HAS_I2C=0)");
#endif
}

void cli_cmd_lcd_print(char* text) {
#if LUCE_HAS_LCD
  if (!g_lcd_present) {
    ESP_LOGW(kTag, "CLI command lcd_print: LCD not initialized or absent");
    return;
  }
  if (!text || !*text) {
    ESP_LOGW(kTag, "CLI command lcd_print: missing text argument");
    return;
  }
  const bool ok = g_lcd.write_text(0, text);
  ESP_LOGI(kTag, "CLI command lcd_print: rc=%s text='%s'", ok ? "OK" : "ERR", text);
#else
  (void)text;
  ESP_LOGW(kTag, "CLI command lcd_print: unsupported (LUCE_HAS_LCD=0)");
#endif
}

void cli_cmd_wifi_status() {
#if LUCE_HAS_WIFI
  wifi_status_for_cli();
#else
  ESP_LOGW(kTag, "CLI command wifi.status: unsupported (LUCE_HAS_WIFI=0)");
#endif
}

void cli_cmd_wifi_scan() {
#if LUCE_HAS_WIFI
  wifi_scan_for_cli();
#else
  ESP_LOGW(kTag, "CLI command wifi.scan: unsupported (LUCE_HAS_WIFI=0)");
#endif
}

#if LUCE_HAS_NTP
void cli_cmd_time_status() {
  ntp_log_status_line();
  if (!ntp_has_time()) {
    ESP_LOGI(kTag, "[NTP] time not synced: %s", g_ntp_runtime.last_reason);
    return;
  }

  const time_t now = time(nullptr);
  const uint64_t age_s = now >= g_ntp_runtime.last_sync_unix
                             ? static_cast<uint64_t>(now - g_ntp_runtime.last_sync_unix)
                             : 0;
  char utc_now[64] = "n/a";
  ntp_format_utc(utc_now, sizeof(utc_now), now);
  ESP_LOGI(kTag, "CLI command time.status: state=%s sync_state=%s last_sync_unix=%lld last_sync_age_s=%llu utc_now=%s",
           ntp_state_name(g_ntp_runtime.state), ntp_state_name(g_ntp_runtime.state),
           static_cast<long long>(g_ntp_runtime.last_sync_unix), static_cast<unsigned long long>(age_s),
           utc_now);
}
#else
void cli_cmd_time_status() {
  ESP_LOGW(kTag, "CLI command time.status: unsupported (LUCE_HAS_NTP=0)");
}
#endif

#if LUCE_HAS_MDNS
void cli_cmd_mdns_status() {
  mdns_log_status();
}
#else
void cli_cmd_mdns_status() {
  ESP_LOGW(kTag, "CLI command mdns.status: unsupported (LUCE_HAS_MDNS=0)");
}
#endif

void cli_cmd_cli_net_status() {
#if LUCE_HAS_TCP_CLI
  ESP_LOGI(kTag,
           "CLI command cli_net.status: enabled=%d running=%d session=%d authed=%d port=%u ip=%s",
           g_cli_net_config.enabled ? 1 : 0, g_cli_net_runtime.initialized ? 1 : 0,
           g_cli_net_runtime.session_open ? 1 : 0, g_cli_net_runtime.session_authed ? 1 : 0,
           g_cli_net_runtime.active_port,
           g_cli_net_runtime.session_open && g_cli_net_runtime.active_ip[0] != '\0' ? g_cli_net_runtime.active_ip
                                                                             : "(none)");
#else
  ESP_LOGW(kTag, "CLI command cli_net.status: unsupported (LUCE_HAS_TCP_CLI=0)");
#endif
}

bool cli_net_is_readonly_allowed(const char* cmd) {
  if (!cmd) {
    return false;
  }
  return std::strcmp(cmd, "help") == 0 || std::strcmp(cmd, "status") == 0 ||
         std::strcmp(cmd, "wifi.status") == 0 || std::strcmp(cmd, "time.status") == 0 ||
         std::strcmp(cmd, "mdns.status") == 0 || std::strcmp(cmd, "i2c_scan") == 0 ||
         std::strcmp(cmd, "mcp_read") == 0 || std::strcmp(cmd, "buttons") == 0 ||
         std::strcmp(cmd, "sensors") == 0 || std::strcmp(cmd, "cli_net.status") == 0 ||
         std::strcmp(cmd, "http.status") == 0;
}

int execute_cli_command(int argc, char* argv[], bool read_only, const char* source_ip) {
  if (argc <= 0 || !argv || !argv[0]) {
    return 1;
  }

  if (read_only && !cli_net_is_readonly_allowed(argv[0])) {
    ESP_LOGW(kTag, "[CLI_NET] cmd ip=%s cmd=\"%s\" rc=2 denied", source_ip ? source_ip : "local", argv[0]);
    return 2;
  }

  int rc = 0;
  if (source_ip) {
    ESP_LOGI(kTag, "[CLI_NET] cmd ip=%s cmd=\"%s\"", source_ip, argv[0]);
  }

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
    } else if (std::strcmp(argv[1], "gpioa") == 0 || std::strcmp(argv[1], "A") == 0 ||
               std::strcmp(argv[1], "gpiob") == 0 || std::strcmp(argv[1], "B") == 0) {
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
      uint32_t channel = 0;
      uint32_t value = 0;
      char tmp1[32] = {0};
      char tmp2[32] = {0};
      const bool ok1 = parse_u32_with_base(argv[1], 10, &channel, tmp1);
      const bool ok2 = parse_u32_with_base(argv[2], 10, &value, tmp2);
      if (!ok1 || !ok2 || value > 1 || channel > 7) {
        ESP_LOGW(kTag,
                 "CLI command relay_set: parse error or out-of-range (channel=%s value=%s)", tmp1, tmp2);
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
      uint32_t value = 0;
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
      char text[128] = {0};
      std::snprintf(text, sizeof(text), "%s", argv[1]);
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
  } else if (std::strcmp(argv[0], "wifi.status") == 0) {
    cli_cmd_wifi_status();
  } else if (std::strcmp(argv[0], "wifi.scan") == 0) {
    cli_cmd_wifi_scan();
  } else if (std::strcmp(argv[0], "time.status") == 0) {
    cli_cmd_time_status();
  } else if (std::strcmp(argv[0], "mdns.status") == 0) {
    cli_cmd_mdns_status();
  } else if (std::strcmp(argv[0], "mqtt.status") == 0) {
    cli_cmd_mqtt_status();
  } else if (std::strcmp(argv[0], "mqtt.pubtest") == 0) {
    cli_cmd_mqtt_pubtest();
  } else if (std::strcmp(argv[0], "cli_net.status") == 0) {
    cli_cmd_cli_net_status();
  } else if (std::strcmp(argv[0], "http.status") == 0) {
#if LUCE_HAS_HTTP
    cli_cmd_http_status();
#else
    ESP_LOGW(kTag, "CLI command http.status: unsupported (LUCE_HAS_HTTP=0)");
#endif
  } else if (std::strcmp(argv[0], "reboot") == 0) {
    ESP_LOGW(kTag, "CLI command reboot: restarting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
  } else if (std::strcmp(argv[0], "sensors") == 0) {
    ESP_LOGI(kTag, "CLI command sensors: unsupported in this firmware revision");
    rc = 1;
  } else {
    ESP_LOGW(kTag, "CLI unknown command '%s'", argv[0]);
    cli_print_help();
    rc = 1;
  }

  if (source_ip) {
    ESP_LOGI(kTag, "[CLI_NET] cmd ip=%s cmd=\"%s\" rc=%d", source_ip, argv[0], rc);
  }
  return rc;
}

#if LUCE_HAS_TCP_CLI
bool cli_net_load_config(CliNetConfig& config) {
  config = {};
  config.enabled = false;
  config.port = kCliNetDefaultPort;
  config.idle_timeout_s = kCliNetDefaultIdleTimeoutS;
  std::snprintf(config.token, sizeof(config.token), "%s", "luce-cli");

  nvs_handle_t handle = 0;
  if (nvs_open(kCliNetNamespace, NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGI(kTag, "[CLI_NET] key=enabled present=0 value=0");
    ESP_LOGI(kTag, "[CLI_NET] key=port present=0 value=%u", config.port);
    ESP_LOGI(kTag, "[CLI_NET] key=idle_timeout_s present=0 value=%lu",
             static_cast<unsigned long>(config.idle_timeout_s));
    return false;
  }

  bool present = false;
  uint8_t enabled = 0;
  if (wifi_read_u8_key(handle, "enabled", &enabled, &present)) {
    if (enabled == 0 || enabled == 1) {
      config.enabled = (enabled != 0);
    }
  }
  ESP_LOGI(kTag, "[CLI_NET] key=enabled present=%d value=%d", present ? 1 : 0,
           config.enabled ? 1 : 0);

  uint32_t port = config.port;
  if (wifi_read_u32_key(handle, "port", &port, &present) && port > 0 && port <= 65535) {
    config.port = static_cast<uint16_t>(port);
  }
  ESP_LOGI(kTag, "[CLI_NET] key=port present=%d value=%u", present ? 1 : 0,
           static_cast<unsigned>(config.port));

  if (wifi_read_u32_key(handle, "idle_timeout_s", &config.idle_timeout_s, &present)) {
    if (config.idle_timeout_s == 0 || config.idle_timeout_s > 7200) {
      config.idle_timeout_s = kCliNetDefaultIdleTimeoutS;
    }
  }
  ESP_LOGI(kTag, "[CLI_NET] key=idle_timeout_s present=%d value=%lu", present ? 1 : 0,
           static_cast<unsigned long>(config.idle_timeout_s));

  bool token_present = false;
  if (wifi_read_string_key(handle, "token", config.token, sizeof(config.token), &token_present) && token_present) {
    if (config.token[0] == '\0') {
      std::snprintf(config.token, sizeof(config.token), "%s", "luce-cli");
    }
  }
  ESP_LOGI(kTag, "[CLI_NET] key=token present=%d value=%s", token_present ? 1 : 0,
           token_present ? "(set)" : "(default)");

  nvs_close(handle);
  return config.enabled;
}

bool cli_net_send_line(int socket, const char* text) {
  if (socket < 0 || !text) {
    return false;
  }
  std::size_t len = std::strlen(text);
  if (len == 0) {
    return true;
  }
  return send(socket, text, len, 0) >= 0;
}

void cli_net_handle_session(int socket, const char* ip_text, const CliNetConfig& config) {
  char line[kCliNetMaxLine] = {0};
  std::size_t line_len = 0;

  bool authed = false;
  uint32_t failed_auth = 0;
  TickType_t last_activity = xTaskGetTickCount();

  g_cli_net_runtime.session_open = true;
  g_cli_net_runtime.session_authed = false;
  g_cli_net_runtime.active_ip[0] = '\0';
  if (ip_text) {
    std::snprintf(g_cli_net_runtime.active_ip, sizeof(g_cli_net_runtime.active_ip), "%s", ip_text);
  }

  cli_net_send_line(socket, "Welcome to LUCE CLI (TCP). AUTH <token> required\r\n");
  if (config.idle_timeout_s > 0) {
    char timeout_msg[96] = {0};
    std::snprintf(timeout_msg, sizeof(timeout_msg), "Idle timeout: %lu seconds\r\n",
                 static_cast<unsigned long>(config.idle_timeout_s));
    cli_net_send_line(socket, timeout_msg);
  }

  while (true) {
    if ((xTaskGetTickCount() - last_activity) >= pdMS_TO_TICKS(config.idle_timeout_s * 1000U)) {
      cli_net_send_line(socket, "session timeout\r\n");
      ESP_LOGW(kTag, "[CLI_NET] session timeout ip=%s", g_cli_net_runtime.active_ip);
      break;
    }

    char rx_byte;
    const int got = recv(socket, &rx_byte, 1, 0);
    if (got <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    last_activity = xTaskGetTickCount();
    g_cli_net_runtime.last_activity_tick = last_activity;

    if (rx_byte == '\r') {
      continue;
    }
    if (rx_byte == '\n') {
      if (line_len == 0) {
        continue;
      }
      line[line_len] = '\0';

      char command_line[kCliNetMaxLine];
      std::memcpy(command_line, line, sizeof(command_line));
      line_len = 0;
      cli_trim(command_line);

      char* argv[kCliNetMaxArgs] = {nullptr};
      const std::size_t argc = tokenize_cli_line(command_line, argv, kCliNetMaxArgs);
      if (argc == 0) {
        continue;
      }

      log_cli_arguments(argv[0], static_cast<int>(argc), argv);
      if (!authed) {
        if (std::strcmp(argv[0], "AUTH") == 0 && argc == 2 &&
            std::strcmp(argv[1], config.token) == 0) {
          authed = true;
          g_cli_net_runtime.session_authed = true;
          failed_auth = 0;
          ESP_LOGW(kTag, "[CLI_NET] auth ok ip=%s", g_cli_net_runtime.active_ip);
          cli_net_send_line(socket, "auth ok\r\n");
          cli_net_send_line(socket,
                           "Type help/status/wifi.status/time.status/mdns.status/i2c_scan/mcp_read/buttons/cli_net.status\r\n");
          cli_net_send_line(socket, "+OK\r\n");
          continue;
        }
        ++failed_auth;
        g_cli_net_runtime.auth_failures = failed_auth;
        ESP_LOGW(kTag, "[CLI_NET] auth fail ip=%s", g_cli_net_runtime.active_ip);
        cli_net_send_line(socket, "auth fail\r\n");
        if (failed_auth >= kCliNetDefaultMaxAuthFails) {
          cli_net_send_line(socket, "session aborted\r\n");
          break;
        }
        continue;
      }

      const int rc = execute_cli_command(static_cast<int>(argc), argv, true, g_cli_net_runtime.active_ip);
      char response[kCliNetResponseBuffer] = {0};
      std::snprintf(response, sizeof(response), "cmd=%s rc=%d\r\n", argv[0], rc);
      cli_net_send_line(socket, response);
      if (std::strcmp(argv[0], "reboot") == 0 && rc == 0) {
        cli_net_send_line(socket, "rebooting\r\n");
      }
      continue;
    }

    if (line_len < sizeof(line) - 1) {
      line[line_len++] = rx_byte;
    } else {
      line_len = 0;
      std::memset(line, 0, sizeof(line));
      cli_net_send_line(socket, "line too long\r\n");
    }
  }

  g_cli_net_runtime.session_open = false;
  g_cli_net_runtime.session_authed = false;
  g_cli_net_runtime.active_fd = -1;
  g_cli_net_runtime.active_ip[0] = '\0';
}

void cli_net_task(void*) {
  CliNetConfig config;
  const bool enabled = cli_net_load_config(config);
  g_cli_net_runtime.initialized = true;
  g_cli_net_runtime.active_port = config.port;
  g_cli_net_config = config;

  if (!enabled) {
    ESP_LOGI(kTag, "[CLI_NET] enabled=%d", 0);
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  if (config.idle_timeout_s == 0) {
    config.idle_timeout_s = kCliNetDefaultIdleTimeoutS;
  }

  const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_fd < 0) {
    ESP_LOGW(kTag, "[CLI_NET] failed to create listener: %s", strerror(errno));
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  int yes = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(config.port);
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  const int bind_rc = bind(listen_fd, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr));
  if (bind_rc != 0) {
    ESP_LOGW(kTag, "[CLI_NET] bind failed: port=%u rc=%d (%s)", config.port, bind_rc, strerror(errno));
    close(listen_fd);
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  if (listen(listen_fd, 1) != 0) {
    ESP_LOGW(kTag, "[CLI_NET] listen failed: %s", strerror(errno));
    close(listen_fd);
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  ESP_LOGI(kTag, "[CLI_NET] enabled=%d port=%u", 1, config.port);
  log_heap_integrity("cli_net_task");

  while (true) {
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    const int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    const char* remote = inet_ntoa(client_addr.sin_addr);
    std::strncpy(g_cli_net_runtime.active_ip, remote ? remote : "unknown",
                 sizeof(g_cli_net_runtime.active_ip) - 1);
    g_cli_net_runtime.active_fd = client_fd;
    ESP_LOGI(kTag, "[CLI_NET] conn ip=%s", g_cli_net_runtime.active_ip);

    g_cli_net_runtime.auth_failures = 0;
    cli_net_handle_session(client_fd, g_cli_net_runtime.active_ip, config);

    g_cli_net_runtime.active_fd = -1;
    close(client_fd);
    ESP_LOGI(kTag, "[CLI_NET] conn closed ip=%s", g_cli_net_runtime.active_ip);
  }
}

void cli_net_startup() {
  if (xTaskCreate(cli_net_task, "cli_net", 4096, nullptr, 2, &g_cli_net_task) != pdPASS) {
    ESP_LOGW(kTag, "[CLI_NET] startup: task create failed");
  }
}
#endif  // LUCE_HAS_TCP_CLI

void cli_task(void*) {
  uart_config_t uart_cfg{};
  uart_cfg.baud_rate = 115200;
  uart_cfg.data_bits = UART_DATA_8_BITS;
  uart_cfg.parity = UART_PARITY_DISABLE;
  uart_cfg.stop_bits = UART_STOP_BITS_1;
  uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_cfg.source_clk = UART_SCLK_APB;
  esp_err_t err = uart_param_config(UART_NUM_0, &uart_cfg);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "CLI uart_param_config failed: %s", esp_err_to_name(err));
  }
  err = uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "CLI uart_set_pin failed: %s", esp_err_to_name(err));
  }
  err = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "CLI uart_driver_install failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(kTag, "CLI listening on UART0 at 115200. Type 'help' for commands.");
  }
  log_stage4_watermarks("cli_start");
  log_heap_integrity("cli_start");

  char line_buffer[128] = {0};
  size_t line_len = 0;
  char ch;
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

      char command_buffer[128];
      std::memcpy(command_buffer, line_buffer, sizeof(command_buffer));
      cli_trim(command_buffer);
      line_len = 0;

      char* argv[kCliNetMaxArgs] = {nullptr};
      const std::size_t argc = tokenize_cli_line(command_buffer, argv, kCliNetMaxArgs);
      if (argc == 0) {
        continue;
      }

      log_cli_arguments(argv[0], static_cast<int>(argc), argv);
      log_heap_integrity("cli_pre_cmd");
      log_stage4_watermarks("cli_pre_cmd");
      const int rc = execute_cli_command(static_cast<int>(argc), argv, false, nullptr);
      (void)rc;
      std::memset(line_buffer, 0, sizeof(line_buffer));
      log_heap_integrity("cli_post_cmd");
      log_stage4_watermarks("cli_post_cmd");
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
#if LUCE_STAGE4_CLI
  if (xTaskCreate(cli_task, "cli", kCliTaskStackWords, nullptr, 2, &g_cli_task) != pdPASS) {
    ESP_LOGW(kTag, "CLI task create failed");
  } else {
    log_stage4_watermarks("cli_startup");
  }
#else
  ESP_LOGW(kTag, "CLI task creation disabled by LUCE_STAGE4_CLI=%d", LUCE_STAGE4_CLI);
#endif
}

#if LUCE_HAS_TCP_CLI
void tcp_cli_startup() {
  cli_net_startup();
}
#endif

#endif  // LUCE_HAS_CLI

#if LUCE_HAS_CLI
void diagnostics_task(void*) {
  run_stage2_diagnostics();
}
#endif  // LUCE_HAS_CLI

void blink_alive_task(void*) {
  blink_alive();
}

#endif  // LUCE_HAS_I2C

void blink_alive() {
  const gpio_num_t leds[] = {GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_27};

  for (gpio_num_t pin : leds) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
  }

  while (true) {
    for (size_t idx = 0; idx < 3; ++idx) {
      for (size_t j = 0; j < 3; ++j) {
        gpio_set_level(leds[j], j == idx ? 1 : 0);
      }
      vTaskDelay(pdMS_TO_TICKS(160));
    }
    for (size_t j = 0; j < 3; ++j) {
      gpio_set_level(leds[j], 0);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

extern "C" void app_main(void) {
  log_startup_banner();

  print_chip_info();
  print_heap_stats();
  print_app_info();
  print_partition_summary();

  ESP_LOGI(kTag, "Feature flags: NVS=%d I2C=%d LCD=%d CLI=%d WIFI=%d NTP=%d mDNS=%d MQTT=%d HTTP=%d",
           LUCE_HAS_NVS, LUCE_HAS_I2C, LUCE_HAS_LCD, LUCE_HAS_CLI, LUCE_HAS_WIFI, LUCE_HAS_NTP,
           LUCE_HAS_MDNS, LUCE_HAS_MQTT, LUCE_HAS_HTTP);

#if LUCE_HAS_NVS
  update_boot_state_record();
#endif

#if LUCE_HAS_I2C
  xTaskCreate(blink_alive_task, "blink", kBlinkTaskStackWords, nullptr, 1, nullptr);
#if LUCE_HAS_CLI
#if LUCE_STAGE4_DIAG
  if (xTaskCreate(diagnostics_task, "diag", kDiagTaskStackWords, nullptr, 1, &g_diag_task) != pdPASS) {
    ESP_LOGW(kTag, "Stage4 diagnostic task create failed");
  }
#else
  ESP_LOGW(kTag, "Diagnostics task disabled by LUCE_STAGE4_DIAG=%d", LUCE_STAGE4_DIAG);
#endif
#if LUCE_STAGE4_CLI
  cli_startup();
#else
  ESP_LOGW(kTag, "CLI startup disabled by LUCE_STAGE4_CLI=%d", LUCE_STAGE4_CLI);
#endif
#if LUCE_HAS_WIFI
  wifi_startup();
#if LUCE_HAS_HTTP
  http_startup();
#endif
#if LUCE_HAS_MDNS
  mdns_startup();
#endif
#if LUCE_HAS_MQTT
  mqtt_startup();
#endif
#if LUCE_HAS_NTP
  ntp_startup();
#endif
#if LUCE_HAS_TCP_CLI
  tcp_cli_startup();
#endif
#endif
  for (;;) {
    log_stage4_watermarks("stage4_main_wait");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#else
  run_stage2_diagnostics();
  ESP_LOGW(kTag, "Stage2 diagnostics loop exited; staying in blink-only fallback");
#endif
#endif

  blink_alive();
}
