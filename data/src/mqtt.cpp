// Standard
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <type_traits>
#include <vector>

// ESP-IDF MQTT
#include <esp_mqtt_client.h>

// FreeRTOS
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// Project
#include "config.h"
using namespace config;
#include "eventBus.h"
#include "http.h"
#include "leds.h"
#include "logger.h"
#include "mqtt.h"
#include "relays.h"
#include "settings.h"
#include "util.h"
 
// Native ESP-IDF MQTT client handle
static esp_mqtt_client_handle_t mqttClient = nullptr;
// Connection state
static bool mqtt_connected = false;

// Public publish helpers using native ESP-IDF MQTT client
void mqttPublish(const char* subtopic, const char* payload, bool retained) {
  if (!mqttClient || !mqtt_connected) return;
  char topic[128];
  snprintf(topic, sizeof(topic), "%s/%s", Settings.getName(), subtopic);
  esp_mqtt_client_publish(mqttClient, topic, payload,
                          strlen(payload), 0, retained);
}
void mqttPublish(const char* subtopic, int value, bool retained) {
  char buf[32]; snprintf(buf, sizeof(buf), "%d", value);
  mqttPublish(subtopic, buf, retained);
}
void mqttPublish(const char* subtopic, float value, bool retained) {
  char buf[32]; snprintf(buf, sizeof(buf), "%f", value);
  mqttPublish(subtopic, buf, retained);
}
// Return whether MQTT client is currently connected
bool mqttIsConnected() {
  return mqtt_connected;
}

// (Removed MQTTPUB macro; use mqttPublish overloads below for all publishes.)

static char mqttUser[64]     = {0};
static char mqttPass[64]     = {0};
static char mqttHost[128]    = {0};
static uint16_t mqttPort     = 1883;


// Subscription dispatch for MQTT topics
struct MqttHandler {
  const char* key;
  bool prefix;
  std::function<void(const char* arg, const char* msg)> fn;
};
static const std::vector<MqttHandler> mqttHandlers = {
  // Configuration
  {"config/name",       false, [](const char*, const char* msg){ Settings.setName(msg); }},
  {"config/ssid",       false, [](const char*, const char* msg){ Settings.setSSID(msg); }},
  {"config/pass",       false, [](const char*, const char* msg){ Settings.setPass(msg); }},
  {"config/ssid2",      false, [](const char*, const char* msg){ Settings.setSSID2(msg); }},
  {"config/pass2",      false, [](const char*, const char* msg){ Settings.setPass2(msg); }},
  {"config/mqtt",       false, [](const char*, const char* msg){ Settings.setMqtt(msg); }},
  {"config/logConsoleFmt",   false, [](const char*, const char* msg){
      Settings.setLog(SettingsClass::LogTarget::Console, SettingsClass::LogParam::Format, msg);
    }},
  {"config/logFileFmt",      false, [](const char*, const char* msg){
      Settings.setLog(SettingsClass::LogTarget::File,    SettingsClass::LogParam::Format, msg);
    }},
  {"config/logConsoleLevel", false, [](const char*, const char* msg){
      Settings.setLog(SettingsClass::LogTarget::Console, SettingsClass::LogParam::Level,   msg);
    }},
  {"config/logFileLevel",    false, [](const char*, const char* msg){
      Settings.setLog(SettingsClass::LogTarget::File,    SettingsClass::LogParam::Level,   msg);
    }},
  // Relays
  {"relays/state",      false, [](const char*, const char* msg){ relaysSetState(static_cast<uint8_t>(strtoul(msg,nullptr,0))); }},
  {"relays/state/",     true,  [](const char* arg, const char* msg){ int idx=atoi(arg); relaysSetStateIdx(static_cast<uint8_t>(idx), msg[0]=='1'); }},
  {"relays/night",      false, [](const char*, const char* msg){ relaysSetNight(static_cast<uint8_t>(strtoul(msg,nullptr,0))); }},
  {"relays/night/",     true,  [](const char* arg, const char* msg){ int idx=atoi(arg); relaysSetNightIdx(static_cast<uint8_t>(idx), msg[0]=='1'); }},
  // Sensor
  {"sensor/threshold",  false, [](const char*, const char* msg){ Settings.setLight(static_cast<uint16_t>(strtoul(msg,nullptr,0))); }},
  // LEDs
  {"leds/state",        false, [](const char*, const char*){ mqttPublish("leds/state", (int)Leds.getMask()); }},
  {"leds/state/",       true,  [](const char* arg, const char* msg){ int idx=atoi(arg); Leds.set(static_cast<uint8_t>(idx), msg[0]=='1'); mqttPublish(arg, msg[0]=='1'); }}
};

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  payload[length] = '\0';
  const char* msg = reinterpret_cast<const char*>(payload);
  // Strip device prefix
  size_t pre = strlen(Settings.getName());
  if (strncmp(topic, Settings.getName(), pre) != 0 || topic[pre] != '/') return;
  const char* sub = topic + pre + 1;
  // Dispatch
  for (const auto& h : mqttHandlers) {
    if (h.prefix) {
      size_t k = strlen(h.key);
      if (strncmp(sub, h.key, k) == 0) {
        h.fn(sub + k, msg);
        return;
      }
    } else {
      if (strcmp(sub, h.key) == 0) {
        h.fn(nullptr, msg);
        return;
      }
    }
  }
  // Unrecognized: ignore
}

// Publish sensor data dynamically over MQTT
void publishSensorData(const SensorEventData& s) {
  if (!client.connected()) return;
  mqttPublish("sensor/lighting",  s.ldr);
  mqttPublish("sensor/voltage",   s.voltage);
  mqttPublish("sensor/temperature", s.temperature);
  mqttPublish("sensor/humidity",   s.humidity);
}


// Parse and configure MQTT broker from URI or mDNS
static bool parseBrokerUri(const char* brokerUri) {
  LOGINFO("MQTT","ParseURI","%s", brokerUri);
  if (strncmp(brokerUri, "mqtt://", 7) == 0) {
    const char* p = brokerUri + 7;
    // Credentials user:pass@?
    if (const char* at = strchr(p, '@')) {
      char cred[64]; size_t len = at - p;
      memcpy(cred, p, len); cred[len] = '\0';
      if (char* c = strchr(cred, ':')) { *c = '\0'; strncpy(mqttUser, cred, sizeof(mqttUser)-1); strncpy(mqttPass, c+1, sizeof(mqttPass)-1); }
      else strncpy(mqttHost, cred, sizeof(mqttHost)-1);
      p = at + 1;
    }
    // Host:port?
    if (const char* c = strrchr(p, ':')) {
      size_t hlen = c - p; hlen = std::min(hlen, sizeof(mqttHost)-1);
      memcpy(mqttHost, p, hlen); mqttHost[hlen] = '\0';
      mqttPort = static_cast<uint16_t>(atoi(c+1));
    } else {
      strncpy(mqttHost, p, sizeof(mqttHost)-1);
      mqttPort = 1883;
    }
    return true;
  }
  if (strncasecmp(brokerUri, "mdns", 4) == 0) {
    LOGERR("MQTT","ParseURI","mDNS discovery not supported");
    return false;
  }
  if (!Settings.hasMqtt()) return false;
  strncpy(mqttHost, brokerUri, sizeof(mqttHost)-1);
  mqttPort = 1883;
  return true;
}

// Initialize MQTT with configured broker URI; return false to abort task
bool mqttInit() {
  LOGINFO("MQTT","Init","Initializing MQTT client");
  
  const char* brokerUri = Settings.getMqtt();
  if (!parseBrokerUri(brokerUri)) {
    LOGERR("MQTT","ParseURI","Failed to parse broker '%s'", brokerUri);
    return false;
  }
  // Configure native ESP-IDF MQTT client
  esp_mqtt_client_config_t cfg = {};
  cfg.host = mqttHost;
  cfg.port = mqttPort;
  cfg.client_id = Settings.getName();
  if (mqttUser[0]) {
    cfg.username = mqttUser;
    cfg.password = mqttPass;
  }
  mqttClient = esp_mqtt_client_init(&cfg);
  if (!mqttClient) {
    LOGERR("MQTT","Init","Failed to init MQTT client");
    return false;
  }
  // Register event handler
  esp_mqtt_client_register_event(mqttClient, ESP_EVENT_ANY_ID,
      [](esp_mqtt_event_handle_t event) -> esp_err_t {
    switch (event->event_id) {
      case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        LOGINFO("MQTT","Connected","%s:%u", mqttHost, (unsigned)mqttPort);
        // Subscribe to topics
        {
          char topic[128];
          snprintf(topic, sizeof(topic), "%s/config/#", Settings.getName());
          esp_mqtt_client_subscribe(mqttClient, topic, 0);
          snprintf(topic, sizeof(topic), "%s/relays/#", Settings.getName());
          esp_mqtt_client_subscribe(mqttClient, topic, 0);
          snprintf(topic, sizeof(topic), "%s/sensor/#", Settings.getName());
          esp_mqtt_client_subscribe(mqttClient, topic, 0);
          snprintf(topic, sizeof(topic), "%s/leds/#", Settings.getName());
          esp_mqtt_client_subscribe(mqttClient, topic, 0);
        }
        break;
      case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        LOGWARN("MQTT","Disconnected","%s:%u", mqttHost, (unsigned)mqttPort);
        break;
      case MQTT_EVENT_DATA:
        // Topic and payload as strings
        {
          std::string topic(event->topic, event->topic_len);
          std::string msg(event->data, event->data_len);
          const char* name = Settings.getName();
          size_t pre = strlen(name);
          if (topic.size() > pre+1 && topic.compare(0, pre, name) == 0 && topic[pre] == '/') {
            const char* sub = topic.c_str() + pre + 1;
            for (const auto& h : mqttHandlers) {
              size_t k = strlen(h.key);
              if (h.prefix) {
                if (strncmp(sub, h.key, k) == 0) {
                  h.fn(sub + k, msg.c_str());
                  break;
                }
              } else {
                if (strcmp(sub, h.key) == 0) {
                  h.fn(nullptr, msg.c_str());
                  break;
                }
              }
            }
          }
        }
        break;
      default:
        break;
    }
    return ESP_OK;
  }, nullptr);
  // Start MQTT client
  if (esp_mqtt_client_start(mqttClient) != ESP_OK) {
    LOGERR("MQTT","Start","Failed to start MQTT client");
    return false;
  }
  return true;


// Maintain MQTT connection; return false to abort task
bool mqttLoop() {
  // Update LED1 to reflect MQTT connection state
  Leds.set(1, mqtt_connected);
  // Dispatch all queued events via EventBus
  EventBus::dispatch();
  return true;
}
// Publish a log message over MQTT under '<deviceName>/logs'
// Publish a log message over MQTT under '<deviceName>/logs'
void publishLog(const char* message, bool retained) {
  if (!mqttClient || !mqtt_connected) return;
  mqttPublish("logs", message, retained);
}