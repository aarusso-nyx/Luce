# Firmware Test Workflow

LUCE test policy is firmware-only on real hardware.

## Canonical target

- PlatformIO environment: `luce_net1`

## Smoke test command

- `scripts/test_firmware_net1.sh`

What it does:

1. Uploads `luce_net1` firmware to the board.
2. Captures serial output for a bounded duration.
3. Verifies boot markers:
   - `LUCE STRATEGY=NET1`
   - `Feature flags: NVS=1 I2C=1 LCD=1 CLI=1 WIFI=1 NTP=1 mDNS=1 MQTT=1 HTTP=1`

## Required environment

- ESP32 board connected.
- `LUCE_UPLOAD_PORT` set if default upload port does not match.
- `LUCE_MONITOR_PORT` set if default monitor port does not match.

## Evidence output

- Logs are written to: `docs/work/diag/<timestamp>/test/`

## Test policy note

This repository uses firmware-only validation on real hardware.

Native host tests and stubs were removed.

## Canonical Test Path

- Build firmware: `pio run -e luce_net1`
- Flash + capture + assert boot markers:
  - `scripts/test_firmware_net1.sh`

## Preconditions

- ESP32 board connected via serial.
- Upload and monitor ports exported as needed:
  - `LUCE_UPLOAD_PORT` (default: `/dev/cu.usbserial-0001`)
  - `LUCE_MONITOR_PORT` (default: `/dev/cu.usbserial-40110`)
