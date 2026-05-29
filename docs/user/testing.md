# Firmware Test Workflow

LUCE uses hardware-free checks for pure logic and hardware-backed evidence for
firmware behavior.

## Canonical target

- PlatformIO environment: `net1`

## Smoke test command

- `./scripts/luce.sh test --layers boot --env net1 --boot-duration 45`

What it does:

1. Uploads `net1` firmware to the board.
2. Captures serial output for a bounded duration.
3. Verifies boot markers:
   - `LUCE STRATEGY=NET1`
   - `Feature flags: NVS=1 I2C=1 LCD=1 CLI=1 WIFI=1 NTP=1 mDNS=1 MQTT=1 HTTP=1`

## Required environment

- ESP32 board connected.
- `LUCE_UPLOAD_PORT` set if default upload port does not match.
- `LUCE_MONITOR_PORT` set if default monitor port does not match.

## Evidence output

- Logs are written to: `docs/work/diag/<run_id>/test-layers/`

## Test policy note

Native host tests cover pure C++ helpers without ESP32 hardware. They do not
replace firmware, serial, relay, button, LCD, I2C, or network contract evidence.

## Canonical Test Path

- Hardware-free pure logic: `scripts/run_host_tests.sh`
- Build firmware: `./scripts/luce.sh build --env net1`
- Flash + capture + assert boot markers:
  - `./scripts/luce.sh test --layers boot --env net1 --boot-duration 45`

## Full Contract Suite

Use the integrated layered test entrypoint to validate transport and lifecycle behavior:

- `./scripts/luce.sh test --layers all --env net1 --host https://<device-ip> --http-token <token> --tcp-token <cli-token> --ws-host <device-ip> --mqtt-host <broker-ip>`
- `./scripts/luce.sh test --layers mqtt --spawn-test-mqtt-broker --mqtt-topic luce/net1` (ephemeral Python broker, no Docker)

The wrapper delegates to `python3 scripts/test_layers.py` and writes a wrapper log beside the layered test outputs.

Selected layers fail loudly when prerequisites are missing. Missing HTTP/TCP
tokens, serial ports, broker connectivity, or Python dependencies are reported
as layer failures in `summary.md` and `summary.json`; they are not silent skips.
Skips are reserved for layers that were not selected, and simulator-unsupported
layers are reported as `DESELECTED`.

Dependencies:

- `python3 -m venv .venv`
- `.venv/bin/python -m pip install -r tests/requirements.txt`

Homebrew-managed Python may reject system installs. Use the repo-local `.venv` path above for local contract-test dependencies.

## Host Unit Tests

Run from the repository root:

```sh
scripts/run_host_tests.sh
```

The host lane builds native C++17 tests for `id_set_parser`, `relay_logic`,
`backoff`, `json_writer`, `str_utils`, `nvs_helpers` status handling, PKI pure
helpers, HTTP route decisions, and LED manual-mode parsing. It writes
`summary.json`, JUnit XML, and logs under
`docs/work/diag/<run_id>/host-tests/`.

## Wokwi Simulated Lane

The simulator lane is supplementary regression coverage. It does not replace
hardware-backed release evidence because relay, button, LCD, and I2C peripheral
behavior is not modeled in this lane.

Run the local Wokwi lane:

- `./scripts/luce.sh test --target wokwi --layers build,boot,http,tcp,ws,mqtt --env net1 --spawn-test-mqtt-broker`

Prerequisites:

- `wokwi-cli` on `PATH`
- `WOKWI_CLI_TOKEN` set
- Python dependencies from `tests/requirements.txt`

Simulator behavior:

- `boot` runs Wokwi, captures serial output, checks NET1 boot markers, and runs a UART CLI smoke scenario for non-I2C commands.
- Current `wokwi-cli` validates `build` and `boot`; `http`, `tcp`, `ws`, and `mqtt` are recorded as `DESELECTED` with `deselected:wokwi` by default because the CLI does not attach to the Wokwi Private IoT Gateway for incoming forwarded ports.
- `--wokwi-network-mode gateway` is available as an experimental local attempt using `wokwigw`, but hardware-backed transport tests remain the canonical protocol evidence.
- Generated simulator NVS, TLS material, Wokwi project files, merged firmware, serial logs, and pytest evidence are written under `docs/work/diag/<run_id>/`.
- Runtime simulator tokens default to `LUCE_SIM_HTTP_TOKEN` or `luce-token`, and `LUCE_SIM_CLI_TOKEN` or `luce-cli`.
- `LUCE_SIM_WOKWI_TIMEOUT_MS` controls the Wokwi CLI simulation timeout.

Modules in the suite:

- Runner-native layers:
  - `build` (PlatformIO compile for selected env)
  - `boot` (upload + serial capture + marker assertions)
- Pytest contract layers:
  - `tests/test_http_contract.py` (auth/method/payload + LED + OTA-check routes)
  - `tests/test_tcp_cli_contract.py` (AUTH, fail-limit disconnect, readonly enforcement)
  - `tests/test_ws_contract.py` (`/ws` handshake + snapshot payload contract)
  - `tests/test_mqtt_contract.py` (compat unsupported responses, control paths, config persistence/reconnect scenarios)
  - `tests/test_serial_cli_contract.py` (serial lifecycle reboot markers + serial CLI parser matrix)

Notes:

- `build` and `boot` are executed by `scripts/test_layers.py`; they are not pytest modules.
- Running `pytest` alone validates contract layers only.

Outputs:

- `docs/work/diag/<run_id>/host-tests/summary.json`
- `docs/work/diag/<run_id>/host-tests/junit.xml`
- `docs/work/diag/<run_id>/test-layers/<layer>.log`
- `docs/work/diag/<run_id>/test-layers/junit-<layer>.xml` (pytest layers)
- `docs/work/diag/<run_id>/test-layers/summary.md`
- `docs/work/diag/<run_id>/test-layers/summary.json`
- `docs/work/diag/<run_id>/test/test.txt` when invoked through `scripts/luce.sh test`

## Preconditions

- ESP32 board connected via serial.
- Upload and monitor ports exported as needed:
  - `LUCE_UPLOAD_PORT` (default: `/dev/cu.usbserial-0001`)
  - `LUCE_MONITOR_PORT` (default: `/dev/cu.usbserial-40110`)
