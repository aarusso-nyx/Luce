# LUCE Wi-Fi Lifecycle

Date: 2026-02-28

## Activation

Wi-Fi is compiled in when `LUCE_NET_CORE=1`.
Runtime reads settings from NVS namespace `wifi`:

- `enabled` (u8, 0/1)
- `ssid` (string)
- `pass` (string)
- `hostname` (string)
- `max_retries` (u32)
- `backoff_min_ms` (u32)
- `backoff_max_ms` (u32)

Missing keys use safe defaults and are logged.

## Runtime behavior

- Event loop task transitions between:
  - disabled
  - starting
  - scanning
  - connecting
  - connected / disconnected
  - backoff
- Retry/backoff values are bounded and stateful across attempts.
- On disconnect or loss of IP, network features depending on IP (mDNS/MQTT/HTTP/TCP CLI) are updated to disconnected state.

## CLI and observability

- `wifi.status` prints current state, attempts, backoff, RSSI and IP/gateway/mask snapshot.
- `wifi.scan` prints scan results and RSSI.
- Logging tags:
  - `[WIFI][LIFECYCLE]`
  - `[WIFI][NVS]`
  - `[WIFI]`

## Strategy dependency notes

- NET0+ services:
  - NET0: Wi-Fi status and scan commands
- NET0+/NET1: SNTP requires IP before sync
- NET0+/NET1: network services start or resume only when IP is available

## Verification

- Evidence: `docs/work/diag/evidence/20260222_214039/90_summary.md`
- Evidence SHA: `2a3b9df`
