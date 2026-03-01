// lcd.h: 20x4 LCD manager showing pages of probe data
#pragma once
// Removed Arduino legacy and WiFi Arduino wrapper; using pure ESP-IDF and probes API
#include <driver/i2c.h>
#include <i2cdev.h>
#include <pcf8574.h>
#include <hd44780.h>
#include "config.h"
#include "probes.h"
#include "settings.h"
#include "relays.h"
#include "logger.h"
// FreeRTOS timer support
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <time.h>
#include <string>

// Key codes for page navigation (stubbed):
// e.g. map buttons or numeric keypad to these values
enum LcdKey {
  LCD_KEY_NONE = 0,
  LCD_KEY_UP,
  LCD_KEY_DOWN,
  LCD_KEY_LEFT,
  LCD_KEY_RIGHT,
  LCD_KEY_ENTER,
  LCD_KEY_EXIT,
  LCD_KEY_1,
  LCD_KEY_2,
  LCD_KEY_3,
  LCD_KEY_4,
  LCD_KEY_5,
  LCD_KEY_6,
  LCD_KEY_7
};

// Free-function API for 20x4 I2C LCD via PCF8574+HD44780
// Initialize LCD hardware and timers; return false on error
bool lcdInit();
// LCD loop (invoked by tasks template); return false to terminate task
bool lcdLoop();
// Show formatted boot message (scrolling buffer)
void lcdShowBoot(const char* fmt, ...);
// Handle a navigation key press
void lcdHandleKey(LcdKey key);
// Helper: write up to 4 text lines (max LCD_ROWS)
void lcdWritePage(const char* row0,
                  const char* row1 = nullptr,
                  const char* row2 = nullptr,
                  const char* row3 = nullptr);