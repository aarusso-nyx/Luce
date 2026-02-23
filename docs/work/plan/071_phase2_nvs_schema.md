# Phase 2 NVS Schema

## Namespace `wifi`

Used by Stage5 (`LUCE_STAGE >= 5`), read by `wifi_startup()` in `src/net_wifi.cpp`.

| Key | Type | Default | Purpose |
| --- | ---- | ------- | ------- |
| `ssid` | `string` | `""` | SSID for STA connect |
| `pass` | `string` | `""` | Wi‑Fi password (masked in logs) |
| `hostname` | `string` | `"luce-esp32"` | STA hostname |
| `enabled` | `u8` | `0` | Master feature gate; if `0`, Wi‑Fi remains disabled |
| `max_retries` | `u32` | `6` | Maximum reconnect attempts before entering `STOPPED` |
| `backoff_min_ms` | `u32` | `500` | Backoff base (ms) |
| `backoff_max_ms` | `u32` | `8000` | Backoff upper bound (ms) |

## Namespace `ntp`

Used by Stage6 (`LUCE_STAGE >= 6`), read by `ntp_startup()` in `src/ntp.cpp`.

| Key | Type | Default | Purpose |
| --- | ---- | ------- | ------- |
| `enabled` | `u8` | `0` | Master feature gate for time sync |
| `server1` | `string` | `"pool.ntp.org"` | Primary NTP server |
| `server2` | `string` | `"time.google.com"` | Secondary NTP server |
| `server3` | `string` | `""` | Optional tertiary NTP server |
| `sync_timeout_s` | `u32` | `30` | Seconds to wait for successful sync |
| `sync_interval_s` | `u32` | `3600` | Re-sync interval when synced |
