# HIL Setup

## Required items

- ESP32 DUT flashed with Luce firmware.
- Relay/button harness with deterministic mapping.
- Serial access path (for telnet/CLI checks).
- MQTT broker reachable from host.
- Optional HTTP endpoint to the DUT.

## Environment

- Configure `test/hil/hil_config.example.yaml` for DUT serial, HTTP base, and broker.
- If broker is unavailable, MQTT scenarios should fail as infra failures and be retried by runner policy.
- A matching CLI token/password is required when DUT has auth enabled; otherwise auth-focused checks are expected to fail fast and report contract failures.

## Bootstrapping

1. Confirm DUT firmware boot and `/api/health` reachable.
2. Confirm HIL credentials are aligned with DUT policy configuration.
3. Execute `scripts/test/run_all.sh`.
