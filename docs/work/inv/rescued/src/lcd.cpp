// lcd.cpp: Implementation of LcdManager using 20x4 I2C LCD
// lcd.cpp: Implementation of LcdManager using esp-idf-lib PCF8574+HD44780 drivers
#include <cstdarg>
#include <cstdio>
#include <inttypes.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>

// LCD manager implementation
#include "lcd.h"
// #include "mqtt.h"
#include "probes.h"
#include "util.h"
using namespace config;

// Module-scope state for LCD
// Timer intervals
#define REFRESH_INTERVAL_MS   config::DISPLAY_UPDATE_MS
#define INACTIVITY_TIMEOUT_MS config::DISPLAY_SWAP_MS

static i2c_dev_t* pcf        = nullptr;
static hd44780_t* lcd_dev    = nullptr;
static bool     lcd_enabled  = false;
// Backlight state: true = on, false = off
static bool     backlight_on = false;
// Page enumeration
static enum Page_e { PAGE_SUMMARY = 0,
                     PAGE_SENSORS,
                     PAGE_NETWORK,
                     PAGE_RELAYS,
                     PAGE_LOGS,
                     PAGE_SYSTEM,
                     PAGE_SYSTEM2,
                     PAGE_COUNT } current_page = PAGE_SUMMARY;
// Timing and interaction
static uint32_t lastRefreshMs     = 0;
static uint32_t lastInteractionMs = 0;
static size_t   bootCount         = 0;
static std::string bootLines[config::LCD_ROWS];
// FreeRTOS timers
static TimerHandle_t refreshTimer    = NULL;
static TimerHandle_t inactivityTimer = NULL;

//-------------------------------------------------------------------------
// Free-function LCD API (replaces LCDManager class)
//-------------------------------------------------------------------------
// Forward drawPage
static void drawPage(Page_e p);

// Initialize LCD; return false on error
bool lcdInit() {
  i2cdev_init();
  esp_err_t err = pcf8574_init_desc(&pcf, I2C_NUM_0,
                    (gpio_num_t)config::I2C_SDA,
                    (gpio_num_t)config::I2C_SCL,
                    config::LCD_I2C_ADDR);
  if (err != ESP_OK) {
    LOGERR("LCD","Init","pcf8574 init failed: %s", esp_err_to_name(err));
    return false;
  }
  lcd_dev = hd44780_create(pcf, config::LCD_COLS, config::LCD_ROWS);
  if (!lcd_dev) {
    LOGERR("LCD","Init","hd44780 create failed");
    pcf8574_delete_desc(pcf);
    return false;
  }
  err = hd44780_init(lcd_dev);
  if (err != ESP_OK) {
    LOGERR("LCD","Init","hd44780 init failed: %s", esp_err_to_name(err));
    hd44780_delete(lcd_dev);
    pcf8574_delete_desc(pcf);
    return false;
  }
  hd44780_backlight_on(lcd_dev);
  hd44780_clear(lcd_dev);
  lcd_enabled = true;
  backlight_on = true;
  current_page = PAGE_SUMMARY;
  lastRefreshMs = millis();
  lastInteractionMs = lastRefreshMs;
  drawPage(current_page);
  // Setup timers
  refreshTimer = xTimerCreate("lcd_refr",
                    pdMS_TO_TICKS(REFRESH_INTERVAL_MS), pdTRUE,
                    NULL,
                    [](TimerHandle_t){ drawPage(current_page); });
  if (refreshTimer) xTimerStart(refreshTimer, 0);
  inactivityTimer = xTimerCreate("lcd_off",
                    pdMS_TO_TICKS(INACTIVITY_TIMEOUT_MS), pdFALSE,
                    NULL,
                    [](TimerHandle_t){
                      backlight_on = false;
                      hd44780_backlight_off(lcd_dev);
                      hd44780_clear(lcd_dev);
                    });
  if (inactivityTimer) xTimerStart(inactivityTimer, 0);
  return true;
}

// LCD main loop; return false to terminate task on error
bool lcdLoop() {
  return lcd_enabled;
}

// Show boot message (scrolling)
void lcdShowBoot(const char* fmt, ...) {
  if (!lcd_enabled) return;
  uint64_t now = millis();
  if (!backlight_on) {
    backlight_on = true;
    hd44780_backlight_on(lcd_dev);
  }
  lastInteractionMs = now;
  char buf[config::LCD_COLS+1];
  va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (bootCount < config::LCD_ROWS) ++bootCount;
  for (size_t i = 0; i+1 < bootCount; ++i) bootLines[i] = bootLines[i+1];
  bootLines[bootCount-1] = std::string(buf);
  hd44780_clear(lcd_dev);
  for (size_t r = 0; r < bootCount; ++r) {
    hd44780_set_cursor(lcd_dev, 0, r);
    hd44780_puts(lcd_dev, bootLines[r].c_str());
  }
}

// Handle navigation key
void lcdHandleKey(LcdKey key) {
  if (!lcd_enabled || key == LCD_KEY_NONE) return;
  uint64_t now = millis();
  if (!backlight_on) {
    backlight_on = true;
    hd44780_backlight_on(lcd_dev);
    current_page = PAGE_SUMMARY;
    lastInteractionMs = now;
    drawPage(current_page);
    return;
  }
  lastInteractionMs = now;
  if (inactivityTimer) xTimerReset(inactivityTimer, 0);
  switch (key) {
    case LCD_KEY_UP:    current_page = Page_e((current_page+PAGE_COUNT-1)%PAGE_COUNT); break;
    case LCD_KEY_DOWN:  current_page = Page_e((current_page+1)%PAGE_COUNT); break;
    case LCD_KEY_1: case LCD_KEY_2: case LCD_KEY_3:
    case LCD_KEY_4: case LCD_KEY_5: case LCD_KEY_6:
    case LCD_KEY_7: {
      int idx = key - LCD_KEY_1;
      if (idx >= 0 && idx < PAGE_COUNT) current_page = Page_e(idx);
    } break;
    case LCD_KEY_EXIT:  current_page = PAGE_SUMMARY; break;
    default: break;
  }
  drawPage(current_page);
}

// Write up to 4 text rows
void lcdWritePage(const char* row0, const char* row1,
                  const char* row2, const char* row3) {
  char buf[config::LCD_COLS+1];
  hd44780_clear(lcd_dev);
  const char* rows[config::LCD_ROWS] = {row0, row1, row2, row3};
  for (int r = 0; r < config::LCD_ROWS; ++r) {
    hd44780_set_cursor(lcd_dev, 0, r);
    const char* txt = rows[r] ? rows[r] : "";
    snprintf(buf, sizeof(buf), "%-*.*s", config::LCD_COLS, config::LCD_COLS, txt);
    hd44780_puts(lcd_dev, buf);
  }
}

// Draw page
static void drawPage(Page_e p) {
  hd44780_clear(lcd_dev);
  char buf[32];
  switch (p) {
    case PAGE_SUMMARY: {
      hd44780_set_cursor(lcd_dev, 0, 0);
      hd44780_puts(lcd_dev, Settings.getName());
      auto u = probes::getUptimeInfo();
      strftime(buf, sizeof(buf), "%H:%M:%S", &u.timestamp);
      hd44780_set_cursor(lcd_dev, 0, 1);
      hd44780_puts(lcd_dev, buf);
      auto r = probes::getRelayInfo();
      hd44780_set_cursor(lcd_dev, 0, 2);
      snprintf(buf, sizeof(buf), "R:0x%02X N:0x%02X", r.stateMask, r.nightMask);
      hd44780_puts(lcd_dev, buf);
      auto s = probes::getSensorInfo();
      hd44780_set_cursor(lcd_dev, 0, 3);
      snprintf(buf, sizeof(buf), "T:%.1fC H:%.1f%%", s.temperature, s.humidity);
      hd44780_puts(lcd_dev, buf);
      hd44780_puts(lcd_dev, (s.light > s.threshold) ? "☀" : "☾");
    } break;
    default: {
      // Other pages not implemented
      lcdWritePage("Page", "Not", "Implemented", nullptr);
    } break;
  }
}

// Legacy LCDManager methods disabled after conversion
#if 0
void LcdManager::begin() {
  // Initialize I2C device layer
  i2cdev_init();
  // Initialize PCF8574 expander
  esp_err_t err = pcf8574_init_desc(&pcf_, I2C_NUM_0, (gpio_num_t)I2C_SDA, (gpio_num_t)I2C_SCL, LCD_I2C_ADDR);
  if (err != ESP_OK) {
    LOGERR("LCD","Init","pcf8574 init failed: %s", esp_err_to_name(err));
    enabled_ = false;
    vTaskDelete(NULL);
    return;
  }
  // Create HD44780 LCD interface
  lcd_ = hd44780_create(pcf_, LCD_COLS, LCD_ROWS);
  if (!lcd_) {
    LOGERR("LCD","Init","hd44780 create failed");
    pcf8574_delete_desc(pcf_);
    enabled_ = false;
    vTaskDelete(NULL);
    return;
  }
  // Initialize LCD controller
  err = hd44780_init(lcd_);
  if (err != ESP_OK) {
    LOGERR("LCD","Init","hd44780 init failed: %s", esp_err_to_name(err));
    hd44780_delete(lcd_);
    pcf8574_delete_desc(pcf_);
    enabled_ = false;
    vTaskDelete(NULL);
    return;
  }
  // Turn on backlight and clear
  hd44780_backlight_on(lcd_);
  hd44780_clear(lcd_);
  // Start in ON state with summary page shown
  enabled_ = true;
  state_ = ON;
  page_ = PAGE_SUMMARY;
  lastRefreshMs_ = millis();
  lastInteractionMs_ = lastRefreshMs_;
  drawPage(page_);
  // Set up FreeRTOS timers for refresh and inactivity
  refreshTimer_ = xTimerCreate("lcd_refr",
                              pdMS_TO_TICKS(REFRESH_INTERVAL_MS),
                              pdTRUE,
                              this,
                              [](TimerHandle_t xTimer){
                                auto self = static_cast<LcdManager*>(pvTimerGetTimerID(xTimer));
                                self->refresh();
                              });
  if (refreshTimer_) xTimerStart(refreshTimer_, 0);
  inactivityTimer_ = xTimerCreate("lcd_off",
                                 pdMS_TO_TICKS(INACTIVITY_TIMEOUT_MS),
                                 pdFALSE,
                                 this,
                                 [](TimerHandle_t xTimer){
                                   auto self = static_cast<LcdManager*>(pvTimerGetTimerID(xTimer));
                                   // Turn off display
                                   self->state_ = OFF;
                                   hd44780_backlight_off(self->lcd_);
                                   hd44780_clear(self->lcd_);
                                 });
  if (inactivityTimer_) xTimerStart(inactivityTimer_, 0);
#endif // LcdManager legacy code
}

void LcdManager::showBoot(const char* fmt, ...) {
  if (!enabled_) return;
  unsigned long now = millis();
  // Wake display if off
  if (state_ == OFF) {
    state_ = ON;
    hd44780_backlight_on(lcd_);
  }
  lastInteractionMs_ = now;
  // Format the message line
  char line[LCD_COLS + 1];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  // Scroll buffer up
  if (bootCount_ < LCD_ROWS) bootCount_++;
  for (uint8_t i = 0; i + 1 < bootCount_; ++i) {
    bootLines_[i] = bootLines_[i + 1];
  }
  bootLines_[bootCount_ - 1] = std::string(line);
  // Display buffered lines
  hd44780_clear(lcd_);
  for (uint8_t row = 0; row < bootCount_; ++row) {
    hd44780_set_cursor(lcd_, 0, row);
    hd44780_puts(lcd_, bootLines_[row].c_str());
  }
}

// Helper: write up to 4 lines on LCD with fixed width
void LcdManager::writePage(const char* row0,
                           const char* row1,
                           const char* row2,
                           const char* row3) {
  static char buf[80];
  const char* rows[config::LCD_ROWS] = { row0, row1, row2, row3 };
  hd44780_clear(lcd_);
  for (int r = 0; r < config::LCD_ROWS; ++r) {
    hd44780_set_cursor(lcd_, 0, r);
    const char* text = rows[r] ? rows[r] : "";
    // Trim or pad to LCD_COLS
    snprintf(buf, sizeof(buf), "%-*.*s", LCD_COLS, LCD_COLS, text);
    hd44780_puts(lcd_, buf);
  }
}

// No longer used: refresh and inactivity handled by timers
void LcdManager::loop() {}

void LcdManager::handleKey(LcdKey key) {
  if (key == LCD_KEY_NONE) return;
  unsigned long now = millis();
  if (state_ == OFF) {
    // wake on any key
    state_ = ON;
    hd44780_backlight_on(lcd_);
    page_ = PAGE_SUMMARY;
    lastRefreshMs_ = now;
    lastInteractionMs_ = now;
    drawPage(page_);
    return;
  }
  // state_ == ON: reset inactivity timer
  lastInteractionMs_ = now;
  if (inactivityTimer_) xTimerReset(inactivityTimer_, 0);
  switch (key) {
    case LCD_KEY_UP:
      page_ = (Page)((page_ + PAGE_COUNT - 1) % PAGE_COUNT);
      break;
    case LCD_KEY_DOWN:
      page_ = (Page)((page_ + 1) % PAGE_COUNT);
      break;
    case LCD_KEY_1: case LCD_KEY_2: case LCD_KEY_3:
    case LCD_KEY_4: case LCD_KEY_5: case LCD_KEY_6:
    case LCD_KEY_7: {
      int idx = key - LCD_KEY_1;
      if (idx < PAGE_COUNT) page_ = (Page)idx;
    } break;
    case LCD_KEY_EXIT:
      page_ = PAGE_SUMMARY;
      break;
    default:
      break;
  }
  drawPage(page_);
}

void LcdManager::refresh() {
  drawPage(page_);
}

void LcdManager::drawPage(Page p) {
  hd44780_clear(lcd_);
  switch (p) {
    case PAGE_SUMMARY: {
      // Device name
      hd44780_set_cursor(lcd_, 0, 0);
      hd44780_puts(lcd_, Settings.getName());
      // Time of day
      auto u = probes::getUptimeInfo();
      char buf[20];
      strftime(buf, sizeof(buf), "%H:%M:%S", &u.timestamp);
      hd44780_set_cursor(lcd_, 0, 1);
      hd44780_puts(lcd_, buf);
      // Relay masks
      auto r = probes::getRelayInfo();
      hd44780_set_cursor(lcd_, 0, 2);
      { char buf[21]; snprintf(buf, sizeof(buf), "R:0x%02X N:0x%02X", r.stateMask, r.nightMask);
        hd44780_puts(lcd_, buf);
      }
      // Temp/Humidity + day/night icon
      auto s = probes::getSensorInfo();
      hd44780_set_cursor(lcd_, 0, 3);
      { char buf[21]; snprintf(buf, sizeof(buf), "T:%.1fC H:%.1f%%", s.temperature, s.humidity);
        hd44780_puts(lcd_, buf);
      }
      hd44780_puts(lcd_, s.light > s.threshold ? "☀" : "☾");
    } break;
    case PAGE_SENSORS: {
      auto s = probes::getSensorInfo();
      hd44780_set_cursor(lcd_, 0, 0);
      { char buf[21]; snprintf(buf, sizeof(buf), "LDR:%4u VOL:%4u", s.light, s.voltage);
        hd44780_puts(lcd_, buf);
      }
      hd44780_set_cursor(lcd_, 0, 1);
      { char buf[21]; snprintf(buf, sizeof(buf), "Thr:%4u %s", s.threshold,
                 (s.light > s.threshold) ? "Day" : "Nite");
        hd44780_puts(lcd_, buf);
      }
      hd44780_set_cursor(lcd_, 0, 2);
      { char buf[21]; snprintf(buf, sizeof(buf), "T:%.1fC H:%.1f%%", s.temperature, s.humidity);
        hd44780_puts(lcd_, buf);
      }
    } break;
    case PAGE_NETWORK: {
      auto w = probes::getWifiInfo();
      // SSID
      hd44780_set_cursor(lcd_, 0, 0);
      { char buf[21]; snprintf(buf, sizeof(buf), "SSID:%-13.13s", w.ssid.c_str()); hd44780_puts(lcd_, buf); }
      // IP address
      hd44780_set_cursor(lcd_, 0, 1);
      { char buf[21]; snprintf(buf, sizeof(buf), "IP:%-15.15s", w.ip.c_str()); hd44780_puts(lcd_, buf); }
      hd44780_set_cursor(lcd_, 0, 2);
      { char buf[21]; snprintf(buf, sizeof(buf), "%c%c%c",
                (w.mode != probes::WifiInfo::OFF) ? 'H' : 'h',
                (w.mode == probes::WifiInfo::STA) ? 'T' : 't',
                mqttIsConnected() ? 'M' : 'm');
        lcd_.print(buf);
      }
    } break;
    case PAGE_RELAYS: {
      auto r = probes::getRelayInfo();
      hd44780_set_cursor(lcd_, 0, 0);
      { char buf[21]; snprintf(buf, sizeof(buf), "State:0x%02X", r.stateMask);
        hd44780_puts(lcd_, buf);
      }
      hd44780_set_cursor(lcd_, 0, 1);
      { char buf[21]; snprintf(buf, sizeof(buf), "Night:0x%02X", r.nightMask);
        hd44780_puts(lcd_, buf);
      }
      // Bits text
      hd44780_set_cursor(lcd_, 0, 2);
      for (int i = 7; i >= 0; --i) {
        hd44780_putchar(lcd_, (r.stateMask & (1<<i)) ? '1' : '0');
      }
    } break;
    case PAGE_LOGS: {
      auto buf = probes::getLogBuffer();
      int sz = buf.size();
      int start = sz > LCD_ROWS ? sz - LCD_ROWS : 0;
      for (uint8_t row = 0; row < LCD_ROWS; ++row) {
        hd44780_set_cursor(lcd_, 0, row);
        if (start + row < sz) {
          std::string &l = buf[start + row];
          if (l.size() > LCD_COLS) {
            auto tail = l.substr(l.size() - LCD_COLS);
            hd44780_puts(lcd_, tail.c_str());
          } else {
            hd44780_puts(lcd_, l.c_str());
          }
        }
      }
    } break;
    case PAGE_SYSTEM: {
      // Reset reason and Wakeup reason
      auto ri = probes::getResetInfo();
      auto wi = probes::getWakeupInfo();
      char buf[32];
      hd44780_set_cursor(lcd_, 0, 0);
      snprintf(buf, sizeof(buf), "RST:%s", ri.reasonStr.c_str());
      hd44780_puts(lcd_, buf);
      hd44780_set_cursor(lcd_, 0, 1);
      snprintf(buf, sizeof(buf), "WKP:%s", wi.causeStr.c_str());
      hd44780_puts(lcd_, buf);
      // Flash info
      auto fi = probes::getFlashInfo();
      hd44780_set_cursor(lcd_, 0, 2);
      snprintf(buf, sizeof(buf), "F:%" PRIu32 "kB@%" PRIu32 "MHz",
               fi.sizeKB, fi.speedMHz);
      hd44780_puts(lcd_, buf);
      // Heap info
      auto hi = probes::getHeapInfo();
      hd44780_set_cursor(lcd_, 0, 3);
      snprintf(buf, sizeof(buf), "H:%" PRIu32 "kB min:%" PRIu32 "kB",
               hi.freeKB, hi.minFreeKB);
      hd44780_puts(lcd_, buf);
    } break;
    case PAGE_SYSTEM2: {
      // Firmware info, CPU, PSRAM, and sketch size
      auto ci     = probes::getChipInfo();
      auto cpu    = probes::getCpuInfo();
      auto ps     = probes::getPsramInfo();
      auto sketch = probes::getSketchInfo();
      char l0[32], l1[32], l2[32], l3[32];
      snprintf(l0, sizeof(l0), "V:%s R:%d C:%d",
               FW_VERSION, ci.revision, ci.cores);
      snprintf(l1, sizeof(l1), "CPU:%dMHz %s%dMHz",
               cpu.freqMHz,
               cpu.pmEnabled ? "p:" : "",
               cpu.curFreqMHz);
      if (ps.supported) {
        snprintf(l2, sizeof(l2), "PSRAM:%" PRIu32 "kB/%" PRIu32 "kB",
                 static_cast<uint32_t>(ps.total >> 10),
                 static_cast<uint32_t>(ps.free  >> 10));
      } else {
        snprintf(l2, sizeof(l2), "PSRAM: n/a");
      }
      snprintf(l3, sizeof(l3), "IMG:%" PRIu32 "KB FR:%" PRIu32 "KB",
               sketch.usedKB, sketch.freeKB);
      writePage(l0, l1, l2, l3);
    } break;
    default:
      break;
  }
}