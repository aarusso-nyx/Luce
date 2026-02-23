# LUCE Product Documentation

Authoritative docs for implementation behavior and operations.

- `architecture.md` — compile-time stage architecture and observability model
- `cli-contract.md` — serial and TCP command contract
- `wifi-lifecycle.md` — Wi-Fi state machine and config keys
- `ntp.md` — SNTP service behavior and CLI
- `mdns.md` — zero-config LAN advertising
- `mqtt.md` — publish-only MQTT telemetry and config
- `http.md` — HTTPS read-only API
- `nvs-schema.md` — persisted configuration keys
- `hardware-map.md` — stage hardware constants and pin mapping
- `contract-index.md` — canonical command/transport matrix and evidence pointers

Verification for these docs should use local evidence in:

- `docs/work/diag/evidence/20260222_221921/90_summary.md`
- git SHA `2a3b9df`
