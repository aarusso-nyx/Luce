# Logging, Diagnostics, and Observability

## Logging Model

- Structured in-code tags and levels (`LogLevel`, `LogSeverity`).
- Asynchronous log queue via `q_log_async`.
- Console + optional session file output.

## Session Log Files

Configured by `LogConfig`:

- `session_file_enable`
- `max_file_size_kb`
- `ring_lines`
- `retention_files`

On boot/session start, a session log file can be created under storage and maintained with rotation + retention.

## Real-Time Observability

- `task_logger` drains `q_log_async`.
- `task_supervisor` publishes queue HWM and service metrics into `HealthSnapshot`.
- `task_reducer` publishes relay/sensor state update events.
- `/api/health` exposes queue and service diagnostics.

## Diagnostics Channels

- LCD status lines (`DisplayFrame`) receive asynchronous updates.
- MQTT state topics can reflect faults/status changes.
- HTTP log endpoint exposes current logs (`/api/logs`).

## Priority/Drop Behavior

- Event and log producers avoid blocking:
  - critical events/faults stay non-droppable in reducer and service logic
  - normal telemetry may yield to control events under queue pressure
