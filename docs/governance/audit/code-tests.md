# Code vs Tests Audit

- date: 2026-03-01
- code_files_detected: 38
- test_files_detected: 8
- status: FAIL

## Enforced by current tests

- HTTP contract basics: public health/version, auth gating for protected routes, selected `/api/info` fields, `/api/state` + `/api/ota` payloads, `/api/ota/check` source precedence, OTA manual-check lifecycle progress, selected LED route behaviors, captive portal asset + SPA fallback routes, selected 405 responses.
  - [tests/test_http_contract.py](/Users/aarusso/Development/Luce/tests/test_http_contract.py)
- TCP CLI: auth prompt, 3-failure abort, allow/deny behavior for a small command subset.
  - [tests/test_tcp_cli_contract.py](/Users/aarusso/Development/Luce/tests/test_tcp_cli_contract.py)
- WebSocket: upgrade handshake, snapshot schema check, ping/text roundtrip.
  - [tests/test_ws_contract.py](/Users/aarusso/Development/Luce/tests/test_ws_contract.py)
- MQTT: deterministic unsupported-topic response, LED readback topic, control-path effects (`relays/*`, `sensor/threshold`), and `config/*` supported-vs-unsupported handling.
  - [tests/test_mqtt_contract.py](/Users/aarusso/Development/Luce/tests/test_mqtt_contract.py)

## Missing or weak coverage vs code

1. **No direct tests for Wi-Fi/NTP/mDNS lifecycle state machines.**
- Code includes non-trivial transition/backoff logic with IP dependency.
- No tests currently assert these transitions.
- Evidence:
  - [src/net_wifi.cpp](/Users/aarusso/Development/Luce/src/net_wifi.cpp)
  - [src/ntp.cpp](/Users/aarusso/Development/Luce/src/ntp.cpp)
  - [src/mdns.cpp](/Users/aarusso/Development/Luce/src/mdns.cpp)

2. **OTA lifecycle coverage remains partial.**
- Current tests assert manual check request acceptance and observable progress in `/api/ota`.
- Missing explicit assertions for periodic scheduling cadence and detailed failure-class mapping.
- Evidence:
  - [src/ota.cpp](/Users/aarusso/Development/Luce/src/ota.cpp)
  - [tests/test_http_contract.py](/Users/aarusso/Development/Luce/tests/test_http_contract.py:128)

3. **MQTT coverage remains partial around durability/lifecycle.**
- Missing explicit tests for:
  - `config/*` persistence durability after reboot
  - reconnect/backoff behavior
- Evidence:
  - [src/mqtt.cpp](/Users/aarusso/Development/Luce/src/mqtt.cpp)
  - [tests/test_mqtt_contract.py](/Users/aarusso/Development/Luce/tests/test_mqtt_contract.py)

4. **Serial CLI parser/command matrix is largely untested.**
- Extensive command parsing/validation exists; tests target TCP subset only.
- Evidence:
  - [src/cli.cpp](/Users/aarusso/Development/Luce/src/cli.cpp)
  - [tests/test_tcp_cli_contract.py](/Users/aarusso/Development/Luce/tests/test_tcp_cli_contract.py)

## Residual risk

- High behavioral risk remains in stateful subsystems (Wi-Fi/NTP/mDNS/OTA/MQTT config paths) due to limited enforcement despite broad implementation complexity.
