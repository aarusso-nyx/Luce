# Stage10 Plan: HTTPS-Only Read-Only Server

Date: 2026-02-22
Status: Completed

## Scope and constraints

- Add HTTPS server support in `luce_stage10` with `-DLUCE_STAGE=10`.
- Read-only HTTP interface only; no write endpoints.
- No insecure HTTP server mode.
- No additional stage2+ services beyond stage10 scope.

## State machine

- `DISABLED`
  - `http/enabled = 0` (or namespace missing), log `[HTTP] enabled=0`.
- `INIT`
  - Config loaded, Wi-Fi ready, TLS context prepared.
- `READY`
  - Waits for valid IP (`Wi-Fi` + `GOT_IP` event).
- `STARTED`
  - TLS server running with URI handlers attached.
- `FAILED`
  - Binding/startup/registration failures; logs and continues safe fallback to disabled-like behavior.

## Endpoint contract

- `GET /api/health`
  - Public endpoint.
  - Minimal response:
    - `service`, `stage`, `status`, `build`, `sha`, `uptime_s`
  - Must not require auth.
- `GET /api/info`
  - Auth protected.
  - Response:
    - `service`, `stage`, `wifi_ip`, `http_enabled`, `http_port`, `tls`.
- `GET /api/state`
  - Auth protected.
  - Response:
    - `state`, `wifi_ip`, `relay`, `buttons`, `ntp_state`, `ntp_unix`, `ntp_age_s`, `ntp_utc` (if NTP compiled), `requests`, `unauth`.

## Auth policy

- `Authorization` header scheme:
  - `Bearer <token>` where token from `http/token`.
- Missing/invalid auth returns JSON error with `401` or `403`.
- Failure events are logged as `[HTTP] auth fail ...`.

## TLS policy

- Stage10 ships with embedded self-signed PEM cert/key for bring-up.
- `http/tls_dev_mode` exists as config knob for future certificate provisioning.
- No insecure HTTP fallback is present.

## Logging requirements

- Must emit:
  - `[HTTP] enabled=<0|1>`.
  - `[HTTP] state=<state> reason=<reason>`.
  - `[HTTP] started` / `[HTTP] stopped`.
  - `[HTTP] route=/api/health, /api/info, /api/state`.
  - `[HTTP] method=... path=... status=... remote=... duration_ms=...`.
- Requests with bodies larger than `kHttpBodyLimitBytes` return `413`.

## Runtime/defense constraints

- No dynamic heavy allocation in request path beyond fixed-size buffers.
- Deterministic response ordering.
- Stable JSON key ordering in all responses.
- Request path is strictly `GET` only.

## Retry policy

- No reconnect retry policy (server lifecycle is event-driven from Wi-Fi).
- If startup fails:
  - state transitions to `FAILED`.
  - runtime continues and attempts start on next `GOT_IP` if applicable.

## Acceptance criteria

- `pio run -e luce_stage10` succeeds.
- TLS startup is stable from boot with Wi-Fi connected.
- No command-line API mutations are accepted from HTTP.
- `cli` command `http.status` present and deterministic.
- Logs show disabled path by default when `http/enabled=0`.

## Evidence artifacts (required)

- `docs/work/diag/20260222_204547/10_lint/lint_luce_stage10.txt`
- `docs/work/diag/20260222_204547/20_build/build_luce_stage10.txt`
- `docs/work/diag/20260222_204547/30_unit/unit_native.txt`
- `docs/work/diag/evidence/20260222_204547/40_upload/upload_luce_stage10.txt` (if hardware available)
- `docs/work/diag/evidence/20260222_204547/50_boot/luce_stage0_boot.txt`
- `docs/work/diag/evidence/20260222_204547/50_boot/luce_stage10_boot.txt`
- `docs/work/diag/evidence/20260222_204547/60_e2e/luce_stage10_cli_http_status.txt`
- `docs/work/diag/template_stage10_expected_boot.txt`
- `docs/work/diag/template_stage10_cli_time_status.txt`
- `docs/work/diag/evidence/20260222_204547/90_summary.md`

## Gate status (20260222_204547)

- build/static/unit/upload evidence captured (PASS, lint has low-severity findings only).
- CLI-only `http.status` evidence captured (PASS).
- HTTPS endpoint/network transcript remains limited pending enabled-and-networked validation (`http/enabled=1`).
