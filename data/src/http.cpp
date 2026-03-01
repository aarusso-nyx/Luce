// http.cpp: ESP-IDF HTTP server with WebSocket, captive portal, cJSON
#include "http.h"
#include <esp_http_server.h>
#include <cJSON.h>
#include <sys/stat.h>
#include <stdio.h>
#include "websocket_server.h"
#include "config.h"
#include "settings.h"
#include "relays.h"
#include "leds.h"
#include "mqtt.h"
#include "probes.h"

static httpd_handle_t server = nullptr;

// WebSocket callbacks
static void ws_open(int sockfd) {
    LOGINFO("WS","Open","Client %d connected", sockfd);
}
static void ws_close(int sockfd) {
    LOGINFO("WS","Close","Client %d disconnected", sockfd);
}
static void ws_recv(int sockfd, char* msg, size_t len) {
    // ignore incoming
}

// API: GET /api/info
static esp_err_t info_get_handler(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", Settings.getName());
    cJSON_AddStringToObject(root, "version", FW_VERSION);
    cJSON_AddNumberToObject(root, "uptimeMs", esp_timer_get_time()/1000);
    cJSON_AddNumberToObject(root, "relays", Settings.getState());
    cJSON_AddNumberToObject(root, "nightMask", Settings.getNight());
    cJSON_AddBoolToObject(root,   "day", relaysIsDay());
    cJSON_AddNumberToObject(root, "threshold", Settings.getLight());
    cJSON_AddNumberToObject(root, "light", Settings.getSensor());
    cJSON_AddNumberToObject(root, "temperature", Settings.getTemperature());
    cJSON_AddNumberToObject(root, "humidity", Settings.getHumidity());
    // network
    cJSON* net = cJSON_AddObjectToObject(root, "network");
    esp_netif_ip_info_t ipinfo;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ipinfo);
    char ipstr[16]; snprintf(ipstr,16,"%d.%d.%d.%d", IP2STR(&ipinfo.ip.u_addr.ip4));
    cJSON_AddStringToObject(net, "ip", ipstr);
    cJSON_AddBoolToObject(net, "mqttConnected", mqttIsConnected());

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

// API: GET /api/version
static esp_err_t version_get_handler(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", FW_VERSION);
    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, strlen(out));
    free(out);
    return ESP_OK;
}

// Static files & captive portal redirect
static esp_err_t static_get_handler(httpd_req_t* req) {
    const char* uri = req->uri;
    char relpath[64];
    if (strcmp(uri, "/") == 0) strcpy(relpath, "/index.html");
    else strncpy(relpath, uri, sizeof(relpath)-1);
    // Construct full filesystem path under LittleFS mount
    char fullpath[128];
    snprintf(fullpath, sizeof(fullpath), "/littlefs%s", relpath);
    struct stat st;
    if (stat(fullpath, &st) == 0) {
        FILE* f = fopen(fullpath, "r");
        if (!f) {
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, get_mime(relpath));
        char buf[1024];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
            httpd_resp_send_chunk(req, buf, r);
        }
        fclose(f);
        httpd_resp_send_chunk(req, NULL, 0);
    } else {
        // captive portal redirect
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t httpInit() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = config::HTTP_PORT;
    if (httpd_start(&server, &config) != ESP_OK) {
        LOGERR("HTTP","Init","server start failed");
        return ESP_FAIL;
    }
    // WebSocket via esp-idf-lib
    ws_server_config_t ws_cfg = WS_SERVER_CONFIG_DEFAULT();
    ws_cfg.uri = "/ws";
    ws_cfg.open_cb = ws_open;
    ws_cfg.close_cb= ws_close;
    ws_cfg.recv_cb = ws_recv;
    ws_server_start(&ws_cfg);

    // Register API handlers
    httpd_register_uri_handler(server, &(httpd_uri_t){"/api/info",    HTTP_GET, info_get_handler,    NULL});
    httpd_register_uri_handler(server, &(httpd_uri_t){"/api/version", HTTP_GET, version_get_handler, NULL});
    // Static and captive portal for all others
    httpd_register_uri_handler(server, &(httpd_uri_t){"/*", HTTP_GET, static_get_handler, NULL});
    LOGINFO("HTTP","Init","started on port %u", (unsigned)config::HTTP_PORT);
    return ESP_OK;
}

void httpLoop() {
    // no-op
}
// helper for MIME types
static const char* get_mime(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    if (strcmp(ext, ".html")==0) return "text/html";
    if (strcmp(ext, ".css")==0) return "text/css";
    if (strcmp(ext, ".js")==0) return "application/javascript";
    if (strcmp(ext, ".ico")==0) return "image/x-icon";
    return "application/octet-stream";
}