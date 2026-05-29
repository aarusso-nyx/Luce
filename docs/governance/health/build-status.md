# Build Status

- timestamp: 2026-05-28T02:26:30Z

## Checks

- health: PASS (`./scripts/luce.sh health`; environments resolved as `default`, `net0`, `net1`).
- config: PASS (`pio project config`; `net0` and `net1` inherit common build flags plus their `LUCE_NET_*` flags).
- lint: PASS (`docs/work/diag/20260527_232624/lint/summary.txt`; `default`, `net0`, `net1`).
- build: PASS for canonical environments:
  - `default`: `docs/work/diag/20260527_232623/build/default/build.txt`
  - `net0`: `docs/work/diag/20260527_232623/build/net0/build.txt`
  - `net1`: `docs/work/diag/20260527_232610/build/net1/build.txt`
- test: PARTIAL_PASS; host unit tests, filesystem docs-alignment pytest, release-gate negative/positive verifier checks, and whole-tree clang-format pass locally. Hardware-backed boot/protocol layers are not reproduced in this run.
- upload: SKIPPED (no hardware upload validated in this evidence update).
- boot: SKIPPED (no upload path validated in this evidence update).
- e2e: PREREQ_MISSING for NET1 (device IP, tokens, broker, and hardware prerequisites required).

## Notes

- Native host testing is active for pure helper logic; hardware remains required and enforced for release evidence.
- Canonical test target is real firmware on `net1`.
- Historical PASS entries are retained in archived scorecards/logs only; this file describes current reproducibility status.
- Local `platformio` at `/opt/homebrew/bin/platformio` still has a broken interpreter, but `source ~/.zshrc` exposes a working `pio` at `~/.platformio/penv/bin/pio`.
- Earlier CMake-generation failures in `docs/work/diag/20260527_224004/build/` were cleared by allowing the concurrent PlatformIO process to finish and rerunning each env from the stable generated tree.
- Python contract-test dependencies are installed in repo-local `.venv`; Homebrew system Python rejects direct system installs under PEP 668.
