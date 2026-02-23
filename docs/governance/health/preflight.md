# Preflight

- timestamp: 2026-02-23T21:57:39Z
- status: PASS
- latest_scorecard: docs/governance/compliance/scorecard-2026-02-20.md
- nci: 100
- min_nci: 75
- override_used: no
- allowed_work_scope: full

## Evidence policy

- Governance evidence artifacts are produced locally under `docs/work/diag/evidence/<timestamp>/`.
- Evidence under `docs/work/diag/` is intentionally not committed.
- Governance docs must reference evidence paths and local SHA in their verification sections.

## Required evidence commands

- Lint/static:
  - `scripts/lint.sh`
  - `script output` in `docs/work/diag/evidence/<timestamp>/10_lint/`
- Build matrix:
  - `python3 -m platformio run -e luce_stage0`
  - `python3 -m platformio run -e luce_stage1`
  - `python3 -m platformio run -e luce_stage2`
  - `python3 -m platformio run -e luce_stage3`
  - `python3 -m platformio run -e luce_stage4`
  - `python3 -m platformio run -e luce_stage5`
  - `python3 -m platformio run -e luce_stage6`
  - `python3 -m platformio run -e luce_stage7`
  - `python3 -m platformio run -e luce_stage8`
  - `python3 -m platformio run -e luce_stage9`
  - `python3 -m platformio run -e luce_stage10`
  - `scripts` output in `docs/work/diag/evidence/<timestamp>/20_build/`
- Unit tests:
  - `pio test -e luce_test_native`
  - output in `docs/work/diag/evidence/<timestamp>/30_unit/`
- Boot/e2e (if hardware available):
  - `scripts/collect_logs.sh luce_stage0 manual 60`
  - `scripts/collect_logs.sh luce_stage10 manual 60`
  - optional HTTPS/TCP scripts in corresponding stage plans
  - outputs in `docs/work/diag/evidence/<timestamp>/50_boot/` and `docs/work/diag/evidence/<timestamp>/60_e2e/`

## Violated rules

- `docs/work/governance/audit/structure-conformance.md` includes historical missing-dir note for `docs/work/tooling`; accepted as non-blocking until backlog item is closed.

## Required user action

- close remaining known issues in `docs/work/plan/103_governance_backlog.md`
