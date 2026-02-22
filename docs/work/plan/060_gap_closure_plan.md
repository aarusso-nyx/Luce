# Luce Gap Closure Plan (Phase-2 Readiness)

## Executive view
Current blockers are concentrated in three classes:
- Evidence completeness: boot/e2e/test evidence is still insufficient or blocked by environment.
- Stage4 stability: stage4 compile/check paths are intermittently failing and no clean deterministic evidence exists.
- Failure-model gaps: brownout/task recovery behavior is under-specified, and monolithic structure remains a maintainability risk.

## Extracted gaps and required closure

| Gap | Root cause | Owner prompt | Acceptance | Evidence file path |
|---|---|---|---|---|
| Stage4 lint/build instability | `platformio check`/build failures and toolchain/codemodel/C++ check tool errors on stage4 runs | Platform owner: resolve stage4 build toolchain path and re-run matrix | Stage4 check+build complete cleanly (or documented acceptable warnings) with identical source signature; no stage-regression diagnostics | `docs/work/diag/evidence/20260222_153914/build/luce_stage4.txt`, `docs/work/diag/evidence/20260222_153914/lint/platformio_check_stage4.txt`, updated evidence `docs/work/diag/evidence/<timestamp>/build` |
| Stage0/host lint check unavailable due missing/failed cppcheck | Static check depends on external checker not producing output | Tooling owner: install/align cppcheck in environment and add reproducible preflight for check-only runs | `platformio check -e luce_stage0` completes with expected exit code and captured output | `docs/work/diag/evidence/<timestamp>/lint/platformio_check_stage0.txt`, `docs/work/diag/evidence/90_summary.md` |
| Unit tests not implemented | `pio test` reports nothing to build | Test owner: create host test harness and baseline tests (bitmask/CLI/state formatting/debounce if extractable) | Unit suite runnable with explicit pass/fail report for at least required unit list | `docs/work/diag/evidence/<timestamp>/unit/<date>_pure_logic_*.md` |
| Boot/e2e telemetry blocked by terminal capture failure (`termios`) | Serial monitor path/TTY capture incompatibility in environment | Evidence owner: re-run boot+CLI using compatible capture path and produce transcripts | `docs/work/diag/evidence/90_summary.md` updated from `FAIL` to at least `PASS` for `boot` and `e2e`; `docs/work/diag/evidence/50_boot/`, `docs/work/diag/evidence/60_e2e/` updated |
| Stage4 runtime crash signature in CLI transcript (`tlsf_free` assert) | Heap/free assertion observed before stable session | Runtime owner: diagnose/mitigate heap corruption path and confirm no repeat in cleaned stage4 CLI capture | `docs/work/diag/evidence/20260222_153914/cli/stage4_cli_transcript.txt`, renewed `docs/work/diag/evidence/60_e2e/..._cli_*.md` |
| Brownout/WDT behavior not codified | No explicit policy for containment beyond reset logging | Reliability owner: define and document brownout and watchdog policy + add code path/handlers where required | `docs/review/analysis/20260222_architecture_alignment.md` updated in follow-up; implementation evidence to include added policies under `docs/review/analysis/` or changelog |
| Monolithic architecture limits ownership boundaries | All subsystems in one TU, no separate ownership boundaries beyond global variables | Refactor owner: keep behavior stable while extracting low-churn abstractions for diagnostics/I2C/CLI/state formatting | `docs/review/analysis/20260222_code_quality_dry.md` linked tasks completed and evidence in build/lint + unit/component/e2e updates |
| Evidence index hygiene drift risk | Evidence exists but not all new runs are reflected under current index | Governance owner: enforce naming, index, and timestamped manifest with pass/fail per run | `docs/work/diag/evidence/00_index.md`, per-run `docs/work/diag/evidence/<timestamp>/index.md` |

## Ordered execution plan

### 1) Stop the bleeding (tooling + evidence capture)
1. Re-run preflight chain with clean environment and regenerate:
   - `docs/work/diag/evidence/<timestamp>/preflight.md`
   - `docs/work/diag/evidence/<timestamp>/build/*`
   - `docs/work/diag/evidence/<timestamp>/lint/*`
2. Fix check toolchain for `pio check` on stage0/stage4;
   - if toolchain unavailable, capture explicit tool-version waiver.
3. Capture a fresh evidence summary and unblock `boot`/`e2e` categories by using a monitor capture method that supports the local TTY path.
4. Normalize evidence naming and ensure each run has a dated index file and update `docs/work/diag/evidence/00_index.md`.

### 2) Stabilize stage4
1. Investigate and resolve remaining stage4 build failures (`build/luce_stage4.txt`, `platformio_check_stage4.txt`).
2. Rebuild clean matrix stage0..4 after stage4 stabilization.
3. Validate stage4 upload and immediate reboot path is stable:
   - collect upload log and CLI boot transcript without crash.
4. Re-run targeted diagnostics (`status`/`help`/`i2c_scan`/`mcp` commands) once stable prompt is confirmed.

### 3) Enable minimal tests
1. Add host unit suite covering minimum set:
   - bitmask helpers
   - CLI parsing
   - state formatting
   - (debounce/dependency if extractable)
2. Add `unit/<date>_stage_build_matrix.md` report and `unit/*` evidence files.
3. Execute component baseline:
   - I2C scan, MCP readback, LCD smoke (where hardware allows)
4. Execute e2e minimum CLI triad:
   - `help`, `status`, `relay_mask safe`
5. Validate required signatures in every file (`TS`, `RESULT`, `FAIL_REASON`, `ERROR_DOMAIN`, `COMPONENT`, `EXPECTED`, `ACTUAL`, `REPRO_CMD`).

### 4) DRY/cleanup with low churn
1. Implement Must refactors from `docs/review/analysis/20260222_code_quality_dry.md` in small steps:
   - stage-capability policy abstraction,
   - unified startup diagnostics helper,
   - MCP bitmask formatter.
2. After each change:
   - run lint/check/build for affected stages,
   - append evidence to same timestamp run.
3. Defer Should/Could refactors until Musts pass and stage4 gate is green.

## Prioritized must/fix sequence

### Must
- Stabilize stage4 compile/check path.
- Re-establish host and on-device evidence capture for boot/e2e.
- Produce minimal unit suite baseline and stage build matrix.
- Resolve heap/free runtime crash evidence blockers for stage4 command path.

### Should
- Add explicit brownout/WDT policy and traceable handling.
- Improve compile-time evidence manifest discipline (`00_index`, per-run index).
- Add deterministic component baselines for I2C/MCP/LCD.

### Could
- Begin broader module extraction to improve boundaries after Must milestones.
- Add richer failure signatures and auto-parsing scripts for evidence triage.

## Readiness dependencies

Must progress must complete before entering networking work:
1. `docs/work/plan/050_phase2_entry_criteria.md` must remain aligned to current evidence before any stage2+ regression.
2. `docs/work/plan/061_phase2_readiness_checklist.md` updated with all evidence links checked.
3. Evidence-backed resolution of all Must items above.
