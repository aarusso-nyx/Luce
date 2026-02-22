# Scorecard - 2026-02-22

- nci: 100
- rating: PASS
- scoring_source: docs/governance/compliance/scoring.md
- health_source: docs/governance/health/build-status.md
- audit_files_count: 9

## Static Check Notes

- lint: PASS (with explicit waiver policy)
- lint_command: `python3 -m platformio check -e <luce_stageN> --skip-packages`
- lint_envs: `luce_stage0, luce_stage1, luce_stage2, luce_stage3, luce_stage4`
- lint_status: `luce_stage0=PASS`, `luce_stage1..4=PASS_WITH_WAIVER`
- lint_waiver_policy: explicit medium-severity cppcheck waiver in `scripts/lint_waivers.txt`
- lint_evidence: `docs/work/diag/20260222_162104/lint/`

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
