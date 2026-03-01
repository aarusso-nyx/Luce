# Stage Plan 010

Date: 2026-02-22
Status: Active

## Run Procedure

- Use `./scripts/collect_logs.sh <env> <tag> <duration_seconds>`.
- Store logs under `docs/work/diag/<date>_<env>_<tag>.txt`.
- Use `LUCE_STRATEGY` to validate expected compiled feature set per env.

## Stage0 - Hardware Bring-Up
Acceptance criteria:
- Power-on is stable on USB power.
- UART boot log is visible and includes:
  - `LUCE STAGE0`
  - build timestamp
  - reset reason
- Console shows chip/clock/heap diagnostics.
- LEDs 25/26/27 execute the alive sequence.
- No dependency symbols for NVS/I2C/LCD/CLI in `luce_core` build.
- Stage0 run for 30 seconds with no watchdog-reset due to task starvation.

## Stage1 - I/O Baseline
Acceptance criteria:
- Stage1 firmware builds with `luce_core` and reports `LUCE STAGE1`.
- `esp_nvs` is initialized and no Stage1 image includes I2C/LCD/CLI symbols.
- Boot state record is persisted every boot:
  - `boot_count` increments on each restart.
  - `last_reset_reason` is updated with current `esp_reset_reason`.
- Boot log prints verbose NVS section:
  - `namespace`, `key`, `type`, and stored value (or readback marker where feasible).
- Device and app diagnostics are present:
  - partition table dump
- Reset/reflash acceptance check:
  - `python3 -m platformio run -e luce_core -t erase` (or erase tool equivalent), rebuild, flash, then confirm `boot_count` changes on next boot.

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
- `luce_core` run shows one-time relay safety initialization (`all relays OFF` at startup message).

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
- Stage2 I2C/MCP sweep path remains functional under `luce_core`.
- A CLI unavailable check must still hold: `LUCE_HAS_CLI=0` in Stage3.

## Stage4 - Resilience And Release Readiness
Acceptance criteria:
- `luce_core` exists with `-DLUCE_STRATEGY=CORE`.
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
- `nvs_dump`, `i2c_scan`, and control commands are optional-by-gate (`LUCE_HAS_NVS`, `LUCE_HAS_I2C`) with explicit warnings when disabled.

## Stage5 - Wi-Fi Lifecycle
- Add compile-time gated Wi-Fi lifecycle with bounded backoff and non-rebooting failure handling.
- Network parameters read from NVS namespace `net`:
  - `ssid`, `pass`, `hostname`, `enabled`, `max_retries`, `backoff_min_ms`, `backoff_max_ms`.
- CLI provides read-only network inspection:
  - `wifi.status`, `wifi.scan`.
- Wi-Fi state transitions and snapshots are emitted with `[WIFI]` log tags:
  - `INIT`, `CONNECTING`, `GOT_IP`, `BACKOFF`, `STOPPED`.

## Stage6 - Time Sync
- SNTP service compiles with `LUCE_HAS_NTP` and starts from Stage6 runtime order.
- Runtime sync state is explicit and command-visible via `time.status`.
- NTP sync is logged as lifecycle state transitions and failures; no hard failure loops.
- `luce_build.h` stage gates remain source-of-truth for include decisions.

## Stage7 - mDNS + Discovery
- Add optional mDNS publish on `_luce._tcp` when Wi-Fi IP is present and `mdns/enabled=1`.
- Continue direct-task startup style; no service manager introduced.
- Add CLI/diagnostic line:
  - `mdns.status`.
- Guard all mDNS code with `LUCE_HAS_MDNS`.

## Stage8 - TCP CLI
- Add TCP CLI transport behind `LUCE_HAS_TCP_CLI` (Stage8+).
- Keep UART0 CLI behavior unchanged.
- TCP session exposes same read-only command set used by serial CLI.
- Add lifecycle logging for listener/task/session lifecycle.

## Stage9 - MQTT Publish Telemetry
- Add publish-only MQTT status telemetry with bounded reconnect backoff.
- No topic subscriptions in this stage.
- Add command surface:
  - `mqtt.status`, `mqtt.pubtest`.
- Gate config with `LUCE_HAS_MQTT`, defaults in `mqtt` NVS namespace.

## Stage10 - HTTPS Read-only API
- Add read-only HTTPS surface behind `LUCE_HAS_HTTP`.
- Keep write operations disabled/unsupported in this stage.
- Endpoints: `/api/health`, `/api/info`, `/api/state` with bearer token checks where required.
- Add `[HTTP]` lifecycle/request diagnostics and explicit startup disabled-by-default evidence.

## Next-Stage Completion Plan
- Stage order for next execution: 7 → 8 → 9 → 10.
- For each stage boundary, require:
  - matrix build for every environment in `platformio.ini` (or at least through new stage),
  - boot evidence capture on `/dev/cu.usbserial-40110`,
  - CLI/TCP/MQTT/HTTP command evidence matching feature surface,
  - scorecard update in `docs/governance/compliance/`.
