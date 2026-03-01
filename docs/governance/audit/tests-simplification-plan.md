# Tests Simplification Plan

Date: 2026-03-01
Source: `docs/governance/audit/tests-current-state.md`

## Objective

Keep the firmware-only hardware test policy intact while reducing harness complexity, output noise, and operator ambiguity.

## Constraints

- Keep PlatformIO + `scripts/luce.sh` as canonical harness.
- Keep canonical environments: `default`, `net0`, `net1`.
- Do not reintroduce removed native/stub test frameworks.

## Phase 1: Normalize Interface and Outputs (low risk)

1. Standardize serial test artifact naming.
- Change `collect` and `test` outputs to one consistent extension/pattern.
- Keep one stable filename contract per command.

2. Add explicit output index per run.
- Write `docs/work/diag/<RUN_ID>/index.txt` listing generated files and command exit codes.
- Reduce manual traversal during audits.

3. Isolate HTTP smoke temp output.
- Replace fixed `/tmp/luce_http_smoke.body` with run-scoped temp file (for example `mktemp`).
- Prevent collisions during parallel or repeated runs.

Acceptance criteria:
- `build`, `test`, `http-smoke`, and `lint` outputs are deterministic and consistently named.
- No fixed shared temp file in HTTP smoke path.

## Phase 2: Clarify Assertions and Failure Semantics (medium risk)

1. Versioned marker profile for smoke tests.
- Move required boot markers to a small profile file (for example under `scripts/`), keyed by env.
- Keep `--require` as an additive override.

2. Emit machine-readable result summary.
- For each `test` run, write a compact status file (`PASS/FAIL`, missing markers list, env, ports).
- Preserve full logs for human diagnostics.

3. Distinguish infra failure vs assertion failure.
- Use explicit exit/status tagging for:
  - upload/serial prerequisites missing
  - capture execution error
  - marker assertion mismatch

Acceptance criteria:
- Operator can identify failure class without reading full logs.
- Marker updates are data-only changes, not script logic edits.

## Phase 3: Reduce Duplication and Improve Maintainability (medium/high risk)

1. Consolidate command plumbing.
- Remove duplicated option parsing patterns where feasible.
- Centralize shared validation (positive ints, env resolution, port handling).

2. Unify command logging preamble.
- Standardize headers written by `run_and_capture` and direct command wrappers.

3. Add shell-level static quality gate for harness scripts.
- Integrate `shellcheck` (when available) into `lint` or `health` flow as non-breaking advisory first.

Acceptance criteria:
- Reduced script duplication in `scripts/luce.sh`.
- Consistent preamble and metadata across command logs.
- Shell lint findings visible in diagnostics.

## Suggested Execution Order

1. Phase 1 changes (naming/temp-file isolation/index file).
2. Phase 2 changes (assertion model/result summaries).
3. Phase 3 refactor hardening.

## Risk Notes

- Any changes touching log format may impact governance evidence parsers and existing audit references.
- Marker profile externalization must preserve current net1 assertions exactly before adding flexibility.
- Temp-file/path adjustments should remain POSIX-compatible with current shell target.

## Verification Checklist

After each phase:
- Run `scripts/luce.sh health`.
- Run `scripts/luce.sh test --env net1 --duration 45` (with attached hardware).
- Run `scripts/luce.sh http-smoke --host <device> --token <token>` where HTTP is enabled.
- Confirm artifacts exist under `docs/work/diag/<RUN_ID>/` and are self-describing.
- Update `docs/user/testing.md` only if operator-facing behavior changes.
