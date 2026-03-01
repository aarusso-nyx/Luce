# Tests Current State

Date: 2026-03-01
Auditor: Codex (`test-harness-inventory`)

## Scope

Inventory of test harness components in this repository, focused on:
- test suites/runners and config entrypoints
- environment loading and mode flags
- output directories and noise sources

## 1) Suite and Runner Inventory

## JavaScript test harnesses (Jest/Playwright/npm)

- `package.json`: not present.
- Jest config files (`jest.config.*`): not present.
- Playwright config files (`playwright.config.*`): not present.
- `tests/` or `test/` suite directories: `tests/` present and used by Python pytest.
- npm test scripts: not applicable (no npm manifest in repo).

Conclusion: there is no active JS/Node test harness in-repo; test automation is Python/pytest.

## Firmware test harness (active)

Primary firmware operations runner is `scripts/luce.sh` with PlatformIO-oriented commands:
- `build`
- `upload`
- `monitor`
- `collect`
- `lint`
- `health`
- `http-smoke`
- `clean`

Primary test runner is `scripts/test_layers.py` (layered test entrypoint).

Canonical smoke test flow is firmware-on-device:
- `python3 scripts/test_layers.py --layers boot --env net1 --boot-duration 45`

Supporting scripts:
- `scripts/capture_serial.py` (serial evidence capture)
- `scripts/http_api_smoke.sh` (HTTP endpoint smoke checks)

Pytest suite (contract/HIL):
- `scripts/test_layers.py` (single layered entrypoint)
- `tests/pytest.ini`
- `tests/conftest.py`
- `tests/test_http_contract.py`
- `tests/test_tcp_cli_contract.py`
- `tests/test_ws_contract.py`
- `tests/test_mqtt_contract.py`

## 2) Config and Mode Flags

## PlatformIO environments

Defined in `platformio.ini`:
- `default`
- `net0`
- `net1`

Feature gating is compile-time via `build_flags` in `platformio.ini`.

## Harness defaults and selectors

In `scripts/test_layers.py`:
- Layer selection: `--layers` (`build`, `boot`, `http`, `tcp`, `ws`, `mqtt`, or `all`)
- Build/boot env: `--env` (default `net1`)
- Boot capture duration: `--boot-duration`
- Additional boot marker assertions: `--require-marker`
- Protocol endpoints/tokens: `--host`, `--http-token`, `--tcp-*`, `--ws-*`, `--mqtt-*`
- Failure behavior: `--continue-on-fail`
- Output roots: `--diag-root`, `--run-id`

## Environment variables used by harness

- `LUCE_ENV`
- `LUCE_UPLOAD_PORT` (default `/dev/cu.usbserial-0001`)
- `LUCE_MONITOR_PORT` (default `/dev/cu.usbserial-40110`)
- `LUCE_DIAG_DIR` (default `docs/work/diag`)
- `LUCE_HTTP_TOKEN`/`LUCE_CLI_NET_TOKEN`/`LUCE_MQTT_*`/`LUCE_WS_*` (used by `scripts/test_layers.py` and pytest fixtures)

Tool resolution behavior:
- `scripts/luce.sh` attempts to locate `pio`; if not found, it refreshes PATH from `~/.zshrc` and falls back to `python3 -m platformio`.

## 3) Output Directories and Noise Sources

## Primary outputs

All harness command outputs are timestamped under:
- `docs/work/diag/<RUN_ID>/<command>/...`

Common files include:
- build logs (`build.txt`)
- upload logs (`upload.txt`)
- monitor logs (`monitor_boot.txt`)
- collected/test serial logs (`<env>_<tag>.log`)
- lint logs (`platformio_check_<env>.txt`, `summary.txt`)
- health logs (`health.txt`)
- http smoke logs (`http-smoke.txt`)
- layered test logs (`test-layers/<layer>.log`)
- pytest JUnit XML for protocol layers (`test-layers/junit-<layer>.xml`)

## Secondary outputs / transient files

- `scripts/http_api_smoke.sh` writes response bodies to `/tmp/luce_http_smoke.body`.
- PlatformIO build artifacts land under `.pio/` (outside `docs/work/diag`, but expected for build/test execution).

## Noise and fragility observations

1. Command logs include timestamps and full command lines; useful for traceability but noisy for diff/review.
2. HTTP smoke script uses a fixed temporary path in `/tmp`, which can collide across concurrent runs.
3. Build/boot/protocol layers are hardware/network dependent, so CI reproducibility is limited without hardware adapters.
4. Boot marker checks are string-based and strict; small boot text changes can fail smoke tests even when behavior is correct.

## 4) Current Governance Alignment (tests)

- Repository documentation (`docs/user/testing.md`) matches the active policy: firmware-only, hardware smoke validation on `net1`.
- No hidden alternate test runner was found in the repository.

## 5) Summary

Current test harness is firmware-centric with a pytest-based contract layer and remains coherent for staged automation (build, boot, HTTP/TCP/WS/MQTT). Remaining evolution should focus on broader state-machine and persistence scenario depth.
