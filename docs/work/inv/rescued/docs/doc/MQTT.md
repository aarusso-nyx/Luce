# MQTT Contract (Canonical)

Base topic defaults to configured hostname (for example `luce`).

## Published Topics

- `<base>/version` (retained)
- `<base>/status/wifi` (retained)
- `<base>/status/isDay` (retained)
- `<base>/relays/desired` (retained)
- `<base>/relays/state` (retained, effective output)
- `<base>/relays/night` (retained)
- `<base>/sensor/threshold` (retained)
- `<base>/sensor/lighting` (telemetry)
- `<base>/sensor/voltage` (telemetry)
- `<base>/sensor/temperature` (telemetry)
- `<base>/sensor/humidity` (telemetry)
- `<base>/ack/<requestId>` (command acknowledgement)

## Subscribed Topics

- `<base>/cmd/reboot`
- `<base>/cmd/reset`
- `<base>/cmd/ota/start`
- `<base>/cmd/verbosity`
- `<base>/relays/state`
- `<base>/relays/state/<idx>`
- `<base>/relays/night`
- `<base>/sensor/threshold`
- `<base>/feature/<http|mqtt|telnet|ota|sessionlog>`

## Payload Rules

- Integer fields use decimal text.
- Toggle fields use `0` or `1`.
- `cmd/ota/start` payload may contain URL override.
- If `security.require_http_auth=1`, mutating payloads must be prefixed:
  `token=<token>;<value>`

## Acknowledgement Payload

`<base>/ack/<requestId>` publishes:

```json
{ "requestId": 42, "ok": 1, "message": "queued" }
```

## Notes

- No `/v2` prefix exists in canonical topics.
- No compatibility shims are provided.
