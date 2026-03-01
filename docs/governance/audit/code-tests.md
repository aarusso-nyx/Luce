# Code vs Tests Audit

- date: 2026-03-01
- code_files_detected: 38
- test_files_detected: 9
- status: PASS

## Enforced by current tests

- HTTP contract basics: public health/version, auth gating for protected routes, selected `/api/info` fields, `/api/state` + `/api/ota` payloads, `/api/ota/check` source precedence, OTA manual-check lifecycle progress, selected LED route behaviors, captive portal asset + SPA fallback routes, selected 405 responses.
  - [tests/test_http_contract.py](/Users/aarusso/Development/Luce/tests/test_http_contract.py)
- TCP CLI: auth prompt, 3-failure abort, allow/deny behavior for a small command subset.
  - [tests/test_tcp_cli_contract.py](/Users/aarusso/Development/Luce/tests/test_tcp_cli_contract.py)
- WebSocket: upgrade handshake, snapshot schema check, ping/text roundtrip.
  - [tests/test_ws_contract.py](/Users/aarusso/Development/Luce/tests/test_ws_contract.py)
- MQTT: deterministic unsupported-topic response, LED readback topic, control-path effects (`relays/*`, `sensor/threshold`), and `config/*` supported-vs-unsupported handling.
  - [tests/test_mqtt_contract.py](/Users/aarusso/Development/Luce/tests/test_mqtt_contract.py)
- Network lifecycle and serial parser matrix: reboot capture assertions for Wi-Fi/NTP/mDNS lifecycle tags and serial CLI parser/error handling matrix.
  - [tests/test_serial_cli_contract.py](/Users/aarusso/Development/Luce/tests/test_serial_cli_contract.py)

## Remaining notes

- OTA cadence assertions are configuration-sensitive and may skip when `ota.interval_s=0` or large.
- MQTT reconnect/backoff test requires managed broker mode (`--spawn-test-mqtt-broker`) to exercise outage/recovery deterministically.
