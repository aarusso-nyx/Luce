# LUCE Architecture Overview

Date: 2026-02-28

## Canonical model

LUCE is a single-module ESP-IDF firmware with compile-time strategy gating. Strategy is selected via `LUCE_STRATEGY` in PlatformIO and gate macros in `include/luce_build.h`:

- `CORE`: minimal base build (NVS, I2C, LCD, CLI)
- `NET0`: CORE + Wi-Fi + NTP + mDNS + TCP CLI
- `NET1`: NET0 + MQTT + HTTP

Runtime initialization remains deterministic in `app_main` and includes service startups in increasing stack dependency order.

## Feature gating

- `LUCE_HAS_NVS` (`CORE`): `LUCE_STRATEGY >= LUCE_STRATEGY_CORE`
- `LUCE_HAS_I2C` (`CORE`): `LUCE_STRATEGY >= LUCE_STRATEGY_CORE`
- `LUCE_HAS_LCD` (`CORE`): `LUCE_STRATEGY >= LUCE_STRATEGY_CORE`
- `LUCE_HAS_CLI` (`CORE`): `LUCE_STRATEGY >= LUCE_STRATEGY_CORE`
- `LUCE_HAS_WIFI` (`NET0+`): `LUCE_STRATEGY >= LUCE_STRATEGY_NET0`
- `LUCE_HAS_NTP` (`NET0+`): `LUCE_STRATEGY >= LUCE_STRATEGY_NET0`
- `LUCE_HAS_MDNS` (`NET0+`): `LUCE_STRATEGY >= LUCE_STRATEGY_NET0`
- `LUCE_HAS_TCP_CLI` (`NET0+`): `LUCE_STRATEGY >= LUCE_STRATEGY_NET0`
- `LUCE_HAS_MQTT` (`NET1+`): `LUCE_STRATEGY >= LUCE_STRATEGY_NET1`
- `LUCE_HAS_HTTP` (`NET1+`): `LUCE_STRATEGY >= LUCE_STRATEGY_NET1`

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
