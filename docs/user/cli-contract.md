# LUCE CLI Contract

Date: 2026-02-28

## Transport and format

Serial CLI is always available (baseline).
Commands are UTF-8 text lines ending in CR/LF over UART0 at 115200.
Commands are parsed from whitespace-separated tokens.

NET0 adds read-only TCP transport:
- Listener `LUCE_NET_CORE=1`
- Default port: `2323`
- First command sequence requires `AUTH <token>`
- Command execution is command-line compatible with the serial parser subset

## Common commands

- `help`
- `status`
- `nvs_dump`
- `i2c_scan`
- `mcp_read <gpioa|gpiob>`
- `relay_set <0..7> <0|1>`
- `relay_mask <hex>`
- `led_set <0..2> <auto|off|on|blink|fast|slow|flash>`
- `led_clear <0..2|all>`
- `led_status`
- `buttons`
- `lcd_print <text>`
- `reboot`
- `wifi.status` (NET0+)
- `wifi.scan` (NET0+)
- `time.status` (NET0+)
- `mdns.status` (NET0+)
- `mqtt.status` (`LUCE_NET_MQTT=1`)
- `mqtt.pubtest` (`LUCE_NET_MQTT=1`)
- `http.status` (`LUCE_NET_HTTP=1`)
- `cli_net.status` (NET0+)
- `sensors` (active alias for `sensor` snapshot behavior in current firmware)

## Parsing and errors

- `parse_u32_with_base` is used for numeric arguments.
- Missing arguments return parser usage logs and non-zero return code.
- Unrecognized commands print help and error code.
- `relay_set` rejects channel out-of-range and non-binary values.
- `relay_mask` rejects values above `0xFF`.

## Networking command policy (NET0+)

- Serial CLI remains full read/write.
- TCP CLI enforces read-only execution list:
  - `help`, `status`, `wifi.status`, `time.status`, `mdns.status`, `i2c_scan`, `mcp_read`, `buttons`, `sensors`, `http.status`
- Mutating commands are rejected in remote sessions and logged as denied.

## TCP auth and session

- `AUTH <token>` required before command execution.
- Max auth failures: 3 before disconnect.
- Idle timeout is configurable from `cli_net/idle_timeout_s`.
- Session emits wire responses as `AUTH ...`, `OK`, `ERR`, and `DENIED`/`DENIED cmd=<name>`.

## Verification

- Evidence: `docs/work/diag/evidence/20260222_221921/90_summary.md`
- Evidence SHA: `2a3b9df`
