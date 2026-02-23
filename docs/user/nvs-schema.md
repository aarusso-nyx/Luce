# LUCE NVS Schema Reference

Date: 2026-02-23

LUCE uses namespace-scoped key-value configuration.

## Base and common namespaces

- `wifi`
  - `enabled` u8
  - `ssid` string
  - `pass` string
  - `hostname` string
  - `max_retries` u32
  - `backoff_min_ms` u32
  - `backoff_max_ms` u32
- `net`
  - `hostname` string
- `ntp`
  - `enabled` u8
  - `server1` string
  - `server2` string
  - `server3` string
  - `sync_timeout_s` u32
  - `sync_interval_s` u32
- `mdns`
  - `enabled` u8
  - `instance` string
- `cli_net`
  - `enabled` u8
  - `port` u32
  - `token` string
  - `idle_timeout_s` u32
- `mqtt`
  - `enabled` u8
  - `uri` string
  - `client_id` string
  - `base_topic` string
  - `username` string
  - `password` string
  - `tls_enabled` u8
  - `ca_pem_source` string
  - `ca_pem` string
  - `qos` u32
  - `keepalive_s` u32
- `http`
  - `enabled` u8
  - `port` u16
  - `token` string
  - `tls_dev_mode` u8

## Defaults

- Missing keys are logged and defaulted to safe values.
- Booleans default to disabled for all network namespaces unless explicitly enabled.

## Verification

- Evidence: `docs/work/diag/evidence/20260222_214039/90_summary.md`
- Evidence SHA: `2a3b9df`
