# Luce Test Strategy

## Layers

- `host`: pure logic tests executed with `platformio test -e native`.
- `embedded`: component and task-level integration tests executed with `platformio test -e nodemcu-32s`.
- `hil`: hardware integration tests run through `scripts/test/run_hil.py`.

## Coverage Mapping

- Policy/automation: `test/host_policy/policy_test.cpp`.
- Auth and parser behavior: `test/host_parser/protocol_test.cpp`.
- Store/logger wrappers and service-level smoke checks: `test/embedded_smoke/embedded_smoke.cpp`.
- HIL scenarios: `test/hil/scenarios/*`.

## Execution

- Build firmware and run host suite: `scripts/test/run_all.sh`.
- Host suite focuses on deterministic logic and rejection policy correctness.
- Embedded suite validates NVS/build integration and runtime object availability.
- HIL suite validates end-to-end CLI/HTTP/MQTT behavior against physical hardware.

## Result policy

- Contract failures stop suite quality score for full execution.
- Infrastructure failures are retried once when configured in `hil_config.example.yaml`.
- HTTP, MQTT, and serial transport issues are separated as infra errors in the JSON summary.

## Artifacts

- Summary JSON is emitted to `.test-artifacts/run_hil_<timestamp>.json` (or printed by script and captured by CI/local runner).
- Per-suite logs are available under `.test-artifacts/*.log`.
