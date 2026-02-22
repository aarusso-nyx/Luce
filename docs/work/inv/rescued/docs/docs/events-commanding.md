# Event and Command Contracts

## Event Domain Model

All internal runtime transitions are represented by `AppEvent`.

### EventDomain

- `SYSTEM`
- `BUTTON`
- `RELAY`
- `SENSOR`
- `NET`
- `MQTT`
- `CLI`
- `HTTP`
- `OTA`
- `FAULT`

### EventOrigin

- `INTERNAL`
- `BUTTON`
- `CLI`
- `HTTP`
- `MQTT`
- `OTA`

### EventPriority

- `CRITICAL` (lowest numeric value)
- `NORMAL`
- `TELEMETRY`

### AppEvent fields

Core fields used by reducer, logger, and transport publishing:

- `domain`
- `priority`
- `drop_eligible`
- `ts_ms`
- `correlation_id`
- Union payload (`ButtonEventData`, `RelayEventData`, `SensorEventData`, ...).

## CommandBus Model

`CommandMsg` is the external write path envelope for CLI/HTTP/MQTT command handlers.

- `source`: command origin enum
- `request_id`: origin-provided or generated id
- `correlation_id`: cross-path correlation
- `auth_ok`: transport-auth authorization result
- `flags`: extensibility bits
- `action`: command action enum
- `target`: index/id target for per-relay actions
- `value`: integer payload
- `text`: string payload (key/value or auth context)

## Reduction Rules

1. Transport handlers enqueue commands only after auth policy checks.
2. `task_command` converts command envelopes to canonical events and publishes to reducer.
3. `task_reducer` applies all state mutations and publishes derived state events.
4. HAL actions are side-effect points (relay writes, network notifications, etc.).

## Physical Override Rule

`BUTTON` events are treated as highest precedence:

- button press updates internal button timestamp map
- applies relay toggle intent into desired state
- marks override window for impacted relay
- automation reevaluates with override window in effect

## Error/Event Visibility

Fault events are non-droppable, flagged with `CRITICAL`, and propagated to:

- supervisor (error level escalation),
- logger (structured events),
- HTTP/MQTT event stream/status publishing where implemented.
