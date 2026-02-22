# Source Map and Component Responsibility

## Core Runtime

- `src/main.cpp` → boot entrypoint and top-level startup orchestration.
- `src/app_config.h` → compile-time constants, pins, limits, queues, tasks, defaults.
- `src/types.h` → event, command, health, config, and runtime model types.
- `src/test_seams.h/.cpp` → deterministic time seams for testing builds.

## Service Control

- `src/service_manager.h/.cpp` → service graph, state machine, restart policy, health snapshots.
- `src/supervisor.h/.cpp` → service lifecycle signaling and status LED policy.
- `src/event_bus.h/.cpp` → queue wrappers and publish/consume APIs.

## Storage and Persistence

- `src/store.h/.cpp` → NVS namespaces and runtime cache management.

## Logging

- `src/logger.h/.cpp` → async logger, level/verbosity and session file controls.

## HAL Layer

- `src/hal_i2c.h/.cpp` → bus init and lock helpers.
- `src/hal_relays.h/.cpp` → relay outputs + button scanning/debouncing.
- `src/hal_lcd.h/.cpp` → LCD rendering and queue-driven frame updates.
- `src/hal_sensors.h/.cpp` → DHT22 + LDR/VCC sensing and filtering.

## Network Layer

- `src/net_wifi.h/.cpp` → Wi-Fi lifecycle.
- `src/srv_http.h/.cpp` → REST server, static routes, and event stream.
- `src/srv_mqtt.h/.cpp` → MQTT subscriptions/publications and ack topic flow.
- `src/srv_cli.h/.cpp` → serial and telnet CLI parsing.
- `src/srv_ota.h/.cpp` → OTA task and verification path.

## Policy and Cross-Cutting

- `src/policy.h/.cpp` → desired/effective mask + override/automation math.
- `src/protocol_utils.h/.cpp` → text formatting utilities for APIs/transport payloads.
- `src/CMakeLists.txt` → ESP-IDF component build wiring.
