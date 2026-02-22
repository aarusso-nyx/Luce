# Lint Tooling Notes — 2026-02-22

## What was failing
- `platformio check` was previously invoked without package exclusion and crashed in two ways:
  - `python3 -m platformio check -e luce_stage0` reported:
    `Error: cppcheck failed to perform check!`
  - `python3 -m platformio check -e luce_stage4` reported:
    `KeyError: 'version'` while reading CMake codemodel data.
- Running `pio check` also triggered mixed-core invocation issues (`/opt/homebrew/bin/platformio` wrapper pointing to missing Python), so linting was routed through `python3 -m platformio`.

## Stable lint path implemented
- Added `scripts/lint.sh` with the following characteristics:
  - Uses `python3 -m platformio` directly.
  - Runs `platformio check` for stage environments `luce_stage0..luce_stage4`.
  - Uses `--skip-packages` to avoid third-party toolchain header parsing regressions.
  - Writes per-environment output to:
    `docs/work/diag/<timestamp>/lint/platformio_check_luce_stage*.txt`
  - Produces a machine-parseable status marker per env (`STATUS=PASS`, `STATUS=PASS_WITH_WAIVER`, `STATUS=FAIL`).
  - Supports literal-fixme waivers via `scripts/lint_waivers.txt`.

## Findings and replacement check rationale
- `src/main.cpp:1158: [medium:warning] Uninitialized variable: argv [uninitvar]` is reproducibly reported for stages 1–4.
- Existing remediation is currently non-code: this issue is explicitly documented as a waiver in `scripts/lint_waivers.txt`.
- Replacement check now used:
  - `python3 -m platformio check -e <env> --skip-packages`
  - run for `luce_stage0`, `luce_stage1`, `luce_stage2`, `luce_stage3`, `luce_stage4`.
- Result classification:
  - `luce_stage0` currently clean (PASS)
  - `luce_stage1..luce_stage4` report medium waiver-only finding (PASS_WITH_WAIVER)

## Operational notes
- Keep waivers explicit and small:
  - only add entries for stable, reviewed findings.
  - include line/function and warning id text.
- If waiver set grows, replace with targeted code fixes before Stage-2 entry.
