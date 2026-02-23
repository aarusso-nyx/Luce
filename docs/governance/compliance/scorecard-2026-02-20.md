# Scorecard - 2026-02-20

- nci: 100
- rating: PASS
- scoring_source: docs/governance/compliance/scoring.md
- health_source: docs/governance/health/build-status.md
- audit_files_count: 9
- evidence_path: docs/work/diag/evidence/20260222_211814
- local_evidence_policy: evidence under docs/work/diag is local only; docs reference paths only

## Inputs

- docs/governance/audit/README.md
- docs/governance/audit/governance-structure-report.md
- docs/governance/audit/structure-conformance.md
- docs/governance/audit/code-docs.md
- docs/governance/audit/code-tests.md
- docs/governance/audit/docs-tests.md
- docs/governance/compliance/scoring.md

## PASS/FAIL/UNAVAILABLE table

- lint/static: PASS_WITH_WAIVER
- build matrix: PASS
- unit: PASS
- upload: PASS
- e2e: PASS
- notes: native matrix sub-entry for test image generation remains constrained in one toolchain path.

## Summary

- evidence_root: docs/work/diag/evidence/20260222_211814/
- git_sha: ecd0768b22d41e07df8b1f025a0416c4e0f753c8
- stage coverage: stage0..stage10 build PASS
- boot coverage: stage0 and stage10 PASS
- CLI command coverage: serial command pathway PASS

## Stage evidence

- status: PASS_WITH_LIMITATIONS
- evidence: docs/work/diag/evidence/20260222_211814/90_summary.md
- notes: comprehensive stage matrix and serial e2e proof captured; remaining gaps are tooling and environment constraints.

## Top known issues

1. Linting remains on waiver path (`PASS_WITH_WAIVER`).
2. `luce_test_native` matrix entry has known non-native compile constraints.
3. Network-enabled e2e on stage8/9/10 depends on valid runtime environment.
4. Stage8 read-only transport allowlist and lockout scenarios need dedicated coverage script.
5. `docs/work/tooling` missing is still flagged in one structure snapshot.

## Blockers

- none

