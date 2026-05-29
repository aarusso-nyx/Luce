# LUCE Architecture Overview

Date: 2026-05-28

## Canonical model

LUCE is a single-module ESP-IDF firmware with compile-time feature gating. Build flags are selected by environment in `platformio.ini`:

- `CORE`: minimal base build (NVS, I2C, LCD, CLI)
- `NET0`: CORE + Wi-Fi + NTP + mDNS + TCP CLI
- `NET1`: NET0 + MQTT + HTTP + OTA

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
- `LUCE_HAS_OTA` (`OTA`): `LUCE_NET_OTA=1`

## Observability model

Startup logs include strategy name, reset reason, and feature flags.

Network services are started only when their dependency state is available.

## State ownership

- Relay/button masks are maintained in `src/i2c_io.cpp` and exposed through the I/O helper API.
- Runtime service states are stored in per-subsystem runtime structs.
- I/O mask state is protected by `g_io_mask_lock`; relay operations use the
  recursive relay operation mutex. Network services read I/O snapshots through
  helper APIs instead of sharing the lock directly.
- PKI is a leaf module. HTTP, MQTT, and OTA may consume PKI material, but PKI
  does not call back into network services while holding NVS state.

## Degraded Mode

If MCP23017 hardware is unavailable, firmware enters hardware-degraded mode.
Relay/button control paths refuse unsafe I/O, while Wi-Fi, TCP CLI, HTTPS, MQTT,
OTA, status JSON, and logs continue to run for diagnostics.

## Verification Model

Pure helper logic is covered by native host tests in `test/host`. Hardware-free
CI runs those tests, docs alignment, and formatting checks. Device behavior
still requires local firmware builds and hardware-backed HIL evidence, with the
committed evidence manifest validated by CI.

## Verification

Fresh evidence for this model should be generated locally under `docs/work/diag/` with:

- `./scripts/luce.sh health`
- `./scripts/luce.sh build --env net1`
- `scripts/run_host_tests.sh`
- `./scripts/luce.sh test --layers boot --env net1 --boot-duration 45` when hardware is attached

Governance status is summarized in `docs/governance/compliance/scorecard-current.md`.
