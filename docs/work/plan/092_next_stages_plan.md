# LUCE Next-Stage Execution Plan

Date: 2026-02-23

## Current State Snapshot
- Evidence run created at `docs/work/diag/evidence/20260223_023906`.
- Stage5/6 runtime is implemented and compiles.
- Stage7-10 environments exist in `platformio.ini` and build cleanly.
- `PIO monitor` is blocked in this non-interactive shell; direct serial capture was used as fallback.

## Priority Targets
- Stage7: mDNS advertise lifecycle.
- Stage8: TCP CLI transport.
- Stage9: MQTT publish telemetry.
- Stage10: HTTPS-only read-only API.

## Stage7: mDNS (compile-only)
- Add `src/mdns.cpp`, `include/luce/mdns.h` with compile-time gate `LUCE_HAS_MDNS`.
- Add NVS namespace `mdns` config keys:
  - `mdns/enabled` (u8, default 0)
  - `mdns/instance` (string, default `Luce Stage`)
- Runtime behavior:
  - start only when Wi-Fi has IP and `LUCE_HAS_WIFI` true
  - start/stop on Wi-Fi state transitions
  - advertise `_luce._tcp` with TXT fields `fw`, `stage`, `device`, `build`
- Logging requirements:
  - `[mDNS]` lines for enabled/config/state/start/failed.
- CLI command:
  - `mdns.status`
- Validation:
  - `pio run -e luce_stage7`
  - `pio run -e luce_stage7 -t size`
  - boot evidence shows mDNS gated by Wi-Fi/`mdns/enabled`

## Stage8: TCP CLI
- Add compile-time transport abstraction in `src/cli_transport_tcp.cpp`, `include/luce/cli_transport_tcp.h` behind `LUCE_HAS_TCP_CLI`.
- Keep UART0 CLI active; TCP CLI adds read-only serializable duplicate command entrypoint.
- Add command namespace to avoid conflict with existing CLI parser:
  - all Stage4 commands remain available.
  - include `cli.status` and `mdns.status` passthrough.
- Networking lifecycle:
  - bind on TCP port 23 by default (or define via NVS `tcp_cli/enabled`, `tcp_cli/port`)
  - only active when Wi-Fi/IP is present
  - close socket and stop server on disconnect/reboot path.
- Logging:
  - `[CLI_NET]` and session accept/close lines.
- Validation:
  - `pio run -e luce_stage8` and size
  - evidence transcript for socket connect and `help`/`status`/`wifi.status`

## Stage9: MQTT
- Add `src/mqtt.cpp`, `include/luce/mqtt.h` behind `LUCE_HAS_MQTT`.
- NVS namespace `mqtt` config:
  - `mqtt/enabled` (u8)
  - `mqtt/uri` (string)
  - `mqtt/client_id`, `mqtt/base_topic`, `mqtt/username`, `mqtt/password`
  - `mqtt/tls_enabled`, `mqtt/ca_pem_source`, `mqtt/qos`, `mqtt/keepalive_s`
- Runtime requirements:
  - no subscriptions (publish-only)
  - bounded reconnect using existing Wi-Fi state transitions
  - publish periodic telemetry + on-demand publish test
- CLI:
  - `mqtt.status`
  - `mqtt.pubtest`
- Logging:
  - `[MQTT]` state/connected/error/pub` lines.
- Validation:
  - `pio run -e luce_stage9` and size
  - e2e script for `mqtt.status` with Wi-Fi disabled path as PASS if logs show skip behavior.

## Stage10: HTTPS Read-only API
- Add `src/http_server.cpp`, `include/luce/http_server.h` behind `LUCE_HAS_HTTP`.
- NVS namespace `http` config:
  - `http/enabled` (u8)
  - `http/port` (u16, default `443`)
  - `http/token` (string)
  - `http/tls_dev_mode` (u8)
- Endpoints:
  - `GET /api/health` (public)
  - `GET /api/info` + `GET /api/state` with bearer token
- Runtime behavior:
  - start only when Wi-Fi/IP present and enabled
  - never mutates state (read-only), no POST/PUT actions.
- Logging:
  - `[HTTP] enabled`, `[HTTP] request` (method/path/status/duration/source)`.
- Validation:
  - `pio run -e luce_stage10` and size
  - scripted GET attempts against `/api/health` and token-gated endpoints

## Cross-Stage Invariants
- Keep compile-time gates in `luce_build.h` as source of truth.
- Preserve direct task model in `src/app_main.cpp`/runtime entry.
- Continue to keep no service-manager/supervisor reducer style; startup is explicit stage blocks.
- Keep docs in `docs/user/*` synced when adding feature contracts.
- Update stage acceptance checklist in `docs/work/plan/010_stage_plan.md` once modules are stable.

## Immediate Next-Step Slices
1. Stage7 implementation + static evidence + boot logs.
2. Stage7 status + Stage8 transport skeleton.
3. Stage8 transport regression + e2e TCP proof.
4. Stage9 publish-only pipeline + config hardening.
5. Stage10 HTTPS API + endpoint authorization evidence.

## Validation commands (mandatory for each finished slice)
- `pio run -e <env>` for all envs covered by that stage (`luce_stage0`..`luce_stage10`)
- `pio run -e <env> -t size`
- `pio test -e luce_test_native`
- if Wi-Fi hardware available: `pio run -e <env> -t upload --upload-port /dev/cu.usbserial-0001`
- boot + monitor transcripts on `/dev/cu.usbserial-40110`
