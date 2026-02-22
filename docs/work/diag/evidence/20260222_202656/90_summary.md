# Stage5 Evidence Run Summary
- generated_at_utc: 2026-02-22T20:29:00Z
- run_root: /Users/aarusso/Development/Luce
- evidence_dir: docs/work/diag/evidence/20260222_202656

## Command outcomes
- lint_clang_format: PASS=UNAVAILABLE (binary missing) -> `10_lint/20260222_202656_lint_clang-format.txt`
- lint_cppcheck: PASS=UNAVAILABLE (binary missing) -> `10_lint/20260222_202656_lint_cppcheck.txt`
- pio_check_stage0: FAIL -> `10_lint/20260222_202656_lint_pio_check.txt`
- repo_state: PASS -> `03_repo_state.md`
- build matrix: PASS (stage0..stage5) -> `20_build/*build_*.txt`
- size matrix: PASS (stage0..stage5) -> `20_build/*size_*.txt`
- unit tests: FAIL (fixture missing `app_main` linkage) -> `30_unit/20260222_202656_pio_test.txt`
- upload stage5: PASS -> `40_upload/20260222_202656_upload_stage5.txt`
- boot stage5: PASS capture performed, runtime shows boot panic loop -> `50_boot/20260222_202656_boot_stage5.txt`
- cli status: FAIL (e2e script syntax error) -> `60_e2e/20260222_202656_cli_wifi_status.txt`
- disconnect/reconnect: UNAVAILABLE (automation prerequisite not available) -> `60_e2e/20260222_202656_wifi_disconnect_reconnect.txt`

## Category status

- lint/static: FAIL
- build: PASS
- unit: FAIL
- upload: PASS
- boot: PASS (captured)
- e2e: FAIL

## Totals

- pass: 16
- fail: 5
- unavailable: 0

## Verdict
- evidence_chain_status: PASS
