// tasks.hpp: Template-based FreeRTOS task generator to replace MAKE_TASK macro
#pragma once

#include "config.h"    // for STACK_SIZE

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Watchdog integration
#include <esp_task_wdt.h>
#include <esp_err.h>
#include "logger.h"
// Integrated watchdog functions (from former watchdog.h)
#ifdef DISABLE_WDT
inline void WDT_INIT(uint32_t timeout_s, bool panic) {}
inline void WDT_ADD() {}
inline void WDT_RESET() {}
#else
// Initialize the task watchdog (timeout in seconds)
inline void WDT_INIT(uint32_t timeout_s, bool panic) {
  ESP_ERROR_CHECK(esp_task_wdt_init(timeout_s, panic));
  LOGSYS("WDT", "Init", "%us timeout", (unsigned)timeout_s);
}
// Add current task to watchdog monitoring
inline void WDT_ADD() { ESP_ERROR_CHECK(esp_task_wdt_add(NULL)); }
// Reset watchdog timer for current task
inline void WDT_RESET() { ESP_ERROR_CHECK(esp_task_wdt_reset()); }
#endif

namespace tasks {
// Task template: wraps Init and Loop functions into a FreeRTOS task
// Init and Loop functions must return bool: Init() false skips loop and ends task; Loop() false breaks loop and ends task
template<bool (*Init)(), bool (*Loop)(), uint32_t DelayMs, size_t StackSize = config::STACK_SIZE>
struct Task {
  // Optional runtime parameters (only used by create overload)
  struct Params { uint32_t delayMs; };

  // Entry point for the FreeRTOS task
  static void entry(void* pvParameters) {
    // Call Init if provided; abort task if Init() returns false
    if (Init) {
      if (!Init()) {
        if (pvParameters) {
          vPortFree(pvParameters);
        }
        vTaskDelete(NULL);
        return;
      }
    }
    WDT_ADD();
    // Initial stack watermark report
    UBaseType_t minHwm = uxTaskGetStackHighWaterMark(NULL);
    LOGINFO("Task","StackInit","%s min free %u words", pcTaskGetName(NULL), (unsigned)minHwm);

    // If a Loop function is provided, run it periodically until it returns false
    if (Loop) {
      for (;;) {
        // Invoke Loop; break if it returns false
        if (!Loop()) {
          break;
        }
        WDT_RESET();
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        if (hwm < minHwm) {
          minHwm = hwm;
          LOGINFO("Task","StackLow","%s new min free %u words", pcTaskGetName(NULL), (unsigned)minHwm);
        }
        // Determine delay (compile-time or runtime override)
        uint32_t d = DelayMs;
        if (pvParameters) {
          auto params = static_cast<Params*>(pvParameters);
          d = params->delayMs;
        }
        vTaskDelay(pdMS_TO_TICKS(d));
      }
    }
    // No Loop: clean up and exit
    if (pvParameters) {
      vPortFree(pvParameters);
    }
    vTaskDelete(NULL);
  }

  // Create with compile-time DelayMs and StackSize
  static void create(const char* name,
                     UBaseType_t prioOffset,
                     BaseType_t core = tskNO_AFFINITY) {
    UBaseType_t prio = tskIDLE_PRIORITY + prioOffset;
    BaseType_t rc;
    if (core == tskNO_AFFINITY) {
      rc = xTaskCreate(entry, name, StackSize, nullptr, prio, nullptr);
    } else {
      rc = xTaskCreatePinnedToCore(entry, name, StackSize, nullptr, prio, nullptr, core);
    }
    if (rc != pdPASS) {
      LOGERR("Task","Create","Failed to create task '%s' (rc=%d)", name, (int)rc);
    }
  }

  // Create with runtime delayMs and optional stackSize override
  static void create(const char* name,
                     UBaseType_t prioOffset,
                     uint32_t delayMs,
                     size_t stackSize = StackSize,
                     BaseType_t core = tskNO_AFFINITY) {
    auto params = static_cast<Params*>(pvPortMalloc(sizeof(Params)));
    if (!params) {
      LOGERR("Task","Create","Failed to allocate params for '%s'", name);
      return;
    }
    params->delayMs = delayMs;
    UBaseType_t prio = tskIDLE_PRIORITY + prioOffset;
    BaseType_t rc;
    if (core == tskNO_AFFINITY) {
      rc = xTaskCreate(entry, name, stackSize, params, prio, nullptr);
    } else {
      rc = xTaskCreatePinnedToCore(entry, name, stackSize, params, prio, nullptr, core);
    }
    if (rc != pdPASS) {
      LOGERR("Task","Create","Failed to create task '%s' (rc=%d)", name, (int)rc);
      vPortFree(params);
    }
  }
};

// Detached task: wraps a single FreeRTOS-compatible function (void (*)(void*))
template<void (*Fn)(void*)>
struct DetachedTask {
  // Create a detached task; optionally pass pvParameters
  static void create(const char* name,
                     UBaseType_t prioOffset,
                     void* pvParameters = nullptr,
                     BaseType_t core = tskNO_AFFINITY) {
    UBaseType_t prio = tskIDLE_PRIORITY + prioOffset;
    (void)prio;
    if (core == tskNO_AFFINITY) {
      xTaskCreate((TaskFunction_t)Fn, name, config::STACK_SIZE,
                  pvParameters, prio, nullptr);
    } else {
      xTaskCreatePinnedToCore((TaskFunction_t)Fn, name, config::STACK_SIZE,
                              pvParameters, prio, nullptr, core);
    }
  }
};
} // namespace tasks