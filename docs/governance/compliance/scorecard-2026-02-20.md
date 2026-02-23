# Scorecard - 2026-02-20

- nci: 100
- rating: PASS
- scoring_source: docs/governance/compliance/scoring.md
- health_source: docs/governance/health/build-status.md
- audit_files_count: 9
- evidence_path: docs/work/diag/20260222_214039
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

- lint/static: PASS
- build matrix: PASS
- unit: PASS
- upload: PREREQ_MISSING (no hardware attached in the evidence run)
- e2e: PREREQ_MISSING (stage8/9/10 requires runtime network prerequisites)
- notes: upload and network e2e remain conditional; explicit `PREREQ_MISSING` logs are captured.

## Summary

- evidence_root: docs/work/diag/20260222_214039
- git_sha: `2a3b9df`
- stage coverage: stage0..stage10 build PASS
- boot coverage: stage0 and stage10 PASS
- CLI command coverage: serial command pathway PASS

## Stage evidence

- status: PASS
- evidence: docs/work/diag/20260222_214039/90_summary.md
- notes: comprehensive stage matrix and native unit evidence captured; remaining gaps are environment-dependent e2e prerequisites.

## Top known issues

1. Linting is now strict (`PASS`), no PASS_WITH_WAIVER.
2. `luce_test_native` matrix entry is fixed with native filter; image generation now passes.
3. Network-enabled e2e on stage8/9/10 depends on valid runtime environment.
4. Stage8/9/10 e2e evidence now captures PREREQ_MISSING skip cases when prerequisites are absent.
5. `docs/work/tooling` is now present.

## Blockers

- none
