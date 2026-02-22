# HIL Playbook (LUCE)

Date: 2026-02-22
Purpose: Reproducible bench protocol for stage validation and data capture.

## 1) Scope and Safety

- No field modifications to firmware are made by the playbook; it only validates runtime behavior.
- Keep relay loads disconnected from J1 during initial validation to avoid electrical surprises.
- Start with all Jx connectors disconnected; connect only one interface at a time.

## 2) Power-Up Sequence

1. Power-off DUT and disconnect all external accessories.
2. Connect the desired UART cable to J3:
   - J3 pin 2 (`TX`) -> J3 RX on adapter (to mirror/monitor side)
   - J3 pin 3 (`RX`) -> J3 TX on adapter
   - J3 pin 1 -> GND
3. Connect serial monitor cable to `/dev/cu.usbserial-40110` and upload cable to `/dev/cu.usbserial-0001`.
4. Power-cycle DUT using USB only.
5. Start monitor before upload or immediately after upload with timestamps.
6. Capture first boot lines and record them in the stage run log.

## 3) Connector Wiring

### J1 Relay connector (`1x10`) — relay board

- Pin 1: `+5V`
- Pin 2: `GND`
- Pin 3: CH1 -> MCP `A0`
- Pin 4: CH2 -> MCP `A1`
- Pin 5: CH3 -> MCP `A2`
- Pin 6: CH4 -> MCP `A3`
- Pin 7: CH5 -> MCP `A4`
- Pin 8: CH6 -> MCP `A5`
- Pin 9: CH7 -> MCP `A6`
- Pin 10: CH8 -> MCP `A7`

During Stage2/4 relay verification, start with CH1 only enabled and observe `LUCE` relay mask transitions.

### J2 Button connector (`1x9`) — buttons

- Pin 1: `GND`
- Pin 2: `B1` -> MCP `B0`
- Pin 3: `B2` -> MCP `B1`
- Pin 4: `B3` -> MCP `B2`
- Pin 5: `B4` -> MCP `B3`
- Pin 6: `B5` -> MCP `B4`
- Pin 7: `B6` -> MCP `B5`
- Pin 8: `B7` -> MCP `B6`
- Pin 9: `B8` -> MCP `B7`

For debounce verification, press one button at a time and confirm transition logs.

### J4 I2C connector (`1x4`) — MCP/LCD bus

- Pin 1: `SDA` -> ESP32 `GPIO23`
- Pin 2: `SCL` -> ESP32 `GPIO22`
- Pin 3: `GND`
- Pin 4: `3.3V`

Use this connector when validating I2C scan and LCD behavior.

## 3a) Confirmed hardware assumptions

- Relay outputs (`MCP GPIOA`) are active-LOW: cleared bit is ON (`1` = OFF, `0` = ON).
- MCP button inputs (`MCP GPIOB`) are internally pulled-up and active-LOW: `1` = released/idle, `0` = pressed.
- LCD backpack is fixed at I2C address `0x27` and is 3.3V logic-compatible.
- MCP `ITA` is intentionally unused on this hardware; `ITB` is monitored on `GPIO19`.

### Optional LCD

- Optional `20x4` I2C backpack expected at `0x27` on J4 SDA/SCL.
- If the display is not present, firmware must continue with warning-only behavior.

## 4) What to Observe

### LEDs (GPIO25,26,27)

- All stages run alive pattern while firmware is active:
  - LED25 on briefly, then LED26, then LED27, then all off.
- Missing pattern indicates task/boot stall.

### Serial logs (all stages)

- Upload/build metadata:
  - `LUCE STAGEx`
  - build timestamp, project version, git SHA
  - reset reason and chip info
- Stage-specific markers in section 5.

## 5) Stage Acceptance Signals

### Stage0
- `LUCE STAGE0` present.
- No compile-time NVS/I2C/LCD/CLI markers are required.
- Blink alive pattern confirmed.
- No extra runtime dependency crashes after 30 seconds.

### Stage1
- Stage1 build boot shows NVS boot state updates:
  - `boot_count` increments per boot.
  - `last_reset_reason` updated.
- `NVS entries` dump prints discovered namespace/key/type/value when available.

### Stage2
- I2C bus scan enumerates addresses when hardware is attached.
- MCP at `0x20` is logged as expected when present.
- Relay sweep changes one channel at a time and prints masks.
- Button transitions log on change only, not every sample.
- INTB level messages are observable or missing with explicit degraded warning if MCP absent.

### Stage3
- LCD auto-detection at `0x27` attempted.
- If detected, status lines appear:
  - `LUCE S3`
  - `I2C:ok MCP:ok/no`
  - `REL:0xNN`
  - `BTN:0xNN`
- If missing, console confirms warning and runtime still proceeds.

### Stage4
- CLI reachable on UART0.
- Commands execute and print return codes:
  - `status`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `buttons`, `nvs_dump`, `lcd_print`, `reboot`.
- `help` outputs all required commands.
- No networking command set in this firmware image.

## 6) Reproducible Capture

- Use `scripts/collect_logs.sh` for each run:
  - `scripts/collect_logs.sh <env> <tag> <duration_seconds>`
  - Example: `./scripts/collect_logs.sh luce_stage4 stage4-smoke 180`
- Logs are stored as `docs/work/diag/<timestamp>/boot/<env>_<tag>.txt`.

## 7) How to capture boot evidence

Use `scripts/capture_serial.py` when `pio device monitor` cannot stay stable in CI/non-interactive shells.

Examples:
- Capture one boot line window (stage0):
  - `mkdir -p docs/work/diag/$(date +%Y%m%d_%H%M%S)/boot`
  - `python3 scripts/capture_serial.py --port /dev/cu.usbserial-40110 --baud 115200 --seconds 20 --output docs/work/diag/20260222_boot_stage0.txt`
- Run a full runner-style capture with upload + fixed serial window:
  - `source ~/.zshrc && ./scripts/collect_logs.sh luce_stage4 stage4_boot 60 --upload-port /dev/cu.usbserial-0001 --monitor-port /dev/cu.usbserial-40110`
- Optional wrapper for deterministic output filenames:
  - `source ~/.zshrc && ./scripts/collect_logs.sh luce_stage0 stage0_boot 30 --upload-port /dev/cu.usbserial-0001 --monitor-port /dev/cu.usbserial-40110`

Capture script behavior:
- Reads bytes for the requested duration and writes to the requested file.
- Returns non-zero on port open/read setup failures.
