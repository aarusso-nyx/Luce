# LUCE Product Contract Index

Date: 2026-02-23

## Stage/Feature Contracts

- `cli-contract.md` — Serial + TCP command model (`help`, `status`, `wifi.status`, `time.status`, `mqtt.status`, `mdns.status`, `cli_net.status`, `http.status`)
- `wifi-lifecycle.md` — Stage5 Wi-Fi behavior and event model
- `ntp.md` — Stage6 SNTP state machine and `time.status`
- `mdns.md` — Stage7 hostname + TXT records + `mdns.status`
- `mqtt.md` — Stage9 publish-only telemetry + `mqtt.status`, `mqtt.pubtest`
- `http.md` — Stage10 HTTPS-only API + `http.status`
- `architecture.md` — Stage gating and direct orchestration model
- `nvs-schema.md` — NVS namespaces and key defaults
- `hardware-map.md` — pin map and relay/button electrical assumptions

## Transport and command matrix (authoritative)

- Serial CLI: `LUCE_STAGE >= 4`
- TCP CLI: `LUCE_STAGE >= 8` (read-only command subset + AUTH)
- HTTPS CLI/API: `LUCE_STAGE >= 10` (read-only API surface)

## Evidence references

- Evidence index (latest anchor): `docs/work/diag/evidence/20260222_221921/90_summary.md`
- Governance scorecard pointer: `docs/governance/compliance/scorecard-current.md`

## Deprecation notes

- `docs/user/CLI.md` is a launcher page; canonical contract is `docs/user/cli-contract.md`.
- `docs/dev/hardware-map.md` is a historical reference copy; canonical map is `docs/user/hardware-map.md`.
