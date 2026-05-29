# LUCE Product Contract Index

Date: 2026-05-28

## Strategy/Feature Contracts

- `cli-contract.md` — Serial + TCP command model (`help`, `status`, `wifi.status`, `time.status`, `mqtt.status`, `mdns.status`, `cli_net.status`, `http.status`)
- `wifi-lifecycle.md` — NET0 behavior and event model
- `ntp.md` — NET0 SNTP state machine and `time.status`
- `mdns.md` — NET0 hostname + TXT records + `mdns.status`
- `mqtt.md` — NET1 control subscriptions + telemetry + compatibility aliases + `mqtt.status`, `mqtt.pubtest`
- `http.md` — NET1 HTTPS-only API + `http.status`
- `ota.md` — NET1 OTA checks through CLI and HTTP API
- `architecture.md` — Strategy gating and direct orchestration model
- `nvs-schema.md` — NVS namespaces and key defaults
- `hardware-map.md` — pin map and relay/button electrical assumptions
- `testing.md` — firmware-only hardware smoke validation (`net1`) + full HIL contract suite

## Transport and command matrix (authoritative)

- Serial CLI: always enabled (baseline)
- TCP CLI: `LUCE_NET_CORE=1` (read-only command subset + `AUTH <token>`)
- HTTPS API: `LUCE_NET_HTTP=1` (JSON API and websocket on TLS server;
  captive webapp on plain HTTP port `80`)

### Serial/TCP CLI commands

The canonical CLI command table is `kCliCommands` in `src/cli.cpp` and is
mirrored in `cli-contract.md`. TCP CLI availability is limited to rows whose
`tcp_ro` value is `yes`; mutating commands remain serial-only.

| Strategy | Commands |
| --- | --- |
| Baseline | `version`, `info`, `wakeup`, `uptime`, `system`, `state`, `nvs`, `free`, `sensor`, `sensors`, `set`, `log`, `logpage`, `test`, `reset`, `parts`, `help`, `status`, `nvs_dump`, `i2c_scan`, `mcp_read`, `relay_set`, `relay_mask`, `led_set`, `led_clear`, `led_status`, `buttons`, `lcd_print`, `reboot`, `wifi`, `wifi.status`, `wifi.scan`, `time.status` |
| NET0 (`LUCE_NET_CORE=1`) | Baseline plus `mdns.status`, `cli_net.status` |
| MQTT (`LUCE_NET_MQTT=1`) | NET0 plus `mqtt.status`, `mqtt.pubtest` |
| HTTP (`LUCE_NET_HTTP=1`) | NET0 plus `http.status`, `tls.status`, `tls.keygen`, `tls.csr`, `tls.cert.begin`, `tls.cert.append`, `tls.cert.commit`, `tls.reset` |
| OTA (`LUCE_NET_OTA=1`) | NET1 plus `ota.status`, `ota.check` |

### HTTPS JSON API routes

The canonical JSON route table is `kJsonApiRoutes` in `src/http_server.cpp`.

| Route | Methods | Auth |
| --- | --- | --- |
| `/api/health` | `GET` | no |
| `/api/info` | `GET` | yes |
| `/api/version` | `GET` | no |
| `/api/state` | `GET` | yes |
| `/api/ota` | `GET` | yes |
| `/api/ota/check` | `POST`, `PUT` | yes |
| `/api/leds/state` | `GET`, `PUT` | yes |
| `/api/leds/state/0` | `GET`, `PUT` | yes |
| `/api/leds/state/1` | `GET`, `PUT` | yes |
| `/api/leds/state/2` | `GET`, `PUT` | yes |

### Websocket and captive routes

| Route | Transport | Methods | Auth |
| --- | --- | --- | --- |
| `/ws` | HTTPS API server and captive HTTP server | `GET` websocket | no bearer-token check in route handler |
| `/`, `/index.html`, `/app.css`, `/script.js` | captive HTTP server | `GET` through wildcard handler | no |
| any other captive HTTP path | captive HTTP server | wildcard fallback to `index.html` | no |

## Evidence references

- Evidence index: regenerate current local evidence under `docs/work/diag/` with `./scripts/luce.sh health`, `./scripts/luce.sh build --env net1`, and hardware-backed `./scripts/luce.sh test --layers boot --env net1 --boot-duration 45`.
- Governance scorecard pointer: `docs/governance/compliance/scorecard-current.md`

## Notes

- Hardware mapping is canonical in `docs/user/hardware-map.md`; engineering
  docs link to that file instead of carrying a second copy.
