# AGENTS

## Repository Context
- **Path**: `/Users/aarusso/Documents/PlatformIO/Projects/Luce`
- **Project type**: ESP32 firmware (ESP-IDF) with PlatformIO
- **Primary stack**: C/C++ firmware + PlatformIO + ESP-IDF

## Standards and Layout
- Governance artifacts must stay under:
  - `docs/governance/audit/`
  - `docs/governance/health/`
  - `docs/governance/compliance/`
- Temporary scratch work and investigation artifacts must stay under:
  - `docs/work/inv/`
  - `docs/work/diag/`
  - `docs/work/plan/`
- Product documentation stays in:
  - `doc/` (APIs, protocol contracts, operations, architecture)

## Repository Files to Keep Stable
- `README.md` and `AGENTS.md` must remain at repository root.
- `platformio.ini` defines build/test environments.
- Source and include trees remain canonical:
  - `src/`
  - `include/`
  - `lib/`
  - `test/`
  - `scripts/`

## Required Governance Evidence
- `docs/governance/audit/structure-conformance.md`
- `docs/governance/compliance/scorecard-2026-02-20.md`
- `docs/governance/health/preflight.md`
- Evidence outputs and temporary logs may be written under `docs/work/**`.

See `.codex/system.md` for repository scope and stack-level assumptions.
- NPM security audits and Node dependency checks are excluded for this firmware-only repository unless Node-based tooling is introduced later.
- See `docs/governance/audit/npm-applicability.md` for the explicit firmware-only rationale.

## Contributor Expectations
- Prefer task-oriented FreeRTOS architecture and explicit state ownership.
- Keep physical-input authority semantics explicit in code and docs.
- Avoid introducing legacy compatibility shims when the canonical interface has changed.
- Use canonical HTTP routes and MQTT topics only (no `/v2` aliases).

## Operational Notes
- Keep security and resilience features in sync across CLI/HTTP/MQTT and docs.
- Any plan or intervention touching behavior should include matching doc updates under `doc/`.
- When uncertain, prefer conservative defaults, clear rollback paths, and explicit diagnostics.
