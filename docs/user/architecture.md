# LUCE Architecture Overview

Date: 2026-02-28

## Canonical model

LUCE is a single-module ESP-IDF firmware with compile-time feature gating. Build flags are selected by environment in `platformio.ini`:

- `CORE`: minimal base build (NVS, I2C, LCD, CLI)
- `NET0`: CORE + Wi-Fi + NTP + mDNS + TCP CLI
- `NET1`: NET0 + MQTT + HTTP

Runtime initialization remains deterministic in `app_main` and includes service startups in increasing stack dependency order.

## Feature gating

- `CORE` always includes:
  - NVS
  - I2C
  - LCD
  - CLI
- `LUCE_HAS_WIFI` (`NET0+`): `LUCE_NET_CORE=1`
- `LUCE_HAS_NTP` (`NET0+`): `LUCE_NET_CORE=1`
- `LUCE_HAS_MDNS` (`NET0+`): `LUCE_NET_CORE=1`
- `LUCE_HAS_TCP_CLI` (`NET0+`): `LUCE_NET_CORE=1`
- `LUCE_HAS_MQTT` (`MQTT`): `LUCE_NET_MQTT=1`
- `LUCE_HAS_HTTP` (`HTTP`): `LUCE_NET_HTTP=1`

## Observability model

Startup logs include strategy name, reset reason, and feature flags.

Network services are started only when their dependency state is available.

## State ownership

- Relay/button masks are maintained in `src/main.cpp` and updated through explicit helper updates.
- Runtime service states are stored in per-subsystem runtime structs.

## Verification

Evidence for this model is linked in:
- `docs/work/diag/evidence/20260222_221921/90_summary.md`
- `docs/governance/compliance/scorecard-current.md`
