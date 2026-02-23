# LUCE CLI Contract

Date: 2026-02-23

## Transport and format

Serial CLI is available when `LUCE_STAGE >= 4`.
Commands are UTF-8 text lines ending in CR/LF over UART0 at 115200.
Commands are parsed from whitespace-separated tokens.

Stage8 adds read-only TCP transport:
- Listener `LUCE_STAGE >= 8`
- Default port: `2323`
- First command sequence requires `AUTH <token>`
- Command execution is command-line compatible with serial parser subset

## Common commands

- `help`
- `status`
- `nvs_dump`
- `i2c_scan`
- `mcp_read <gpioa|gpiob>`
- `relay_set <0..7> <0|1>`
- `relay_mask <hex>`
- `buttons`
- `lcd_print <text>`
- `reboot`
- `wifi.status` (Stage5+)
- `wifi.scan` (Stage5+)
- `time.status` (Stage6+)
- `mdns.status` (Stage7+)
- `mqtt.status` (Stage9+)
- `mqtt.pubtest` (Stage9+)
- `http.status` (Stage10+)
- `cli_net.status` (Stage8+)
- `sensors` (supported as unavailable placeholder in current firmware)

## Parsing and errors

- `parse_u32_with_base` is used for numeric arguments.
- Missing arguments return parser usage logs and non-zero return code.
- Unrecognized commands print help and error code.
- `relay_set` rejects channel out-of-range and non-binary values.
- `relay_mask` rejects values above `0xFF`.

## Networking command policy (Stage8+)

- Serial CLI remains full read/write.
- TCP CLI enforces read-only execution list:
  - `help`, `status`, `wifi.status`, `time.status`, `mdns.status`, `i2c_scan`, `mcp_read`, `buttons`, `sensors`, `cli_net.status`, `http.status`
- Mutating commands are rejected in remote sessions and logged as denied.

## TCP auth and session

- `AUTH <token>` required before command execution.
- Max auth failures: 3 before disconnect.
- Idle timeout default is configurable from `cli_net/idle_timeout_s`.
- Session emits command response lines including `cmd=<name> rc=<code>`.

## Verification

- Evidence: `docs/work/diag/evidence/20260222_211814/90_summary.md`
- Evidence SHA: `ecd0768b22d41e07df8b1f025a0416c4e0f753c8`

