# LUCE Scorecard Current State

Date: 2026-05-28

This file is the live scorecard pointer, not a claim that the archived
2026-02-23 scorecard is current. The archived scorecards remain historical
snapshots:

- `docs/governance/compliance/scorecard-2026-02-23.md`
- `docs/governance/compliance/archive/scorecard-2026-02-22.md`
- `docs/governance/compliance/archive/scorecard-2026-02-20.md`

## Current Verification Model

- Local evidence is the source of truth for the active checkout.
- Firmware builds are verified locally for `default`, `net0`, and `net1`.
- Hardware-backed HIL evidence remains the release gate for device behavior.
- Host unit tests cover pure C++ helpers without a connected ESP32.
- CI runs hardware-free checks directly, verifies committed local evidence, and
  refuses release contexts unless `hardware-hil` is required and passing.
  CI is not the producer of hardware evidence.

```yaml
verification_model:
  current_evidence_root: docs/work/diag/
  firmware_build_envs:
    - default
    - net0
    - net1
  release_gate: hardware-backed HIL contract evidence
  host_unit_tests: populated in Wave E
  ci_gate: Wave F, checks committed evidence, host tests, format, and release HIL enforcement
  archived_scorecards_are_current_claims: false
  nci_status: requires fresh local evidence before reassertion
  program:
    A: docs dedup, shared header scaffolding, host-test harness scaffold
    B: core correctness for I/O hardening, MQTT correctness, test hardening
    C: cross-cutting adoption sweep and duplication reduction
    D: shared certificate and PKI infrastructure
    E: unit tests, CI gating, modern-C++ polish, docs reconciliation
    F: residual lock hardening, release evidence enforcement, repo-wide format gate
  residuals:
    ci_hardware_enforcement: closed for release contexts
    format_gate_scope: closed across tracked src and include/luce C/C++ files
    i2c_and_mqtt_races: closed by guarded RMW and locked MQTT snapshots
    pki_dead_code_and_staging_cost: closed by key_present state and bounded RAM staging
```

## Evidence Regeneration

Regenerate current local evidence with:

```bash
source ~/.zshrc
pio run -e default -e net0 -e net1
scripts/run_host_tests.sh
.venv/bin/python -m pytest tests/test_docs_alignment.py
python scripts/verify_evidence.py
```
