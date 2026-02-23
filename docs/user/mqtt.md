# LUCE MQTT (Stage9)

Date: 2026-02-23

## Role

Publish-only telemetry client with TLS optional and deterministic reconnect behavior.

## NVS schema (`mqtt`)

- `mqtt/enabled` (u8, default `0`)
- `mqtt/uri` (string, default `mqtt://localhost:1883`)
- `mqtt/client_id` (string, optional)
- `mqtt/base_topic` (string, default `luce/stage9`)
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
- No subscriptions are created in Stage9 implementation.
- Telemetry payload includes basic firmware, network, relay/button state and timestamp fields.

## CLI

- `mqtt.status` prints connected state, counters, URI summary, and last publish fields.
- `mqtt.pubtest` publishes one test message and logs return code.

## Security

- Passwords are masked in logs.
- TLS mode is selected by URI + `mqtt/tls_enabled`.
- CA loading path is logged from config source.

## Verification

- Evidence: `docs/work/diag/evidence/20260222_214039/90_summary.md`
- Evidence SHA: `2a3b9df`
