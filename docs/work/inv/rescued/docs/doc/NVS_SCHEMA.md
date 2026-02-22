# NVS Schema

Schema version: `2` (`sys/schema_ver`).

## Namespaces

- `sys`
  - `schema_ver`, `boot_count`
- `security`
  - `require_http_auth`, `http_bearer_token`
  - `telnet_require_auth`, `telnet_password`
  - `telnet_max_attempts`, `telnet_lockout_ms`
- `net`
  - `wifi_ssid1`, `wifi_pass1`, `wifi_ssid2`, `wifi_pass2`, `hostname`, `mdns_enable`
- `mqtt`
  - `enabled`, `uri`, `user`, `pass`, `client_id`, `base_topic`, `qos_default`, `retain_default`
- `http`
  - `enabled`, `port`, `ws_enabled`
- `cli`
  - `serial_enabled`, `telnet_enabled`, `telnet_port`, `auth_mode`
- `relay`
  - `state_mask`, `night_mask`, `light_threshold`, `button_debounce_ms`, `override_ttl_s`
- `sensor`
  - `sample_ms`, `dht_enable`, `ldr_enable`, `calibration`
- `log`
  - `verbosity`, `session_file_enable`, `max_file_size_kb`, `ring_lines`, `retention_files`
- `ota`
  - `enabled`, `url`, `ca_pem_hash`, `channel`, `auto_check_ms`

## Notes

- Canonical implementation only; no compatibility namespaces/shims.
- Defaults are loaded first, then NVS overrides, then canonical namespaces are persisted.
