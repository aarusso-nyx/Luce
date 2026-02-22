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
- Stage1 firmware builds with `luce_stage1` and reports `LUCE STAGE1`.
- `esp_nvs` is initialized and no stage1 build includes I2C/LCD/CLI symbols.
- Boot state record is persisted every boot:
  - `boot_count` increments on each restart.
  - `last_reset_reason` is updated with current `esp_reset_reason`.
- Boot log prints verbose NVS section:
  - `namespace`, `key`, `type`, and stored value (or readback marker where feasible).
- Device and app diagnostics are present:
  - partition table dump
- Reset/reflash acceptance check:
  - NVS erase + rebuild + flash shows reset history changes exactly as expected.

## Stage2 - Control Semantics
Acceptance criteria:
- I2C bus initializes on GPIO22/23 and logs detected 7-bit addresses.
- MCP23017 at `0x20` is detected and configured with:
  - PORT A (`IODIRA`) as outputs for relays `A0..A7`
  - PORT B (`IODIRB`) as inputs for buttons `B0..B7`
  - `GPPUB` set for button pull-ups
  - relays powered off on startup (active polarity defined and logged)
- Relay sweep runs one relay at a time and reports mask changes each step.
- Button sample path prints only transitions after debounce (no per-cycle spam).
- MCP INT line (`GPIO19`) is configured as input and its level is logged on change.
- If MCP is absent, firmware stays alive and reports clear graceful-degrade messages.
- Stage2 build gates prevent I2C path from compiling in Stage0/Stage1.

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
