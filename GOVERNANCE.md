# Luce Governance

This repository follows `AGENTS.md` as the operational contract.

## Canonical Governance Paths

- Audit evidence: `docs/governance/audit/`
- Health evidence: `docs/governance/health/`
- Compliance evidence: `docs/governance/compliance/`

## Work Scratch Paths

- Planning: `docs/work/plan/`
- Investigation: `docs/work/inv/`
- Diagnostics: `docs/work/diag/`

## Bootstrap Guardrails

- Keep `README.md` and `AGENTS.md` stable at repository root.
- Keep canonical source trees: `src/`, `include/`, `scripts/`.
- Use canonical HTTP routes and MQTT topics only (no `/v2` aliases).
- Keep CLI/HTTP/MQTT behavior and documentation in sync.
