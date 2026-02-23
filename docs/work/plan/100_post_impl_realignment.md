# Post-Implementation Governance Realignment

Date: 2026-02-23
Status: Completed documentation/gov update draft; commit pending
Evidence folder: `docs/work/diag/evidence/20260222_211814/`
Evidence SHA used for narrative: `ecd0768b22d41e07df8b1f025a0416c4e0f753c8`

## Phase A preconditions

1. AGENTS and `platformio.ini` are present at repository root.
2. Working tree was not clean due pre-existing tooling artifacts from prior skill runs (`.agent/tmp/...`, docs/governance working artifacts); this pass intentionally scoped to docs/governance outputs.
3. No firmware code changes were made in this phase.

## Skills executed (in order)

1. `agents-preflight`
2. `health-evidence-capture`
3. `governance-structure-audit`
4. `verify-adherence`
5. `compliance-scorecard`
6. `health-gate`
7. `compliance-gate`

## Pass / fail / unavailable snapshot (from local evidence)

- Source: `docs/work/diag/evidence/20260222_211814/90_summary.md`
- Lint/static: `PASS_WITH_WAIVER`
- Build matrix: `PASS` (stages 0..10)
- Unit tests: `PASS` (8 tests)
- Build-native test matrix entry: `FAIL` for `luce_test_native` image generation path
- Upload/boot: `PASS` (stage0 and stage10 captured)
- E2E: `PASS` (serial CLI transcript captured)
- Hardware status: upload ports `/dev/tty.usbserial-0001`, `/dev/tty.usbserial-40110`; serial capture command `scripts/collect_logs.sh`.

## Docs promoted or updated

New authoritative docs under `/doc` are now present:
1. `doc/architecture.md`
2. `doc/cli-contract.md`
3. `doc/wifi-lifecycle.md`
4. `doc/ntp.md`
5. `doc/mdns.md`
6. `doc/mqtt.md`
7. `doc/http.md`
8. `doc/nvs-schema.md`
9. `doc/hardware-map.md`
Updated governance docs:
10. `docs/governance/health/preflight.md`
11. `docs/governance/compliance/scorecard-2026-02-20.md`

## Issues remaining

1. `PASS_WITH_WAIVER` lint status currently depends on `scripts/lint_waivers.txt` and must be revisited if strict clean lint is required.
2. Stage matrix capture includes a native test-entry failure in non-native toolchain path (`luce_test_native` build step).
3. `docs/work/tooling` is still reported missing in one structure audit snapshot path check.
4. Stage8 network e2e availability is not guaranteed by the latest evidence set and should be treated as conditional on available WLAN config.
5. `docs/work/inv/hardware-map.md` remains under `docs/dev`; canonical hardware map is represented in the new `doc/hardware-map.md`.

## Backlog seed references

Detailed remediation backlog is in `docs/work/plan/103_governance_backlog.md` with required evidence paths.

## Required next action

- Merge this alignment with a single post-implementation commit once governance files are accepted.
- Re-run a fresh `health-evidence-capture` after backlog closure for a new evidence folder.
