#pragma once

#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace luce {

struct TaskBudget {
  const char* name = "";
  std::size_t stack_words = 0;
  UBaseType_t priority = 0;
  BaseType_t pinned_core = tskNO_AFFINITY;
};

[[nodiscard]] inline bool start_task(TaskFunction_t task_entry, const TaskBudget& budget, void* arg = nullptr,
                                    TaskHandle_t* handle = nullptr) {
  if (budget.pinned_core == tskNO_AFFINITY) {
    return xTaskCreate(task_entry, budget.name, static_cast<configSTACK_DEPTH_TYPE>(budget.stack_words), arg,
                       budget.priority, handle) == pdPASS;
  }
  return xTaskCreatePinnedToCore(task_entry, budget.name, static_cast<configSTACK_DEPTH_TYPE>(budget.stack_words), arg,
                                 budget.priority, handle, budget.pinned_core) == pdPASS;
}

[[nodiscard]] inline bool start_task_once(TaskHandle_t& handle, TaskFunction_t task_entry, const TaskBudget& budget,
                                          void* arg = nullptr) {
  if (handle != nullptr) {
    return false;
  }
  return start_task(task_entry, budget, arg, &handle);
}

}  // namespace luce
