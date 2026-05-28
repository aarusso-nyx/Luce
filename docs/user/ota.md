# LUCE OTA (NET1)

Date: 2026-05-28

## Scope

OTA is compiled into `net1` with `LUCE_NET_OTA=1`. It is disabled by default and starts only when the `ota` NVS namespace enables it.

## NVS schema (`ota`)

- `ota/enabled` (u8, default `0`)
- `ota/url` (string, default empty)
- `ota/ca_pem_source` (string, default `nvs`; currently the only supported CA source)
- `ota/ca_pem` (string, PEM CA material used for HTTPS verification when source is `nvs`)
- `ota/check_interval_s` (u32, default `0`; `0` disables periodic checks)
- `ota/request_timeout_ms` (u32, default `10000`, clamped by firmware)

## Control surfaces

- Serial CLI:
  - `ota.status`
  - `ota.check [url]`
- HTTP API:
  - `GET /api/ota`
  - `POST /api/ota/check`
  - `PUT /api/ota/check`

HTTP check requests accept an optional URL from query string or plain-text body. Query URL takes precedence over body URL; when neither is provided, the configured `ota/url` is used.

## Runtime behavior

- Missing `ota` namespace disables the service.
- Checks require Wi-Fi IP and an OTA update partition.
- Manual checks are queued from CLI or HTTP and consumed by the OTA task.
- Periodic checks run only when `ota/check_interval_s` is greater than zero.

## Security and completeness notes

- OTA uses ESP-IDF `esp_https_ota`.
- Set `ota/ca_pem_source=nvs` with `ota/ca_pem` to validate the OTA endpoint against the configured CA. Empty or unsupported CA configuration fails the check before download.

## Verification

- Build evidence: `./scripts/luce.sh build --env net1`
- Runtime evidence: `./scripts/luce.sh test --layers boot,http --env net1 --host https://<device-ip> --http-token <token>`
