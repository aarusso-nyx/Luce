# Firmware Test Workflow

LUCE test policy is firmware-only on real hardware.

## Canonical target

- PlatformIO environment: `net1`

## Smoke test command

- `python3 scripts/test_layers.py --layers boot --env net1 --boot-duration 45`

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

This repository uses firmware-only validation on real hardware.

Native host tests and stubs were removed.

## Canonical Test Path

- Build firmware: `pio run -e net1`
- Flash + capture + assert boot markers:
  - `python3 scripts/test_layers.py --layers boot --env net1 --boot-duration 45`

## Full Contract Suite

Use the integrated layered test entrypoint to validate transport and lifecycle behavior:

- `python3 scripts/test_layers.py --layers all --env net1 --host https://<device-ip> --http-token <token> --tcp-token <cli-token> --ws-host <device-ip> --mqtt-host <broker-ip>`
- `python3 scripts/test_layers.py --layers mqtt --spawn-test-mqtt-broker --mqtt-topic luce/net1` (ephemeral Python broker, no Docker)

Dependencies:

- `python3 -m pip install -r tests/requirements.txt`

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

- `docs/work/diag/<run_id>/test-layers/<layer>.log`
- `docs/work/diag/<run_id>/test-layers/junit-<layer>.xml` (pytest layers)
- `docs/work/diag/<run_id>/test-layers/summary.md`
- `docs/work/diag/<run_id>/test-layers/summary.json`

## Preconditions

- ESP32 board connected via serial.
- Upload and monitor ports exported as needed:
  - `LUCE_UPLOAD_PORT` (default: `/dev/cu.usbserial-0001`)
  - `LUCE_MONITOR_PORT` (default: `/dev/cu.usbserial-40110`)
