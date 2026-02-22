# Operations Guide

## LED Semantics

- LED0 (WiFi)
  - solid: connected
  - blink slow: connecting/disconnected
- LED1 (MQTT)
  - solid: connected
  - off: disconnected/disabled
- LED2 (Error)
  - off: healthy
  - blink slow: degraded (recovering/restarting)
  - blink fast: critical fault

## Security Defaults

- Remote HTTP writes require auth token by default.
- Remote MQTT write topics require token-prefixed payloads when auth is enabled.
- Telnet is disabled unless explicitly enabled and password is configured.
- OTA requires HTTPS and trust material.

## Recovery Workflow

1. Check `health` or `GET /api/health`.
2. Inspect logs from `log` or `GET /api/logs?limit=100`.
3. Validate connectivity with `network` and `mqtt`.
4. Toggle failing feature with `feature ...`.
5. Reboot if needed; factory reset only as last resort.

## OTA Operation

- CLI: `ota start [url]`
- HTTP: `POST /api/ota?url=...`
- MQTT: publish to `<base>/cmd/ota/start`
- MQTT auth payload format (when enabled): `token=<token>;https://...`

## Session Logs

When enabled, logs are written to `/storage/log/`.

- rotation: `log.max_file_size_kb`
- retention: `log.retention_files`

Use `GET /api/logs` for recent in-memory logs and storage stats.
