// eventBus.cpp: Implements central EventBus using FreeRTOS queue
#include <map>
#include <vector>

#include "config.h"
#include "eventBus.h"
#include <freertos/queue.h>

// Internal queue handle (static file-scope)
static QueueHandle_t queue = nullptr;

// Subscribers: map event type to list of callbacks
static std::map<EventType, std::vector<EventCallback>> subscribers;

void EventBus::init() {
  if (!queue) {
    // Create central event queue
    queue = xQueueCreate(config::EVENT_QUEUE_LENGTH, sizeof(BusEvent));
  }
}

void EventBus::subscribe(EventType type, EventCallback cb) {
  subscribers[type].push_back(cb);
}

void EventBus::publish(const BusEvent& ev) {
  // Lazy-init queue if needed
  if (!queue) {
    init();
  }
  xQueueSend(queue, &ev, 0);
}

void EventBus::dispatch() {
  if (!queue) return;
  BusEvent ev;
  while (xQueueReceive(queue, &ev, 0) == pdTRUE) {
    auto it = subscribers.find(ev.type);
    if (it != subscribers.end()) {
      for (auto& cb : it->second) {
        cb(ev);
      }
    }
  }
}