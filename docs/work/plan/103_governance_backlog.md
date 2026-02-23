# Governance Backlog (Post-Implementation)

Date: 2026-02-23

## Top unresolved backlog items

1. Lint strictness.  
Severity: Medium. Owner type: tooling. Evidence: `docs/work/diag/evidence/20260222_211814/10_lint/lint_run.log`.  
Item: `PASS_WITH_WAIVER` remains in place because legacy checks are waived.  
Acceptance: remove waivers or formalize a stable waiver policy with justifying comments.

2. Native matrix image for `luce_test_native`.  
Severity: Medium. Owner type: code/tests. Evidence: `docs/work/diag/evidence/20260222_211814/20_build/build_luce_test_native.txt`.  
Item: build matrix entry notes `FAIL (build uses non-native driver/gpio.h; no native test image produced)`.  
Acceptance: isolate host-only tests into pure C++ path with no ESP-IDF-only headers.

3. Stage8 e2e transport conditions.  
Severity: Low. Owner type: tests. Evidence: `docs/work/diag/evidence/20260222_191336/90_summary.md` and latest evidence if reused.  
Item: older stage8 e2e sequence may be affected by missing Wi-Fi credentials; avoid assuming connectivity in scripts.  
Acceptance: document and gate e2e on credential availability.

4. Missing optional tooling directory.  
Severity: Low. Owner type: docs. Evidence: `docs/governance/audit/structure-conformance.md`.  
Item: `docs/work/tooling` reported missing in one structure snapshot.  
Acceptance: create `docs/work/tooling` with at least `.gitkeep` or remove check expectation.

5. Legacy command references.  
Severity: Low. Owner type: docs. Evidence: `docs/user/CLI.md`, `docs/dev/hardware-map.md`.  
Item: user and dev docs remain as historical references; canonical docs are now `doc/*`.  
Acceptance: add canonical cross-refs and avoid conflicting authoritative statements.

## "Next Gate" candidate list before major refactor

- Close items 1 and 2 before declaring full test completeness.
- Keep 3 and 4 explicit in the evidence chain as environment conditions.
- Keep 5 as docs hygiene check when updating user-facing documentation.
