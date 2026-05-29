# LUCE Product Documentation

Authoritative docs for implementation behavior and operations.

- `architecture.md` — compile-time strategy architecture and observability model
- `cli-contract.md` — serial and TCP command contract
- `wifi-lifecycle.md` — Wi-Fi state machine and config keys
- `ntp.md` — SNTP service behavior and CLI
- `mdns.md` — zero-config LAN advertising
- `mqtt.md` — MQTT control subscriptions, outbound telemetry, and compatibility aliases
- `http.md` — HTTPS authenticated API (state + control routes)
- `ota.md` — NET1 OTA configuration, CLI/API control, and validation caveats
- `nvs-schema.md` — persisted configuration keys
- `hardware-map.md` — strategy-independent hardware constants and pin mapping
- `testing.md` — firmware, host, simulator, and hardware test workflows
- `contract-index.md` — canonical command/transport matrix and evidence pointers

Verification for these docs should use local evidence in:

- `docs/work/diag/<run_id>/`

Current evidence should be regenerated with `./scripts/luce.sh health`,
`./scripts/luce.sh build --env net1`, `scripts/run_host_tests.sh`, and
hardware-backed `./scripts/luce.sh test --layers boot --env net1 --boot-duration 45`.
