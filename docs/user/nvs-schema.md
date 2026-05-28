# LUCE NVS Schema Reference

Date: 2026-05-28

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
  - `token` string (secret)
  - `tls_dev_mode` u8 (0 = prod, 1 = dev)
  - `cert_pem` string (PEM X.509 cert, ~2 KB max; required for HTTPS start)
  - `key_pem` string (PEM private key, ~2 KB max, secret; required for HTTPS start)
- `ota`
  - `enabled` u8
  - `url` string
  - `check_interval_s` u32
  - `request_timeout_ms` u32
- `relays`
  - `state` u32 (stored relay request mask, loaded at startup)
  - `state_fmt` u8 = 1 when `state` is stored as request-mask (internal migration marker)
  - `night_mask` u8 (relay policy: suppresses these relays during day)
- `compat`
  - `log_console_fmt` string
  - `log_file_fmt` string
  - `log_console_level` string
  - `log_file_level` string

## Defaults

- Missing keys are logged and defaulted to safe values.
- Network service booleans default to disabled unless explicitly enabled, except current Wi-Fi firmware code still falls back to a compiled default profile when the `wifi` namespace is missing. That Wi-Fi default is a known implementation gap against the intended safe-default policy.

## Verification

- Generate current local evidence under `docs/work/diag/` with `./scripts/luce.sh health` and `./scripts/luce.sh test --layers boot --env net1 --boot-duration 45`.
