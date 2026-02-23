# Evidence Summary
run_timestamp_utc: 2026-02-22T20:45:47Z
evidence_root: docs/work/diag/evidence/20260222_204547
git_sha: 4dcead6
stage: 10
lint/static: PASS
build: PASS
unit: PASS
upload: PASS
boot: PASS_WITH_LIMITATION
e2e: PASS_WITH_LIMITATION

- lint: `docs/work/diag/20260222_204547/10_lint/lint_luce_stage10.txt`
- build: `docs/work/diag/20260222_204547/20_build/build_luce_stage10.txt`
- unit: `docs/work/diag/20260222_204547/30_unit/unit_native.txt`
- upload: `docs/work/diag/20260222_204547/40_upload/upload_luce_stage10.txt`
- boot: `docs/work/diag/20260222_204547/50_boot/luce_stage10_boot.txt`
- e2e: `docs/work/diag/20260222_204547/60_e2e/luce_stage10_cli_http_status.txt`

notes:
- boot evidence shows deterministic startup and command lifecycle for `http.status` with `http` feature enabled by build stage and runtime disabled by default (`enabled=0`).
- network HTTPS endpoint transcript is intentionally not captured in this evidence run; `curl` verification remains pending `http/enabled=1` and network connectivity.
