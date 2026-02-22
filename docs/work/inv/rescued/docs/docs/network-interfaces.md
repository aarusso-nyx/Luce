# Network Interfaces and Transport Contracts

## HTTP Surface (Canonical)

Implemented API endpoints in this codebase are handled in `doc/API.md` and served in `srv_http.cpp`:

- `GET /api/info`
- `GET /api/config`
- `GET /api/features`
- `GET /api/health`
- `GET /api/relays`
- `GET /api/sensor`
- `GET /api/logs`
- `GET /api/ota`
- `GET /api/events` (SSE/event stream)
- `POST`/`PUT` endpoints for feature/config/runtime mutations (authorization gated)

Error envelope:

- `{ "error": { "code": "...", "message": "...", "requestId": "..." } }`

## MQTT Surface

Canonical topic families:

- `base/cmd/...` for command ingestion
- `base/relays/...`
- `base/sensor/...`
- `base/feature/...`
- `base/status/...`
- `base/ack/<requestId>` for command response

Security is transport-level via optional token where enabled:

- `token=<value>` in command payloads

## CLI / Telnet

- Serial transport always available when enabled.
- Telnet transport is optional and can be auth-gated.
- CLI actions execute through `CommandMsg` and reducer.

## Security Controls

- Write-only endpoints require auth by default in security config.
- OTA commands are treated as privileged actions.
- HTTP auth defaults are conservative.
