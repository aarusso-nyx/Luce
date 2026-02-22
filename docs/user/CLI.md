# Luce CLI (Stage4+)

This document is the authoritative CLI contract for LUCE stage4+ firmware.

The serial CLI on UART0 (`LUCE_STAGE=4+`) remains the primary deterministic interface.

## Transport
- UART0 at 115200 8N1
- ASCII commands terminated by CR/LF
- Commands are parsed from the boot firmware console input

## Commands
- `help`
  - Prints command list and usage.
- `status`
  - Prints reset reason, uptime, heap stats, task watermark, and runtime feature flags.
  - In stage4 (LUCE_HAS_I2C), prints I2C/mcp availability and masks.
- `nvs_dump`
  - Dumps available NVS namespace/key/type/value entries.
  - Supported only when NVS is compiled in.
- `i2c_scan`
  - Ensures I2C is initialized and performs a bus scan over 0x08..0x77.
  - Prints detected addresses and MCP23017/LCD presence.
- `mcp_read <gpioa|gpiob>`
  - Reads MCP23017 GPIOA or GPIOB register and prints raw hex value.
- `relay_set <0..7> <0|1>`
  - Sets one relay channel.
  - Channel value maps to MCP relay output register with explicit polarity from firmware build.
- `relay_mask <hex>`
  - Writes MCP GPIOA as an 8-bit raw hex mask (0x00..0xFF).
- `buttons`
  - Reads and prints MCP GPIOB.
- `lcd_print <text>`
  - Writes a single LCD line (row 0) when LCD is compiled in and present.
- `reboot`
  - Triggers an ESP reset.
- `time.status` *(stage6+)*
  - Prints SNTP sync state, last successful sync Unix time, sync age, UTC string.
  - Prints "time not synced" with an explicit reason when no valid time is available.

### Stage6 additions
- `time.status` reads `[NTP]` state and validates bounded retry behavior without blocking boot.

## Parsing and diagnostics
- Every command is logged with parsed arguments and result codes.
- Error conditions (invalid args, unavailable subsystem, driver errors) print explicit warnings.
- Empty lines are ignored.
- Command processing is periodic and non-blocking in its own task.

## Stage4 behavior at runtime
- `luce_stage4` starts:
  - diagnostic task (`run_stage2_diagnostics`) for I2C/MCP operations
  - UART0 CLI task
  - `blink_alive` LED task
- Stage0/Stage1/Stage2/Stage3 behavior remains unchanged from their stage definitions.
