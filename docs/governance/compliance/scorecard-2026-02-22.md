# Scorecard - 2026-02-22

- nci: 100
- rating: PASS
- scoring_source: docs/governance/compliance/scoring.md
- health_source: docs/governance/health/build-status.md
- audit_files_count: 9

## Static Check Notes

- lint: PASS (non-blocking low-severity findings only)
- lint_command: `pio check -e <luce_stageN> --skip-packages`
- lint_envs: `luce_stage0`, `luce_stage1`, `luce_stage2`, `luce_stage3`, `luce_stage4`, `luce_stage5`, `luce_stage6`, `luce_stage7`, `luce_stage8`, `luce_stage9`, `luce_stage10`
- lint_status: `luce_stage0..10=PASS` (with informational low-severity findings in stage5)
- lint_waiver_policy: none; findings are low-severity warnings only
- lint_evidence: `docs/work/diag/20260222_204547/10_lint/`

## Stage6 Evidence

- status: PASS
- evidence: docs/work/diag/evidence/20260222_180112/90_summary.md
- notes: stage6 run is complete with compile/build/unit/upload/boot/e2e evidence and non-blocking NTP `time.status` command.

## Stage7 Evidence

- status: PASS
- evidence: docs/work/diag/evidence/20260222_183903/90_summary.md
- notes: stage7 adds optional mDNS advertisement with `mdns/status` CLI; runtime evidence includes disabled-by-default behavior and discovery commands.

## Stage8 Evidence

- status: PREREQ_MISSING
- evidence: docs/work/diag/evidence/20260222_214039/90_summary.md
- notes: stage8 adds read-only TCP CLI (`nc`) transport, disabled-by-default baseline, and AUTH-protected session handling. Stage8 transport e2e is `UNAVAILABLE` in this run due no network credentials in onboard NVS.

## Stage9 Evidence

- status: PASS
- evidence: docs/work/diag/20260222_195556/90_summary.md
- notes: stage9 adds publish-only MQTT telemetry with `mqtt.status` + `mqtt.pubtest`, bounded reconnect behavior, and non-subscription policy. Build matrix through stage9 now passes after decoupling stage9 from mDNS-responder-only dependencies.

## Stage10 Evidence

- status: PREREQ_MISSING
- evidence: docs/work/diag/evidence/20260222_214039/90_summary.md
- notes: stage10 adds HTTPS-only, read-only endpoints (`/api/health`, `/api/info`, `/api/state`) with bearer token on protected routes, compile-time gating via `LUCE_STAGE=10`, and disabled-by-default startup evidence. Current run is CLI-only (`http/enabled=0`); HTTPS endpoint transcript is pending `http/enabled=1` with network-enabled validation.

## Modularization Slice S0 Evidence

- status: PASS
- evidence: docs/work/diag/evidence/20260222_214039/90_summary.md
- notes: build matrix (stage0..stage10), lint (PASS_WITH_WAIVER), native unit tests PASS, stage0/stage10 boot captures, stage10 serial CLI transcript captured after refactor slice merge.

## Inputs

- docs/governance/audit/README.md
- docs/governance/audit/code-docs.md
- docs/governance/audit/code-tests.md
- docs/governance/audit/docs-tests.md
- docs/governance/audit/governance-chain-status.md
- docs/governance/audit/governance-structure-report.md
- docs/governance/audit/prompt-scope-loader-check.md
- docs/governance/audit/structure-conformance.md
- docs/governance/audit/npm-applicability.md

## Blockers

- none
