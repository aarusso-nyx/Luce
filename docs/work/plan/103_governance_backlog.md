# Governance Backlog (Post-Implementation)

Date: 2026-02-23

## Top unresolved backlog items

1. Lint strictness.  
Severity: Medium. Owner type: tooling. Evidence: `docs/work/diag/<timestamp>/10_lint/lint_stage0.txt` through `lint_stage4.txt` (strict path) and `scripts/lint_waivers.txt` (now empty).  
Status: DONE  
Item: `PASS_WITH_WAIVER` was resolved by initializing `WifiEvent evt` in `src/main.cpp` and removing remaining medium waiver entry.  
Evidence: `docs/work/diag/20260222_214039/10_lint/lint_run.log`.
Acceptance: lint matrix reports strict PASS with no waived findings.

2. Native matrix image for `luce_test_native`.  
Severity: Medium. Owner type: code/tests. Evidence: `docs/work/diag/<timestamp>/20_build/build_luce_test_native.txt` and `30_unit/unit_luce_test_native.txt` (PASS).  
Status: DONE  
Item: native matrix previously failed because `src/main.cpp` compiled in native mode; now excluded via `build_src_filter` in `platformio.ini`.  
Evidence: `docs/work/diag/20260222_214039/20_build/build_luce_test_native.txt`, `docs/work/diag/20260222_214039/30_unit/unit_luce_test_native.txt`.
Acceptance: `pio run -e luce_test_native` and `pio test -e luce_test_native` pass.

3. Stage8 e2e transport conditions.  
Severity: Low. Owner type: tests. Evidence: `docs/work/diag/evidence/20260222_191336/90_summary.md` and latest evidence if reused.  
Status: DONE  
Item: update stage8 evidence capture docs to require explicit PREREQ_MET/PREREQ_MISSING handling.  
Evidence: `docs/work/diag/20260222_214039/60_e2e/cli_stage8_prereq_missing.txt`.
Acceptance: stage8 HIL doc includes explicit prerequisite matrix and skip artifacts in both network-connected and missing paths.

4. Missing optional tooling directory.  
Severity: Low. Owner type: docs. Evidence: `docs/work/inv/rescued/*` and `docs/governance/audit/structure-conformance.md` (latest).  
Status: DONE  
Item: `docs/work/tooling` directory was absent in one structure snapshot.  
Acceptance: create `docs/work/tooling/.gitkeep` and README.

5. Legacy command references.  
Severity: Low. Owner type: docs. Evidence: `docs/work/plan/100_post_impl_realignment.md`, `docs/work/plan/101_doc_promotion_plan.md`, `docs/user/CLI.md`.  
Status: DONE  
Item: authoritative doc path references still pointed to `doc/*` in multiple governance docs.  
Acceptance: migrate remaining references to `docs/user/*` where those files now live and avoid conflicting authority statements.

## "Next Gate" candidate list before major refactor

- Close items 1 and 2 before declaring full test completeness.
- Keep 3 and 4 explicit in the evidence chain as environment conditions.
- Keep 5 as docs hygiene check when updating user-facing documentation.
