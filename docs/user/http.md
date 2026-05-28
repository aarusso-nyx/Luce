# LUCE HTTP API + Captive Portal (NET1)

Date: 2026-03-01

## Scope

NET1 provides a TLS-protected HTTPS API surface (state + control routes) and a captive portal UI served from `./data/webapp` on plain HTTP port `80`. The HTTPS server reads its certificate and private key from NVS at boot; if either is absent or too large the server logs an explicit refusal and HTTPS stays unavailable. Use `tls.status` (CLI) or `/api/info`'s `http_state`/`https_running`/`tls_status`/`tls_last_error`/`cert_present`/`key_present` fields to diagnose.

## NVS schema (`http`)

- `http/enabled` (u8, default `0`)
- `http/port` (u16, default `443`) for HTTPS API
- `http/token` (string, required for protected routes)
- `http/tls_dev_mode` (u8, default `0`)
  - Surfaced as `tls_mode = "dev" | "prod"` in `/api/info` and CLI `tls.status`.
  - This flag does not provision or generate certificates. Both modes require `cert_pem` and `key_pem` in NVS before HTTPS can start.
- `http/cert_pem` (string, PEM-encoded X.509 certificate, max ~2 KB)
- `http/key_pem` (string, PEM-encoded private key, max ~2 KB)
  - Both PEMs are loaded at boot, never logged (only their presence is reported), and never returned by any API endpoint.
  - Values larger than the runtime PEM buffer are rejected and reported as `tls_material_oversized`; they are not truncated or logged.

## Certificate Provisioning Boundary

This firmware does not include runtime HTTPS certificate provisioning. There is no bundled dev certificate, no `tls.clear` command, and no HTTP API that writes cert/key material.

Only `http/cert_pem` + `http/key_pem` already present in NVS unlock HTTPS.
Recommended provisioning remains out-of-band by baking an NVS partition before flashing:

### Bake into the NVS partition before flashing

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
    generate /tmp/luce_nvs.csv /tmp/luce_nvs.bin 0x6000

esptool.py --chip esp32 --port /dev/cu.usbserial-0001 write_flash 0x9000 /tmp/luce_nvs.bin
```

The cert+key live only on the unit being flashed. After reboot, `tls.status`
should report `cert_present=1 key_present=1`. The `0x6000` size matches the
repository's `nvs` partition in `partitions.csv`.

## Endpoints

- `GET /api/health`
  - minimal public health and service state
- `GET /api/info`
  - requires bearer token in `Authorization: Bearer <token>`
  - includes compatibility fields for legacy consumers:
    - identity/build: `name`, `version`, `strategy`, `sha`, `build`, `uptimeMs`, `uptime_s`
    - relay/sensor: `relays`, `nightMask`, `day`, `threshold`, `light`, `temperature`, `humidity`, `sensor_ok`
    - network: `wifi_ip` and nested `network.{ip,wifiConnected,mqttConnected,ntpSynced}`
    - current HTTP flags: `http_enabled`, `http_port`, `http_state`, `https_running`
    - compatibility TLS flag: `tls` (numeric alias of `tls_dev_mode`; not an "HTTPS is running" flag)
    - TLS material status: `tls_dev_mode` (bool), `tls_mode` ("dev"/"prod"), `tls_status`, `tls_last_error`, `cert_present` (bool), `key_present` (bool)
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
- HTTPS refuses to start when cert/key material is missing, unreadable, or larger than the configured PEM buffers.
- Captive portal starts on plain HTTP port `80` when enabled, unless `http/port` is also `80`.
- Starts/stops with Wi-Fi state transitions.

## Smoke checks

- Run `./scripts/http_api_smoke.sh --host https://<device-ip> --token <http-token>` to verify:
  - GET endpoints for health/info/version/ota.
  - POST/PUT `/api/ota/check` accepted.
  - unsupported methods return `405`.
  - unauthenticated protected endpoints return `401` (unless `--skip-unauth`).
  - The smoke script uses `curl -k`; it validates endpoint behavior, not CA trust or hostname identity.
- Or run the wrapper: `./scripts/luce.sh http-smoke --host https://<device-ip> --token <http-token>`.

## Logging

- `[HTTP]` logs for API and portal startup.
- `[HTTP][CAPTIVE]` logs for portal startup and request fallbacks.
- `tls.status` reports `https_running`, `tls_status`, `last_error`, and cert/key presence/byte counts without logging PEM bodies.
