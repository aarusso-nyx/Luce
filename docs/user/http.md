# LUCE HTTPS (Stage10)

Date: 2026-02-23

## Scope

Stage10 provides HTTPS read-only telemetry/info endpoints.
No plaintext HTTP modes are defined in the current firmware profile.

## NVS schema (`http`)

- `http/enabled` (u8, default `0`)
- `http/port` (u16, default `443`)
- `http/token` (string, required for protected routes)
- `http/tls_dev_mode` (u8, optional for dev cert mode)

## Endpoints

- `GET /api/health`
  - minimal public health and service state
- `GET /api/info`
  - requires bearer token in `Authorization: Bearer <token>`
- `GET /api/state`
  - requires bearer token

## Runtime behavior

- Starts only when Wi-Fi IP is present and config enabled.
- Starts/stops with Wi-Fi state transitions.
- Request handling is read-only and logs method/path/status/duration/source IP.

## Logging

- `[HTTP] request` lines with method, path, status, duration, remote IP.
- `[HTTP] enabled` and startup lifecycle lines.

## Verification

- Evidence: `docs/work/diag/evidence/20260222_214039/90_summary.md`
- Evidence SHA: `2a3b9df`
