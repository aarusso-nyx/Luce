# Phase 2 Entry Criteria (Luce)

## Decision basis
This gate combines results from:
- Governance structure/audit and adherence outputs
- Architecture alignment review
- Code quality + DRY audit
- Test strategy plan

## Go/No-Go
- **Gate status: GO**
- **Go condition:** all “Must be true” items below are satisfied with evidence before enabling networking changes.
- **No-Go triggers:** any missing mandatory artifact, unresolved blocking architecture risk, or unclosed Must task.

### Last gate evidence
- **Evaluated at (UTC):** update with latest run timestamp
- **Evidence set:** `docs/work/diag/evidence/<timestamp>`
- **Evaluated git SHA:** update during next gate review

## Must be true checklist

1. Governance and evidence hygiene is clean
- `docs/work/diag/audit/20260222_governance_structure.md` must report PASS with canonical path and evidence coverage.
- Required evidence files from structure/compliance docs exist and are current:
- `docs/governance/audit/structure-conformance.md`
- `docs/governance/compliance/scorecard-current.md`
  - `docs/governance/health/preflight.md`

2. Stage-gating integrity is proven
- No future-stage symbol leakage remains in lower-stage build targets.
- Verified by compile/build evidence and evidence chain:
  - `docs/work/diag/evidence/<timestamp>/build/build_luce_stage0.txt`
  - `docs/work/diag/evidence/<timestamp>/build/build_luce_stage4.txt`
  - `docs/work/diag/evidence/<timestamp>/lint/platformio_check_luce_stage0.txt`
  - `docs/work/diag/evidence/<timestamp>/lint/platformio_check_luce_stage4.txt`

3. Alignment and risk baseline is approved for networking preconditions
- Architecture alignment file must explicitly call out and track mitigation for preconditions:
  - `docs/review/analysis/20260222_architecture_alignment.md`
- Required remediation tasks from this alignment review must be converted to backlog items before phase entry.

4. DRY/refactoring roadmap is approved and prioritized
- `docs/review/analysis/20260222_code_quality_dry.md` must list:
  - Top 15 DRY candidates,
  - Must/Should/Could shortlist,
  - Safe incremental sequencing.

5. Test plan is ready and staged
- Unit/component/e2e strategy artifacts exist:
  - `docs/work/plan/040_test_strategy_pre_phase2.md`
  - `docs/work/plan/041_hil_matrix.md`
- Minimum viable test set is explicitly defined in `040` as a precondition to Phase2.

6. Stage4 evidence sufficiency for runtime behavior
- Evidence snapshot must include:
  - upload success log (`docs/work/diag/evidence/<timestamp>/upload/luce_stage4_upload.txt`)
  - boot artifact (`docs/work/diag/evidence/<timestamp>/boot/boot_luce_stage4.txt`)
  - CLI-session baseline (`docs/work/diag/evidence/<timestamp>/e2e/cli_stage4_session.txt`).

7. Network surface remains absent from current code/docs baseline
- No networking surface should be introduced in this phase of gating.
- If any networking artifact appears, document and gate remains No-Go until reviewed.

## Explicit forbidden actions until gate is passed
- Do not add HTTP/MQTT/socket code or client/server handlers.
- Do not add runtime configuration toggles that bypass stage gating.
- Do not relax safety assumptions in relay/MCP/boot-critical paths.
- Do not merge behavioral refactors that alter serial CLI contract before evidence of equivalence.
- Do not delete or move governance evidence files without re-creating required equivalents.
- Do not alter build/test naming conventions used by evidence chain unless strategy is updated in docs.

## Failure policy
- Any failure in an item above blocks Phase2.
- Any newly added violation that was not pre-scored in the architecture/quality/audit docs requires immediate update to backlog and evidence before unblocking.

## Phase 2 Gate Sign-Off (one-page)

### Evidence Review Checklist
- [ ] Governance evidence files are present and PASS.
  - Proof: `docs/work/diag/audit/20260222_governance_structure.md`
- [ ] Stage matrix evidence confirms compile integrity for stage0..4.
  - Proof: `docs/work/diag/evidence/<timestamp>/build/build_luce_stage0.txt`
- [ ] Architecture preconditions captured and approved in review.
  - Proof: `docs/review/analysis/20260222_architecture_alignment.md`
- [ ] DRY/refactor plan exists with Must/Should/Could ordering.
  - Proof: `docs/review/analysis/20260222_code_quality_dry.md`
- [ ] Test strategy and HIL matrix are complete and staged.
  - Proof: `docs/work/plan/040_test_strategy_pre_phase2.md`, `docs/work/plan/041_hil_matrix.md`
- [ ] Minimum viable test set executed.
  - Proof: `docs/work/diag/evidence/<timestamp>/unit/unit_native.txt`, `docs/work/diag/evidence/<timestamp>/boot/boot_luce_stage4.txt`, `docs/work/diag/evidence/<timestamp>/e2e/cli_stage4_session.txt`
- [ ] No networking surfaces introduced as part of pre-phase2 state.
  - Proof: adherence summary in `docs/work/diag/audit/20260222_verify_adherence.md`

### Gate Outcome
- Status: GO
- Authorized approver: _____________________
- Approval date: _____________________
- Signature block: _____________________

### Required blocker condition
- Any unchecked item above or any newly introduced blocking violation in a re-scan -> gate remains NO-GO.
