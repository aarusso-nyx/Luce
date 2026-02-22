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
- LCD is optional and detected on I2C address `0x27`.
- I2C backpack initializes with a small 4-bit HD44780-like sequence.
- Stage3 displays continuously (every few seconds):
  - `LUCE S3 <uptime>`
  - `I2C:ok MCP:ok/no`
  - `REL:0xNN`
  - `BTN:0xNN`
- Identical status is mirrored to console output.
- Missing LCD does not prevent running; diagnostics continue with warnings.
- Stage2 I2C/MCP sweep path remains functional under `luce_stage3`.

## Stage4 - Resilience And Release Readiness
Acceptance criteria:
- `luce_stage4` exists with `-DLUCE_STAGE=4`.
- CLI compiles only in stage4 (`LUCE_HAS_CLI` compile-time gate) and does not enable networking code paths.
- `help` command lists at least:
  - `help`, `status`, `nvs_dump`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `buttons`, `lcd_print`, `reboot`.
- `status` command prints reset reason, uptime, heap (free/min), and stack watermark.
- `nvs_dump` command prints namespace/key/type/value where available.
- `i2c_scan` prints detected I2C addresses and presence of MCP/LCD.
- `mcp_read gpioa|gpiob` and `buttons` report MCP register values.
- `relay_set` validates arguments and writes channel bit safely.
- `relay_mask` accepts hex payload and writes MCP GPIOA register.
- `lcd_print` writes text when LCD is enabled, and prints unavailable warning otherwise.
- `reboot` command triggers clean software reset.
- Stage4 CLI and diagnostics tasks run together (no blocking stage_main) and each command logs parsed args and result status.
