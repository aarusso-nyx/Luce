# Build and Test Reference Notes

## Build Entry

- `platformio.ini` defines the `nodemcu-32s` environment.
- `src/CMakeLists.txt` gathers all `*.cpp` files in `src/`.

## Test and Validation Files

- Host/device test harness materials and references are under `test/` and `test/fixtures/`.
- HIL/contract artifacts are documented under `doc/testing/`.

## Runtime Verification Points

Use these paths when auditing code-level correctness:

- code/docs parity and coverage health:
  - `docs/governance/audit/code-docs.md`
  - `docs/governance/compliance/scorecard-2026-02-20.md`
- CLI/API/MQTT contract parity:
  - `doc/API.md`
  - `doc/MQTT.md`
  - `doc/CLI.md`

## Operational Verification

- Health endpoint for live service state and queue metrics.
- Event stream for state transition observability.
- LED statuses and supervisor error level as runtime indicators.
