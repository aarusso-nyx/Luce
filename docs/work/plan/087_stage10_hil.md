# Stage10 HIL Plan: HTTPS Read-Only API

Date: 2026-02-22

Status: Baseline captured (disabled-by-default CLI path)

## Hardware

- ESP32 with `luce_stage10` firmware.
- Serial monitor/USB to capture boot and lifecycle lines.
- LAN with HTTPS client access to DUT IP.

## Preconditions

- `http/enabled=1` for enabled case (NVS set before capture).
- Optional baseline case with `http/enabled=0`.
- Known DUT IP from serial logs (`wifi.ip`/boot line).

## Test commands

1. Configure NVS keys:
  - `http/enabled`
  - `http/port` (default `443`)
  - `http/token` (for protected endpoints)
2. Capture boot:
  - `scripts/capture_serial.py --port <port> --baud 115200 --seconds 60 --output docs/work/diag/20260222_204547/50_boot/luce_stage10_boot.txt`
3. Verify disabled startup:
  - `http/enabled=0` on DUT
  - verify `[HTTP] enabled=0` and no start message.
4. Enabled startup and endpoint check:
  - start with `http/enabled=1`.
  - `curl -k https://<ip>/api/health`
  - `curl -k -H "Authorization: Bearer <token>" https://<ip>/api/info`
  - `curl -k -H "Authorization: Bearer <token>" https://<ip>/api/state`
  - optional: `curl -k -H "Authorization: Bearer wrong" https://<ip>/api/info` expect `401/403`.

## Output transcript requirements

  - `docs/work/diag/20260222_204547/60_e2e/luce_stage10_cli_http_status.txt`
  - `docs/work/diag/<timestamp>/60_e2e/luce_stage10_hil_curl.txt` *(pending network-enabled validation)*

## Captured evidence

- `docs/work/diag/20260222_204547/50_boot/luce_stage10_boot.txt`
- `docs/work/diag/20260222_204547/60_e2e/luce_stage10_cli_http_status.txt`

## Minimum expected lines in boot transcript

- `Feature flags: ... HTTP=1`
- `[HTTP] enabled=<0|1>`
- `[HTTP] state=<state>`
- `[HTTP] started` (enabled path only)
- `CLI command http.status: ...` when tested.

## Pass criteria

- HTTPS endpoint exists when enabled and returns JSON.
- `curl -k https://<ip>/api/health` succeeds.
- Protected routes reject missing or bad tokens and accept correct token.
- No writes are accepted through HTTP in this stage.
