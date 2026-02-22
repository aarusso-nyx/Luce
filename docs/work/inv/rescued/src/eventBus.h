// eventBus.h: Central publish/subscribe event bus abstraction
#pragma once

#include <functional>

#include "config.h"    // Provides BusEvent and EventType

// Callback signature for events
using EventCallback = std::function<void(const BusEvent&)>;

class EventBus {
public:
  // Initialize the event queue; call once at startup
  static void init();
  // Subscribe to a specific event type
  static void subscribe(EventType type, EventCallback cb);
  // Publish an event to all subscribers
  static void publish(const BusEvent& ev);
  // Dispatch pending events; call in main loops (e.g., mqttLoop)
  static void dispatch();
};