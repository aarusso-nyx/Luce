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

## Commands

The serial CLI exposes the following commands. Each row lists whether the
command mutates state (`mut`) and whether it is allowed over the TCP CLI
read-only allowlist (`tcp_ro`). All read-write commands remain available on
the serial transport regardless of `tcp_ro`.

### Baseline (always present)

| Command | mut | tcp_ro | Usage |
| --- | --- | --- | --- |
| `help` | no | yes | `help` |
| `status` | no | yes | `status` |
| `version` | no | yes | `version` |
| `info` | no | yes | `info` |
| `wakeup` | no | yes | `wakeup` |
| `uptime` | no | yes | `uptime` |
| `system` | no | yes | `system` |
| `state` | no | yes | `state` |
| `parts` | no | yes | `parts` |
| `free` | no | yes | `free` |
| `nvs` | no | yes | `nvs` |
| `nvs_dump` | no | no  | `nvs_dump` |
| `sensor` | no | yes | `sensor [<interval_s> <count>]` |
| `sensors` | no | yes | `sensors` (snapshot alias of `sensor`) |
| `set` | yes | no  | `set <relay\|mask\|led> <ids>=<on\|off>` |
| `log` | yes | no  | `log [show \| buffer [<size>] \| console [level\|format] [<val>] \| logfile [level\|format] [<val>]]` |
| `logpage` | yes | no  | `logpage <next\|prev\|reset\|show>` |
| `test` | yes | no  | `test` |
| `reset` | yes | no  | `reset` |
| `i2c_scan` | no | yes | `i2c_scan` |
| `mcp_read` | no | yes | `mcp_read <gpioa\|gpiob>` |
| `relay_set` | yes | no  | `relay_set <0..7> <0\|1>` |
| `relay_mask` | yes | no  | `relay_mask <hex>` |
| `led_set` | yes | no  | `led_set <0..2> <auto\|off\|on\|blink\|fast\|slow\|flash>` |
| `led_clear` | yes | no  | `led_clear <0..2\|all>` |
| `led_status` | no | yes | `led_status` |
| `buttons` | no | yes | `buttons` |
| `lcd_print` | no | no  | `lcd_print <text>` |
| `reboot` | yes | no  | `reboot` |
| `pki.status` | no | yes | `pki.status [role]` |
| `pki.keygen` | yes | no | `pki.keygen <role>` |
| `pki.csr` | no | no | `pki.csr <role>` |
| `pki.cert.begin` | yes | no | `pki.cert.begin <role>` |
| `pki.cert.append` | yes | no | `pki.cert.append <role> <pem-line>` |
| `pki.cert.commit` | yes | no | `pki.cert.commit <role>` |
| `pki.reset` | yes | no | `pki.reset <role>` |

`pki.*` manages on-device TLS identities for roles `https_server`,
`mqtt_client`, and `ota_client`. `pki.keygen` creates a device-local EC P-256
private key; `pki.csr` prints only a CSR; `pki.cert.*` imports a CA-signed
certificate and rejects certificates that do not match the local key. `pki.status`
prints role state, material presence, byte counts, fingerprint, subject, issuer,
and last error without printing private-key material.

### NET0+ (`LUCE_NET_CORE=1`)

| Command | mut | tcp_ro | Usage |
| --- | --- | --- | --- |
| `wifi` | no | yes | `wifi` |
| `wifi.status` | no | yes | `wifi.status` |
| `wifi.scan` | no | yes | `wifi.scan` |
| `time.status` | no | yes | `time.status` |
| `mdns.status` | no | yes | `mdns.status` |
| `cli_net.status` | no | no  | `cli_net.status` |

### MQTT (`LUCE_NET_MQTT=1`)

| Command | mut | tcp_ro | Usage |
| --- | --- | --- | --- |
| `mqtt.status` | no | yes | `mqtt.status` |
| `mqtt.pubtest` | yes | no  | `mqtt.pubtest` |

### HTTP (`LUCE_NET_HTTP=1`)

| Command | mut | tcp_ro | Usage |
| --- | --- | --- | --- |
| `http.status` | no | yes | `http.status` |
| `tls.status` | no | yes | `tls.status` |
| `tls.keygen` | yes | no | `tls.keygen` |
| `tls.csr` | no | no | `tls.csr` |
| `tls.cert.begin` | yes | no | `tls.cert.begin` |
| `tls.cert.append` | yes | no | `tls.cert.append <pem-line>` |
| `tls.cert.commit` | yes | no | `tls.cert.commit` |
| `tls.reset` | yes | no | `tls.reset` |

`tls.*` commands are HTTPS compatibility aliases for `pki.* https_server`.
`tls.status` also reports HTTP server startup state, `https_running`,
`tls_status`, and the last TLS startup error reason.

### OTA (`LUCE_NET_OTA=1`)

| Command | mut | tcp_ro | Usage |
| --- | --- | --- | --- |
| `ota.status` | no | yes | `ota.status` |
| `ota.check` | yes | no  | `ota.check [url]` |

`mqtt.status` and `ota.status` include optional client identity state from the
`mqtt_client` and `ota_client` PKI roles.

## Parsing and errors

- `parse_u32_with_base` is used for numeric arguments.
- Missing arguments return parser usage logs and non-zero return code.
- Unrecognized commands print help and error code.
- `relay_set` rejects channel out-of-range and non-binary values.
- `relay_mask` rejects values above `0xFF`.

## Networking command policy (NET0+)

- Serial CLI remains full read/write.
- TCP CLI enforces a read-only allowlist; the canonical set is the
  `tcp_ro=yes` rows in the command tables above.
- Mutating commands are rejected in remote sessions and logged as denied.
- `nvs_dump` and `cli_net.status` are read-only but intentionally excluded
  from TCP to keep secrets and session state off the wire.

## TCP auth and session

- `AUTH <token>` required before command execution.
- Max auth failures: 3 before disconnect.
- Idle timeout is configurable from `cli_net/idle_timeout_s`.
- Session emits wire responses as `AUTH ...`, `OK`, `ERR`, and `DENIED`/`DENIED cmd=<name>`.

## Verification

- Command-table alignment can be checked from source/docs without hardware.
- Runtime evidence should be regenerated under `docs/work/diag/` with `./scripts/luce.sh test --layers boot,serial --env net1 --boot-duration 45` when hardware is attached.
