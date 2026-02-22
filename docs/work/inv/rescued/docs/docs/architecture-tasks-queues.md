# Architecture, Tasks, and Queues

## Task Graph

- `task_supervisor` (prio 6): service health, heartbeat accounting, restart decisions, queue HWM collection.
- `task_reducer` (prio 5): event domain reducer for policy and state mutations.
- `task_command` (prio 5): command adapter; translates authenticated/enqueued commands into reducer events.
- `task_relays` (prio 5): polls MCP23017 state and GPIO transitions.
- `task_sensors` (prio 4): periodic DHT/LDR sampling.
- `task_wifi` (prio 4): network bootstrap and wifi lifecycle.
- `task_http` (prio 3): REST API service loop.
- `task_mqtt` (prio 3): MQTT subscriptions, publications, ack emit.
- `task_cli` (prio 3): serial/telnet interface and parser loop.
- `task_ota` (prio 2): OTA orchestration.
- `task_lcd` (prio 2): display render task.
- `task_logger` (prio 2): asynchronous log sink and file/console emission.

## Queues

The system uses four bounded queues:

- `q_events`: `AppEvent` objects.
- `q_commands`: `CommandMsg` objects.
- `q_log_async`: `LogMsg` objects.
- `q_display`: `DisplayFrame` objects.

Depth values come from `app_config.h` (`Q_EVENTS_DEPTH`, `Q_COMMANDS_DEPTH`, `Q_LOG_DEPTH`, `Q_DISPLAY_DEPTH`).

## Drop Policy

Event bus enqueue behavior differs by queue:

- Event queue: drop oldest low-priority eligible entry when full.
- Command queue: strict enqueue, no drop helper.
- Log/display queues: drop oldest when full (non-blocking overwrite policy).

## Queue Introspection

`service_manager::task_supervisor` periodically snapshots:

- current queue depth
- queue high-water mark

These values appear in `HealthSnapshot` and are exported via `/api/health` and status topics.
