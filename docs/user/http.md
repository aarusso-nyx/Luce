# LUCE HTTP API + Captive Portal (NET1)

Date: 2026-03-01

## Scope

NET1 provides a TLS-protected API surface on HTTPS and a captive portal UI served from `./data/webapp` on plain HTTP port `80`.

## NVS schema (`http`)

- `http/enabled` (u8, default `0`)
- `http/port` (u16, default `443`) for HTTPS API
- `http/token` (string, required for protected routes)
- `http/tls_dev_mode` (u8, optional for dev cert mode)

## Endpoints

- `GET /api/health`
  - minimal public health and service state
- `GET /api/info`
  - requires bearer token in `Authorization: Bearer <token>`
  - includes compatibility fields for legacy consumers:
    - identity/build: `name`, `version`, `strategy`, `sha`, `build`, `uptimeMs`, `uptime_s`
    - relay/sensor: `relays`, `nightMask`, `day`, `threshold`, `light`, `temperature`, `humidity`, `sensor_ok`
    - network: `wifi_ip` and nested `network.{ip,wifiConnected,mqttConnected,ntpSynced}`
    - current HTTP flags: `http_enabled`, `http_port`, `tls`
- `GET /api/state`
  - requires bearer token
- `GET /api/ota`
  - requires bearer token
- `POST /api/ota/check`
  - requires bearer token; optional `url` query parameter or body payload
- `PUT /api/ota/check`
  - alias of POST behavior for update checks; requires bearer token
- `GET /api/version`
  - returns firmware version/identity details
- `GET /`
  - serves `./data/webapp/index.html`
- `GET /index.html`
- `GET /app.css`
- `GET /script.js`
  - all unresolved paths on captive HTTP fall back to `index.html` for SPA-style navigation

## Runtime behavior

- Starts only when Wi-Fi IP is present and config is enabled.
- HTTPS API server starts on `http/port` (default `443`) and registers `api/*` endpoints.
- Captive portal starts on plain HTTP port `80` when enabled, unless `http/port` is also `80`.
- Starts/stops with Wi-Fi state transitions.

## Smoke checks

- Run `./scripts/http_api_smoke.sh --host https://<device-ip> --token <http-token>` to verify:
  - GET endpoints for health/info/version/ota.
  - POST/PUT `/api/ota/check` accepted.
  - unsupported methods return `405`.
  - unauthenticated protected endpoints return `401` (unless `--skip-unauth`).

## Logging

- `[HTTP]` logs for API and portal startup.
- `[HTTP][CAPTIVE]` logs for portal startup and request fallbacks.
