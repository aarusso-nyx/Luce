# Governance Chain Guide

- generated_at: 2026-02-22T18:14:24.345Z
- repo: /Users/aarusso/Development/Luce
- current_point: post-scoring
- latest_scorecard: docs/governance/compliance/scorecard-2026-02-22.md
- latest_scorecard_status: PASS
- latest_scorecard_nci: 100

## Chain Status

- 1. Prompt Scope Loader Check | PASS | skill: prompt-scope-loader-check | evidence: docs/governance/audit/prompt-scope-loader-check.md
- 2. Initial Gate | PASS | skill: compliance-gate / agents-preflight | evidence: docs/governance/health/preflight.md
- 3. Structure Audit | PASS | skill: governance-structure-audit | evidence: docs/governance/audit/structure-conformance.md
- 4. Alignment (if gaps) | N/A | skill: repo-governance-align | evidence: layout complete
- 5. Health Evidence | PASS | skill: health-evidence-capture + health-gate | evidence: docs/governance/health/build-status.md
- 6. NPM Security Audit | N/A | skill: npm-security-upgrade | evidence: docs/governance/audit/npm-applicability.md
- 7. Adherence Audit | PASS | skill: verify-adherence | evidence: docs/governance/audit/{code-docs,code-tests,docs-tests}.md
- 8. Compliance Scorecard | PASS | skill: compliance-scorecard | evidence: docs/governance/compliance/scorecard-2026-02-22.md
- 9. Final Re-Gate | PASS | skill: compliance-gate / agents-preflight | evidence: preflight is up-to-date

## Next Steps

1. 6. NPM Security Audit is N/A for firmware-only scope. Re-run only if Node tooling is introduced.

## Repeat Rules

- After `repo-governance-align`, repeat `governance-structure-audit` and then re-run gate.
- After code/tests/docs changes, repeat `health-evidence-capture` and `verify-adherence`.
- After dependency changes in a Node/JavaScript ecosystem, run `npm-security-upgrade`.
- After any audit updates, regenerate `compliance-scorecard` and run final gate.
