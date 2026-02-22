#pragma once
#ifdef DISABLE_WDT

// No-op watchdog functions when disabled
inline void WDT_INIT(uint32_t timeout_ms, bool panic) {}
inline void WDT_ADD() {}
inline void WDT_RESET() {}
#else
#include <esp_task_wdt.h>
#include <esp_err.h>
#include <logger.h>
// Initialize the task watchdog (timeout in seconds)
inline void WDT_INIT(uint32_t timeout_s, bool panic) { 
    ESP_ERROR_CHECK(esp_task_wdt_init(timeout_s, panic)); 
    LOGSYS("WDT","Init",String(timeout_s) + "s timeout");
}
// Add current task to watchdog monitoring
inline void WDT_ADD() { ESP_ERROR_CHECK(esp_task_wdt_add(NULL)); }
// Reset watchdog timer for current task
inline void WDT_RESET() { ESP_ERROR_CHECK(esp_task_wdt_reset()); }
#endif