# Bootstrap Plan 000

Date: 2026-02-22

## Objective
Establish a clean, deterministic baseline for Luce firmware development with governance evidence and stable build entrypoints.

## Key Decisions
- Standardized all operator workflows on `scripts/build.sh`, `scripts/flash.sh`, and `scripts/monitor.sh`.
- `LUCE_STAGE` is the stage selector (`dev` default); scripts build stage-matched envs when present, otherwise all declared envs.
- Governance evidence is canonical under `docs/governance/{audit,health,compliance}`.
- Temporary investigation/diagnostics/planning artifacts are constrained to `docs/work/{inv,diag,plan}`.

## Invariants
- Root stability: `README.md` and `AGENTS.md` remain at repository root.
- Canonical source trees remain unchanged: `src/`, `include/`, `lib/`, `test/`, `scripts/`.
- No networking feature additions in bootstrap scope.
- Build entrypoints are shell-only and deterministic (explicit env selection, fail-fast behavior).

## Validation Steps
- Run `./scripts/build.sh` or `pio run -e <env>`.
- Run preflight gate and confirm `docs/governance/health/preflight.md` is current.
- Confirm required governance artifacts exist and are tracked.
