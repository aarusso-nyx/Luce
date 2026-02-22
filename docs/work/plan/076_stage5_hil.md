# Stage5 HIL Test Plan

Date: 2026-02-22
Status: Active
Owner: Luce firmware team

## Scope

This stage validates Wi‑Fi lifecycle behavior added in Stage5 (`LUCE_STAGE=5`) on bench hardware only.  
The tests are serial-only and do not use networking services beyond station connect/disconnect.

Environment constraints:

- Firmware stage: `luce_stage5` from latest commit
- Board: NodeMCU-32S (ESP32)
- Upload port: `/dev/cu.usbserial-0001`
- Monitor port: `/dev/cu.usbserial-40110` (timestamped monitor preferred)

## Router preconditions

- AP SSID and pass match NVS keys:
  - `wifi/ssid`
  - `wifi/pass`
- Router must provide DHCP on the subnet.
- Router security must be WPA2 or WPA2/WPA3 transition (firmware currently requests `WIFI_AUTH_WPA2_PSK`).
- Channel can be 1-11 (2.4GHz recommended), 802.11n/ac enabled.
- Keep RSSI stable during baseline test; if possible keep client within ~5m.
- DHCP lease and DNS can be on/off; only IP acquisition is required.
- For disconnect test:
  - either disable radio on router, or
  - power-cycle router, or
  - temporarily blacklist the test SSID from clients.

## Test sequence

1. Configure NVS (if needed) to match bench AP:
   - `wifi/enabled=1`
   - `wifi/ssid=<bench_ssid>`
   - `wifi/pass=<bench_pass>`
   - `wifi/max_retries=6`
   - `wifi/backoff_min_ms=250`
   - `wifi/backoff_max_ms=8000`
2. Build and flash:
   - `pio run -e luce_stage5 -t upload --upload-port /dev/cu.usbserial-0001`
3. Start monitor with timestamps:
   - `pio device monitor -p /dev/cu.usbserial-40110 --filter time`
4. Execute and capture CLI checks:
   - `wifi.status`
   - `wifi.scan`
   - wait for connect
   - `wifi.status`
5. Induce disconnect and capture backoff:
   - disable router Wi‑Fi for 20–40 seconds or power-cycle AP
   - observe logs and call `wifi.status`
6. Restore router and capture reconnect:
   - re-enable Wi‑Fi
   - call `wifi.status` repeatedly until reconnection

## Expected logs for connect success

At or shortly after reboot, expected signatures (minimum):

- `LUCE STAGE5`
- `[WIFI][NVS] key=ssid ...`
- `[WIFI][NVS] config summary ...`
- `[WIFI] stack initialized`
- `[WIFI][LIFECYCLE] state=INIT ...`
- `[WIFI][LIFECYCLE] state=CONNECTING ...`
- `WIFI_EVENT_STA_CONNECTED` handling reflected by `[WIFI][LIFECYCLE] state=CONNECTING` or transition context
- `[WIFI][LIFECYCLE] state=GOT_IP ...`
- `[WIFI][IP] got_ip=<ip> gw=<gw> mask=<mask>` (if logged by IP event path in current build)
- `CLI command wifi.status ...` and printed `[WIFI][STATUS] state=GOT_IP ...`

## Expected logs for disconnect/retry

When AP disappears:

- `[WIFI][LIFECYCLE] state=BACKOFF ...`
- `[WIFI][BACKOFF] next_ms=<n> attempt=<n> max=<max_retries>`
- repeating `[WIFI][STATUS] state=BACKOFF ...` during the backoff window
- optional repeated `state=CONNECTING` followed by `state=BACKOFF` until link reappears

After AP returns:

- `[WIFI][LIFECYCLE] state=CONNECTING ...`
- eventual `WIFI_EVENT_STA_CONNECTED` path and `[WIFI][LIFECYCLE] state=GOT_IP`  
- `[WIFI][STATUS]` showing `ip != 0.0.0.0`

## `wifi.status` checkpoints

Run and record with timestamps:

1. On fresh boot (first 30 s)
2. 5 s after first disconnect event
3. Just before router recovers
4. Immediately after reconnect success

Each response should include:

- state
- attempt
- backoff values
- ip/gw/mask
- rssi if available

## Deterministic evidence outputs

Create a timestamp directory:
- `docs/work/diag/evidence/<ts>/`

Required files:

- `boot_stage5.txt`:
  - upload + first monitor segment (first 80 lines)
  - expected signature block for connect success
- `wifi_disconnect_reconnect.txt`:
  - full disconnect/reconnect transcript
  - includes timestamps and `[WIFI][BACKOFF]` lines
- `cli_wifi_status.txt`:
  - command/output transcript for all `wifi.status` checkpoints

No other files are required for this stage unless you include optional `wifi.scan` transcript.

## Bench checklist

Before pass:

- Ports set to expected values
- NVS `wifi/enabled=1` and credentials loaded
- Boot evidence file present
- `wifi.status` recorded at all 4 checkpoints
- Disconnect/reconnect evidence includes at least one `state=BACKOFF` and one `state=GOT_IP`
