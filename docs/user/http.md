# LUCE HTTP API + Captive Portal (NET1)

Date: 2026-03-01

## Scope

NET1 provides a TLS-protected HTTPS API surface (state + control routes) and a captive portal UI served from `./data/webapp` on plain HTTP port `80`. The HTTPS server reads its certificate and private key from NVS at boot; if either is absent the server logs an explicit `[HTTP] start refused: missing TLS material` error and stays in the `FAILED` state. Use `tls.status` (CLI) or `/api/info`'s `tls_mode`/`cert_present`/`key_present` fields to diagnose.

## NVS schema (`http`)

- `http/enabled` (u8, default `0`)
- `http/port` (u16, default `443`) for HTTPS API
- `http/token` (string, required for protected routes)
- `http/tls_dev_mode` (u8, default `0`)
  - `1` = dev mode; if NVS has no `cert_pem`/`key_pem` and the firmware was built with `tools/dev_cert.pem` + `tools/dev_key.pem` present, the build-embedded dev cert is used as a fallback.
  - `0` = production mode; cert+key must be provisioned in NVS — no fallback.
  - Surfaced as `tls_mode = "dev" | "prod"` in `/api/info` and CLI `tls.status`. The `cert_source` field on the same surfaces distinguishes `"nvs"`, `"dev-fallback"`, and `"none"`.
- `http/cert_pem` (string, PEM-encoded X.509 certificate, max ~2 KB)
- `http/key_pem` (string, PEM-encoded private key, max ~2 KB)
  - Both PEMs are loaded at boot, never logged (only their presence is reported), and never returned by any API endpoint. Provisioning is currently manual (out-of-band NVS write); a follow-up PR adds a serial CLI provisioning command.

## Dev-mode certificate workflow

For bench testing and lab fleets, generate a shared self-signed cert+key
locally and let the build embed them:

```bash
./tools/gen_dev_cert.sh                  # default CN/SAN luce-dev.local
./tools/gen_dev_cert.sh luce-bench.local # custom CN/SAN
```

This writes `tools/dev_cert.pem` and `tools/dev_key.pem` (both
git-ignored). Rebuild the firmware; CMake xxd-embeds the pair as the
`luce_dev_cert_pem` / `luce_dev_key_pem` symbols, and dev-mode firmware
uses them as a fallback when NVS has no cert+key.

The dev cert is **shared across all units built from the same
`tools/dev_*.pem` pair**. It does not carry per-device identity. Re-run
the script to rotate. Set `LUCE_FORBID_DEV_CERT=1` in the build
environment to refuse to build if the dev cert files exist (use this on
CI/release pipelines).

## Production-mode cert provisioning

Production firmware (`tls_dev_mode=0`) refuses to use the build-embedded
dev fallback — only `http/cert_pem` + `http/key_pem` written to NVS will
unlock HTTPS. There are two supported provisioning paths today:

### A. Bake into the NVS partition before flashing (recommended)

Use the IDF NVS partition generator to produce a binary image that
already contains the cert+key, then flash it as the `nvs` partition
alongside the firmware. The cert+key never traverse a runtime channel.

```bash
# Build a CSV with the http namespace populated:
cat > /tmp/luce_nvs.csv <<'CSV'
key,type,encoding,value
http,namespace,,
enabled,data,u8,1
port,data,u16,443
tls_dev_mode,data,u8,0
token,data,string,"<bearer-token-here>"
cert_pem,file,string,/abs/path/to/cert.pem
key_pem,file,string,/abs/path/to/key.pem
CSV

python3 ${IDF_PATH}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate /tmp/luce_nvs.csv /tmp/luce_nvs.bin 0x4000

esptool.py --chip esp32 --port /dev/cu.usbserial-0001 write_flash 0x9000 /tmp/luce_nvs.bin
```

The cert+key live only on the unit being flashed.

### B. Runtime overwrite via the serial CLI (lab/bench)

For a unit that is already in the field, the path is:

1. Connect serial monitor with an authenticated session.
2. Run `tls.clear` — wipes any existing NVS cert+key. Confirm via
   `tls.status` that `cert_present=0`.
3. Use the IDF runtime NVS commands (`idf.py monitor` + Ctrl-T to drop
   into the IDF console) to `nvs_set http:cert_pem str "<pem>"` and
   `nvs_set http:key_pem str "<pem>"`. This requires the IDF NVS
   console component, which is *not* compiled in to Luce by default;
   the bake-and-flash path (A) is the supported workflow.
4. Reboot via `reboot`. Boot diagnostics should report
   `source=nvs cert_present=1 key_present=1`.

`tls.clear` is the only TLS write-side CLI command shipped today. A
future PR may add a captive-portal HTTP POST endpoint for runtime
provisioning behind a one-time setup token; until then, NVS partition
bake is the canonical production path.

## Endpoints

- `GET /api/health`
  - minimal public health and service state
- `GET /api/info`
  - requires bearer token in `Authorization: Bearer <token>`
  - includes compatibility fields for legacy consumers:
    - identity/build: `name`, `version`, `strategy`, `sha`, `build`, `uptimeMs`, `uptime_s`
    - relay/sensor: `relays`, `nightMask`, `day`, `threshold`, `light`, `temperature`, `humidity`, `sensor_ok`
    - network: `wifi_ip` and nested `network.{ip,wifiConnected,mqttConnected,ntpSynced}`
    - current HTTP flags: `http_enabled`, `http_port`, `tls`
    - TLS material status: `tls_mode` ("dev"/"prod"), `cert_present` (bool), `key_present` (bool)
- `GET /api/state`
  - requires bearer token
- `GET /api/ota`
  - requires bearer token
- `POST /api/ota/check`
  - requires bearer token; optional URL from query/body:
    - query `url=<https://...>` takes precedence when provided and non-empty
    - otherwise plain-text body payload is used when non-empty
    - when neither source provides a URL, queued check runs with configured/default source
  - response payload includes:
    - `status` = `queued`
    - `source` = `query|body|default` (effective URL source resolution)
- `PUT /api/ota/check`
  - alias of POST behavior for update checks; requires bearer token
- `GET /api/version`
  - returns firmware version/identity details
- `GET /api/leds/state`
  - requires bearer token; returns current LED state and manual override masks
- `PUT /api/leds/state`
  - requires bearer token; command LED manual overrides:
    - body/query `value=0..7` sets manual state mask for LEDs `0..2`
    - body/query `value=auto|off|on|blink|fast|slow|flash` applies mode to all LEDs `0..2`
- `GET /api/leds/state/0` (same for `/1`, `/2`)
  - requires bearer token; returns current state for selected LED index and manual value (`null` when auto mode)
- `PUT /api/leds/state/0` (same for `/1`, `/2`)
  - requires bearer token; command selected LED index:
    - body/query `value=0|1|on|off|true|false|auto|blink|fast|slow|flash` sets mode for that index
- `GET /ws`
  - websocket endpoint (available on HTTPS API server and captive HTTP server)
  - handshake sends an immediate state snapshot
  - server pushes periodic state snapshots with relay/night/sensor fields (`type`, `tstamp`, `state`, `night`, `day`, `threshold`, `light`, `voltage`, `temperature`, `humidity`, `sensor_ok`)
- `GET /`
  - serves `./data/webapp/index.html`
- `GET /index.html`
- `GET /app.css`
- `GET /script.js`
  - all unresolved paths on captive HTTP fall back to `index.html` for SPA-style navigation

## Runtime behavior

- Starts only when Wi-Fi IP is present and config is enabled.
- HTTPS API server starts on `http/port` (default `443`) and registers `api/*` endpoints.
- Captive portal starts on plain HTTP port `80` when enabled, unless `http/port` is also `80`.
- Starts/stops with Wi-Fi state transitions.

## Smoke checks

- Run `./scripts/http_api_smoke.sh --host https://<device-ip> --token <http-token>` to verify:
  - GET endpoints for health/info/version/ota.
  - POST/PUT `/api/ota/check` accepted.
  - unsupported methods return `405`.
  - unauthenticated protected endpoints return `401` (unless `--skip-unauth`).
- Or run the wrapper: `./scripts/luce.sh http-smoke --host https://<device-ip> --token <http-token>`.

## Logging

- `[HTTP]` logs for API and portal startup.
- `[HTTP][CAPTIVE]` logs for portal startup and request fallbacks.
