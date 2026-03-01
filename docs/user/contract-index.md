# LUCE Product Contract Index

Date: 2026-02-28

## Strategy/Feature Contracts

- `cli-contract.md` — Serial + TCP command model (`help`, `status`, `wifi.status`, `time.status`, `mqtt.status`, `mdns.status`, `cli_net.status`, `http.status`)
- `wifi-lifecycle.md` — NET0 behavior and event model
- `ntp.md` — NET0 SNTP state machine and `time.status`
- `mdns.md` — NET0 hostname + TXT records + `mdns.status`
- `mqtt.md` — NET1 publish-only telemetry + `mqtt.status`, `mqtt.pubtest`
- `http.md` — NET1 HTTPS-only API + `http.status`
- `architecture.md` — Strategy gating and direct orchestration model
- `nvs-schema.md` — NVS namespaces and key defaults
- `hardware-map.md` — pin map and relay/button electrical assumptions
- `testing.md` — firmware-only hardware smoke validation (`luce_net1`)

## Transport and command matrix (authoritative)

- Serial CLI: `LUCE_STRATEGY >= LUCE_STRATEGY_CORE`
- TCP CLI: `LUCE_STRATEGY >= LUCE_STRATEGY_NET0` (read-only command subset + AUTH)
- HTTPS CLI/API: `LUCE_STRATEGY >= LUCE_STRATEGY_NET1` (read-only API surface)

## Evidence references

- Evidence index (latest anchor): `docs/work/diag/evidence/20260222_221921/90_summary.md`
- Governance scorecard pointer: `docs/governance/compliance/scorecard-current.md`

## Deprecation notes

- `docs/user/CLI.md` is a launcher page; canonical contract is `docs/user/cli-contract.md`.
- `docs/dev/hardware-map.md` is a historical reference copy; canonical map is `docs/user/hardware-map.md`.
