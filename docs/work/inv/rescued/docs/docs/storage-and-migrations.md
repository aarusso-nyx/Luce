# Storage and Migration Behavior

## Persistence Model

`store.*` handles NVS + runtime cache and emits an in-memory mirror guarded by RTOS mutex.

### Config Namespaces and Keys

- `sys`: `schema_ver`, `boot_count`
- `security`: auth credentials and auth guard tuning
- `net`: SSIDs/passphrases, hostname, mDNS
- `mqtt`: broker config
- `http`: feature and server settings
- `cli`: CLI transport flags
- `relay`: masks, threshold, debounce, override TTL
- `sensor`: sample mode and calibration
- `log`: verbosity and session-file behavior
- `ota`: OTA enablement + trust fields + schedule

## Defaults

Default configuration is generated at first boot:

- `schema_ver = 2`
- HTTP/MQTT/CLI services enabled as per `app_config.h` defaults
- OTA enabled with empty URL by default (`ota_enabled` true; `url` empty)
- logger session file enabled and rotating defaults

## Request ID Tracking

`store_next_request_id()` centralizes monotonically increasing request IDs used by command and event correlation.

## Runtime Cache Strategy

- All reads return snapshots (`store_get_config`, `store_get_runtime`).
- Setters perform lock-protected updates to prevent torn reads across tasks.
- Boot count increments during init.

## Validation and Migration Notes

The current implementation initializes defaults if NVS data is unavailable and persists back to namespace set. Any migration logic should be implemented by evolving `schema_ver` handling and filling missing keys with defaults.
