# HTTP API (Canonical)

Base path: `/`.
Content type: `application/json` unless noted.

## Auth Policy

- Read endpoints are open by default on LAN.
- Mutating endpoints require auth token when `security.require_http_auth=1`.
- Send token in one of:
  - `Authorization: Bearer <token>`
  - `X-Auth-Token: <token>`

## GET Endpoints

- `GET /api/version`
- `GET /api/info`
- `GET /api/config`
- `GET /api/features`
- `GET /api/health`
- `GET /api/relays`
- `GET /api/sensor`
- `GET /api/logs?limit=<N>`
- `GET /api/events` (SSE live stream)

## PUT Endpoints

- `PUT /api/relays?state=<0..255>`
- `PUT /api/relays?night=<0..255>`
- `PUT /api/sensor/threshold?threshold=<0..4095>`
- `PUT /api/features/<http|mqtt|telnet|ota|sessionlog>?enabled=<0|1>`

## POST Endpoints

- `POST /api/ota?url=<https-url-optional>`

## Static Hosting

- `GET /` -> `/storage/index.html`
- `GET /*` -> `/storage/*`

## Canonical Error Envelope

```json
{
  "error": {
    "code": "unauthorized",
    "message": "missing or invalid auth token",
    "requestId": "123"
  }
}
```
