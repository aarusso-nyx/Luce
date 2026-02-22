# Stage Plan 010 (Skeleton)

Date: 2026-02-22
Status: Draft skeleton

## Stage0 - Hardware Bring-Up
Acceptance criteria:
- Board powers reliably on expected supply rails.
- UART console accessible and stable.
- I2C bus initializes on GPIO23/GPIO22 without lockups.
- MCP23017 detected at address `0x20`.
- DHT and LDR channels produce plausible raw readings.

## Stage1 - I/O Baseline
Acceptance criteria:
- J1 relay outputs map correctly to MCP `A0..A7`.
- J2 button inputs map correctly to MCP `B0..B7`.
- Button press/release polarity and debounce behavior are validated.
- MCP interrupt path to ESP32 GPIO19 is verified.

## Stage2 - Control Semantics
Acceptance criteria:
- Physical button authority semantics are enforced.
- Relay safe defaults and fail-safe behavior are explicit and tested.
- Local control loops remain deterministic under rapid input changes.
- Event/log output is sufficient for fault triage.

## Stage3 - Interface Integration
Acceptance criteria:
- CLI controls relay/sensor workflows correctly.
- HTTP and MQTT use canonical routes/topics only.
- State changes are consistent across CLI/HTTP/MQTT surfaces.
- Documentation in `doc/` matches observed behavior.

## Stage4 - Resilience And Release Readiness
Acceptance criteria:
- Cold boot/reboot recovers to known-safe operational state.
- Sensor/IO fault paths degrade safely and emit diagnostics.
- Preflight and governance evidence artifacts are current and PASS.
- Release checklist completed with rollback path documented.
