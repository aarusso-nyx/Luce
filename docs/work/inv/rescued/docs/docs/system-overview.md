# Luce Firmware System Overview

## Purpose

`Luce` is a physical-relay automation firmware for ESP32 with:

- MCP23017 GPIO expander for relay and button I/O
- I2C LCD (`20x4`)
- DHT22 temperature/humidity sensor
- LDR-based light sensing
- Wi-Fi-connected control planes (HTTP, MQTT, Telnet CLI, OTA)

The implementation is intentionally event-driven and centered on a single state reducer.

## Runtime Phases

1. `app_main()` initializes flash and filesystem.
2. `service_manager_init()` initializes shared infra:
   - event bus queues
   - NVS/config store
   - logger with persisted verbosity
   - supervisor
   - I2C and HAL initialization
3. `service_manager_start()` creates core tasks.
4. Supervisor maintains health/restart policy and restart scheduling.

## Service Topology

Core services are partitioned into:

- I/O and sensing service layer (`hal_*`, sensor task, relay task)
- State orchestration (`service_manager`, reducer, command worker)
- Networking (`net_wifi`, `srv_http`, `srv_mqtt`, `srv_cli`, `srv_ota`)
- Cross-cutting operations (`logger`, `supervisor`)
- Display and observability (`hal_lcd`, event bus display queue)

## Single Source of Truth

All mutable behavior enters the device through `CommandMsg` or low-level events and is reconciled by the reducer.
The reducer owns relay policy outcomes (`desired_mask`, `effective_mask`) and sensor snapshots.

## Physical Authority Rule

- Every physical button event is authoritative by default.
- Button events override remote changes while within configured guard windows.
- Override timers are tracked per relay and applied back into policy recomputation.

## Canonical Interface Constraints

- HTTP routes remain canonical (no `/v2` aliasing).
- MQTT topics are canonical (`<base>/...` style topic tree as implemented).
- CLI writes pass through the same command bus and policy checks as network writes.
