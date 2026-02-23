# LUCE Architecture Overview

Date: 2026-02-23

## Canonical model

LUCE is a single-module ESP-IDF firmware with compile-time stage gating. Stage behavior is selected via `LUCE_STAGE` in PlatformIO and gates features using `LUCE_HAS_*` macros in `include/luce_build.h`.

Direct tasks stay in `app_main` and startup call paths. Runtime logic is in feature blocks guarded by preprocessor conditions, including:

- Stage0/1: boot + NVS base initialization.
- Stage2: I2C probe and diagnostics.
- Stage3: LCD.
- Stage4: serial CLI and diagnostics task.
- Stage5: Wi-Fi manager + credentials from NVS.
- Stage6: SNTP time sync service.
- Stage7: mDNS advertise.
- Stage8: TCP read-only CLI transport.
- Stage9: MQTT status publish telemetry.
- Stage10: HTTPS read-only API.

## Feature gating

- `LUCE_HAS_NVS`: `LUCE_STAGE >= 1`
- `LUCE_HAS_I2C`: `LUCE_STAGE >= 2`
- `LUCE_HAS_LCD`: `LUCE_STAGE >= 3`
- `LUCE_HAS_CLI`: `LUCE_STAGE >= 4`
- `LUCE_HAS_WIFI`: `LUCE_STAGE >= 5`
- `LUCE_HAS_NTP`: `LUCE_STAGE >= 6`
- `LUCE_HAS_MDNS`: `LUCE_STAGE == 7 || LUCE_STAGE == 8`
- `LUCE_HAS_TCP_CLI`: `LUCE_STAGE >= 8`
- `LUCE_HAS_MQTT`: `LUCE_STAGE >= 9`
- `LUCE_HAS_HTTP`: `LUCE_STAGE >= 10`

## Deterministic bring-up

- Startup logs emit stage number, build metadata, reset reason and feature flags.
- Stage blocks are initialized in deterministic order in `app_main` with periodic watchdog-friendly loops.
- Network services are started only when their stage conditions and config allow.

## State ownership

- Relay/button masks are maintained in global runtime state variables in `src/main.cpp` and updated only through safe helper updates (`set_relay_mask_safe`, `set_relay_mask`) and explicit command handlers.
- Runtime service states are stored in small structs per subsystem (`WifiStatusSnapshot`, `NtpRuntimeState`, `MqttRuntime`, `HttpRuntime`).

## Failure model and observability

- Boot logs include:
  - reset reason
  - heap free/min-free
  - feature flags
  - subsystem lifecycle transitions
- Network and CLI operations log structured reason/transition lines:
  - `[WIFI][LIFECYCLE]`
  - `[NTP][LIFECYCLE]`
  - `[mDNS]`
  - `[MQTT]`
  - `[HTTP]`
  - `[CLI_NET]`
- Heap and stack watermark diagnostics are emitted in debug modes and periodically in CLI paths.

## Verification

- Evidence: `docs/work/diag/evidence/20260222_211814/90_summary.md`
- Evidence SHA: `ecd0768b22d41e07df8b1f025a0416c4e0f753c8`

