# Security and Authorization

## Threat Surface

Remote control paths include:

- HTTP write routes
- MQTT command topics
- OTA trigger/action path
- Telnet session (optional)

## Security Configuration

Configured through `SecurityConfig` in `store.h`:

- `require_http_auth`
- `http_bearer_token`
- `telnet_require_auth`
- `telnet_password`
- `telnet_max_attempts`
- `telnet_lockout_ms`

## HTTP Authorization

Mutating endpoints are expected to enforce:

- Bearer/header token validation
- Rejection on missing/invalid token
- Structured error response

## CLI Authorization

- Serial CLI uses transport policy only.
- Telnet may require password and supports lockout on repeated failures (counter/time-window based).

## OTA Trust and Integrity

- OTA is enabled by feature flag and policy.
- Trust material fields are persisted in `ota` namespace (`ca_pem_hash` and URL).
- OTA transport should be HTTPS-only per implementation policy.

## Failure Exposure

- Authentication failures are logged.
- Faults are visible through health snapshot and status propagation.
