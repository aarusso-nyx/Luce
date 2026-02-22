# Runtime State and Policy

## Config vs Runtime State

The store is separated into two logical blocks:

- `AppConfigData`: persisted parameters (features, network, relay, logger, OTA, security, sensors).
- `RuntimeData`: volatile telemetry and computed policy state (net state, sensor sample, uptime/heap snapshots, mask values).

## Canonical Masks

- `desired_mask`: requested relay pattern after policy intent and configuration.
- `effective_mask`: mask currently enforced after applying automation and physical overrides.
- `night_mask`: configured night profile bitset.
- `light_threshold`: threshold used against LDR value.

## Physical Inputs and Overrides

The policy layer uses:

- `policy_button_authority` to freeze remote intent for relays recently touched by button.
- `override_until_ms[]` (per relay) to keep manual actions authoritative for the configured TTL.
- `policy_compute_automation()` to combine sensor light data and day/night mode with persistent mask configuration.

## Sensor-Derived Day/Night

- Day mode is `light >= threshold`.
- Night mode applies `night_mask` constraints.
- Any automation change drives a full policy recompute.

## Health Snapshot

`HealthSnapshot` includes:

- per-service state
- restart counts
- heartbeat age
- queue depths and HWM
- last fault code
- degraded reason per service

### Service States

- `STOPPED`, `STARTING`, `RUNNING`, `DEGRADED`, `FAILED`

Used by:

- supervisor for escalation decisions
- `/api/health`
- LED/error signaling policy through supervisor layer.
