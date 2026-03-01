# LUCE MQTT (NET1)

Date: 2026-02-28

## Role

MQTT client with inbound control subscriptions and outbound telemetry/compatibility aliases.

## NVS schema (`mqtt`)

- `mqtt/enabled` (u8, default `0`)
- `mqtt/uri` (string, default `mqtt://localhost:1883`)
- `mqtt/client_id` (string, optional)
- `mqtt/base_topic` (string, default `luce/net1`)
- `mqtt/username` (string, optional)
- `mqtt/password` (string, optional)
- `mqtt/tls_enabled` (u8, default `0`)
- `mqtt/ca_pem_source` (string: `embedded`, `nvs`, `partition`, `none`)
- `mqtt/qos` (u32, 0..2)
- `mqtt/keepalive_s` (u32)

## Runtime behavior

- Enabled only when `mqtt/enabled = 1`.
- If no IP yet, service remains in backoff state.
- Connect/reconnect with exponential backoff.
- Subscribes to inbound control topics on connect (base topic):
  - `config/#`
  - `relays/#`
  - `sensor/#`
  - `leds/#`
- Telemetry payload includes basic firmware, network, relay/button state and timestamp fields.

Supported inbound behavior:
- `relays/state` (`uint8_t` bitmask) sets relay states.
- `relays/state/<idx>` (`0|1` or `off|on`) sets relay index `idx`.
- `config/*` accepts selected runtime options and stores them in NVS for next reboot:
  - `name`, `hostname`
  - `wifi/ssid`, `wifi/pass`
  - `mqtt`, `mqtt/uri`, `mqtt/client_id`, `mqtt/base_topic`, `mqtt/username`, `mqtt/password`,
    `mqtt/tls_enabled`, `mqtt/ca_pem_source`, `mqtt/qos`, `mqtt/keepalive_s`
  - `mdns/instance`
  - `http/token`
  - `cli_net/token`
- `sensor/threshold` stores `sensor/threshold` in NVS and updates live relay scheduling.
- `config/*` supports legacy compatibility aliases and keeps legacy keys for future migration:
  - `config/name`, `config/hostname`, `config/ssid`, `config/ssid2`, `config/pass`, `config/pass2`,
    `config/wifi/ssid`, `config/wifi/pass`,
  - `config/mqtt`, `config/mqtt/*`,
  - `config/log*` logger compatibility fields (stored in `compat` namespace).
- `relays/night` and `relays/night/<idx>` persist night masks into NVS and apply relay day/night suppression policy immediately.
- `leds/state` and `leds/state/<idx>` support command + readback semantics:
  - `leds/state`:
    - empty payload -> publish current status mask
    - payload `0..7` -> set manual LED overrides for indexes `0..2` from bitmask
    - payload `auto|off|on|blink|fast|slow|flash` -> apply mode to all LED indexes `0..2`
  - `leds/state/<idx>` (`idx=0..2`):
    - empty payload -> publish that LED current state
    - payload `0|1|on|off|true|false|auto|blink|fast|slow|flash` -> set mode for index

Publishes legacy compatibility aliases under the base topic:
  - `sensor/lighting`
  - `sensor/voltage`
  - `sensor/temperature`
  - `sensor/humidity`
- `relays/state`
  - `relays/state/<idx>`

Additional: `leds/state` and `leds/state/<idx>` can be queried as compatibility read-back topics.

Unsupported legacy control topics now produce deterministic compatibility responses under:
- `compat/unsupported`
  - JSON payload:
    - `status` = `unsupported`
    - `topic` = inbound topic suffix that was rejected
    - `reason` = deterministic rejection reason
    - `payload_present` = whether a payload was provided

## CLI

- `mqtt.status` prints connected state, counters, URI summary, and last publish fields.
- `mqtt.pubtest` publishes one test message and logs return code.

## Security

- Passwords are masked in logs.
- TLS mode is selected by URI + `mqtt/tls_enabled`.
- CA loading path is logged from config source.

## Verification

- Evidence: `docs/work/diag/evidence/20260222_221921/90_summary.md`
- Evidence SHA: `2a3b9df`
