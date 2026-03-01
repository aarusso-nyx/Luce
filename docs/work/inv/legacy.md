# Legacy Rescued Docs Inventory

Scope: `docs/work/inv/rescued/docs/**`  
Last reviewed: 2026-03-01  
Status: **legacy-only**; kept only for historical context.

This file lists only entries that are **not** represented in the current firmware/docs model.

## 1) Network transport and command planes (obsolete)

- `docs/work/inv/rescued/docs/doc/MQTT.md`
  - `<base>/cmd/ota/start` subscription/topic (`lines 24, 36`) describes OTA command handling that no longer exists.
  - `feature/<http|mqtt|telnet|ota|sessionlog>` (`line 30`) uses a command/feature matrix no longer implemented.
- `docs/work/inv/rescued/docs/doc/API.md`
  - `POST /api/ota?url=...` (`line 35`) and `PUT /api/features/...` including `ota` / `sessionlog` (`line 31`) are from a previous release API model.
- `docs/work/inv/rescued/docs/doc/CLI.md`
  - `feature <http|mqtt|telnet|ota|sessionlog> ...` (`line 35`).
  - `ota start [url]` (`line 44`).
- `docs/work/inv/rescued/docs/docs/network-interfaces.md`
  - `GET /api/ota` (`line 14`) and OTA command privilege model (`line 46`).
  - Telnet transport references remain (`lines 37-41`) while current CLI transport model is serial + TCP CLI (non-telnet).
- `docs/work/inv/rescued/docs/doc/OPERATIONS.md`
  - OTA operation sections (`lines 33-36`) describe CLI/HTTP/MQTT OTA triggers not present in current implementation.

## 2) Telnet / auth-token model (obsolete)

- `docs/work/inv/rescued/docs/doc/CLI.md`
  - Telnet availability and telnet password auth (`lines 3,7-9`).
- `docs/work/inv/rescued/docs/doc/NVS_SCHEMA.md`
  - Security keys like `require_http_auth`, `http_bearer_token`, `telnet_require_auth`, `telnet_password`, etc. (`lines 10-12`).
  - CLI keys `serial_enabled`, `telnet_enabled`, `telnet_port`, `auth_mode` (`line 20`).
- `docs/work/inv/rescued/docs/docs/security-and-authentication.md`
  - `SecurityConfig` fields (`lines 16-21`) reference the retired security schema.
  - Telnet auth flow (`lines 10-11`, `33-34`).

## 3) OTA/task architecture and service graph (obsolete)

- `docs/work/inv/rescued/docs/doc/ARCHITECTURE.md`
  - Task graph includes `task_cli` and `task_ota` (`lines 21-23`), plus old service naming.
- `docs/work/inv/rescued/docs/docs/architecture-tasks-queues.md`
  - Named tasks and priorities are from retired architecture (`task_reducer`, `task_command`, `task_cli`, `task_ota`) (`lines 5-16`).
- `docs/work/inv/rescued/docs/docs/events-commanding.md`
  - `task_command`/`task_reducer` command flow names (`lines 63-65`) no longer match source implementation.
- `docs/work/inv/rescued/docs/docs/source-map.md`
  - Service/task modules include `service_manager.h/.cpp`, `srv_cli`, `srv_ota` (`lines 12, 36-37`) that are not in current source map.
- `docs/work/inv/rescued/docs/docs/system-overview.md`
  - `service_manager_init/start`, `service_manager`, `srv_cli`, `srv_ota` references (`lines 18-24`, `32-33`) no longer reflect current startup/task organization.
- `docs/work/inv/rescued/docs/docs/ota-and-recovery.md`
  - OTA worker/service model (`lines 5-8`) including `ServiceId::OTA` is obsolete.

## 4) Storage/session-log model (obsolete)

- `docs/work/inv/rescued/docs/doc/NVS_SCHEMA.md`
  - `ota` namespace with URL/hash/channel (`line 27`) and legacy security keys do not match current settings.
- `docs/work/inv/rescued/docs/doc/README.md`
  - OTA and session logging as runtime features (`lines 13-14`, `20`) no longer present as feature-gated services.
- `docs/work/inv/rescued/docs/doc/OPERATIONS.md`
  - Session log retention settings (`lines 42-43`) and related runtime behavior are removed.
- `docs/work/inv/rescued/docs/docs/storage-and-migrations.md`
  - `ota` namespace defaults and `log` retention/session-file behavior (`lines 17-27`) reference missing config domains.
- `docs/work/inv/rescued/docs/docs/logging-and-observability.md`
  - Session log file lifecycle claims (`lines 17-18`) do not apply to current runtime logging behavior.

## 5) Test and process model (obsolete)

- `docs/work/inv/rescued/docs/doc/testing/TEST_MATRIX.md`
  - Native host, embedded and HIL matrix entries still reference features that are not in current firmware (`lines 3-36`), including OTA-specific contracts (`line 26`) and token parsing expectations (`lines 5,7`).
- `docs/work/inv/rescued/docs/doc/testing/TEST_STRATEGY.md`
  - Test layers `native`, `embedded`, `hil` and file paths (`lines 5-7`, `11-14`, `18`) no longer align with current repository test model.
- `docs/work/inv/rescued/docs/doc/testing/README.md`
  - Three-tier execution and run scripts (`lines 3-7`, `12-19`) are historical and do not match current verification path.
- `docs/work/inv/rescued/docs/doc/testing/HIL_SETUP.md`
  - HIL broker/token workflow and CLI token requirements (`lines 7,13-15`, `21`) target removed test harness assumptions.

## 6) Canonical doc index references (obsolete)

- `docs/work/inv/rescued/docs/docs/README.md`
  - Claiming `docs/work/{inv,diag,plan}` as active temporary artifact paths (`line 31`) is stale since only `inv` and `tooling` remain.
- `docs/work/inv/rescued/docs/docs/build-and-test-notes.md`
  - References to old `doc/testing` artifacts and legacy scorecard filename (`scorecard-2026-02-20.md`) are historical.

