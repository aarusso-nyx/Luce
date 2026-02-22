# Architecture (Canonical)

## Queues

- `q_events` (`AppEvent`): normalized events from hardware/network/services.
- `q_commands` (`CommandMsg`): external write intents from CLI/HTTP/MQTT.
- `q_log_async` (`LogMsg`): async logs.
- `q_display` (`DisplayFrame`): queue-driven LCD rendering frames.
- Event queue overflow policy drops `drop_eligible` telemetry before control/fault events.

## Task Graph

- `task_supervisor`: health, heartbeat age, restart scheduling, LED policy.
- `task_reducer`: canonical policy/state reducer.
- `task_command`: maps commands to reducer events.
- `task_relays`: MCP23017 input polling and button event emission.
- `task_sensors`: DHT22 + ADC sampling.
- `task_wifi`: connection lifecycle and network events.
- `task_http`: canonical HTTP API + SSE + static assets.
- `task_mqtt`: canonical MQTT command/telemetry plane.
- `task_cli`: serial + telnet command plane.
- `task_ota`: controlled OTA worker.
- `task_lcd`: consumes `q_display` frames only.
- `task_logger`: async logger with rotation/retention.

## Policy Rules

1. External writes must be `CommandMsg`.
2. Reducer is the single policy writer.
3. Physical buttons remain authoritative.
4. Remote writes are guard-window filtered after recent physical input.
5. Day/night automation computes desired/effective masks and honors override TTL.

## Health Model

`HealthSnapshot` exports:

- service state per task
- restart attempt counts
- heartbeat age per service
- degraded reason per service
- queue depth + high-water marks
- last fault code

## Canonical Contract Rule

- No `/v2` API prefix.
- No compatibility shim layer.
- Canonical HTTP and MQTT contracts are the only supported public interface.
