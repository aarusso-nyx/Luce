# Preflight

- timestamp: 2026-02-22T18:38:56.488Z
- status: PASS
- latest_scorecard: docs/governance/compliance/scorecard-2026-02-22.md
- nci: 100
- min_nci: 75
- override_used: no
- allowed_work_scope: full
- lint_command: python3 -m platformio check -e <luce_stageN> --skip-packages
- lint_envs: luce_stage0, luce_stage1, luce_stage2, luce_stage3, luce_stage4
- lint_evidence: docs/work/diag/20260222_162104/lint
- lint_result: PASS with waiver (stage0 PASS, stage1-4 PASS_WITH_WAIVER)
- lint_waiver_policy: explicit line-level entry in scripts/lint_waivers.txt

## Violated Rules

- none

## Required User Action

- none
